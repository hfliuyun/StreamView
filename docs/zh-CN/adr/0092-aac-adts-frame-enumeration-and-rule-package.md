# ADR-0092: AAC ADTS 帧枚举机制与官方规则包

## 状态

已接受 (Accepted)

## 背景

StreamView 实施计划阶段 4 引入对 AAC-LC 音频（ISO/IEC 14496-3:2019, 第 5 版）的正式结构支持。原始 AAC 音频码流的主要传输封装格式为音频数据传输流（ADTS, Audio Data Transport Stream），规范定义见 ISO/IEC 14496-3 第 1 部分附录 1.A（Annex 1.A）。

对格式规则语言与执行管线的首轮探测揭示了关键的架构拦截与范式差异：
1. **DSL 扫描器闭集拦截**：在 `src/rules/dsl.cpp:3453-3457` 与 `src/rules/dsl_ir.cpp:3568-3574` 中，序列扫描声明（`sequence<T> seq = scan(scanner_name)`）硬编码严格限制 `scanner_name == "h264_start_code"`，任何其他扫描器名称均被拦截并抛出错误 `DslDiagnosticCode::UnsupportedScanner: "Only h264_start_code is supported"`。
2. **分帧范式核心差异**：
   - **H.264 Annex B**：由 3 字节或 4 字节起始码定界（`0x000001` / `0x00000001`）。由于 NAL 单元头中不声明载荷长度，NAL 单元边界严格依赖向前线性扫描寻找下一个起始码。
   - **AAC ADTS**：由 12 位同步字 `0xFFF`（字节 0 为 `0xFF`，字节 1 高 4 位为 `0xF0`）定界。至关重要的一点是，7 字节（或存在 CRC 时的 9 字节）ADTS 头部内显式携带 13 位字段 `aac_frame_length`（ISO/IEC 14496-3 子条款 1.A.1），明确声明了整帧（包含头部、CRC 校验与原始数据块）的精确字节总长度 $L \in [7, 8192]$。
3. **会话与分析器耦合**：
   `streamview::app::AnalysisSession`（`src/app/analysis_session.cpp:138-144`）当前直接调用 `rules::detectH264AnnexBCandidate` 并硬编码实例化 `rules::H264AnnexBAnalyzer`。支持 AAC ADTS 需要泛化候选格式检测、规则目录匹配和 ADTS 分析执行入口。

## 决策

我们引入 ADTS 帧枚举架构，扩展 DSL 语法、类型系统、核心扫描管线与官方规则包目录：

### 1. ADTS 帧枚举与步进模型

ADTS 码流解析采用“长度链快速推进 + 状态机重同步”的混合步进机制：

1. **长度链步进（快速路径，每帧 $O(1)$）**：
   - 在候选字节偏移 $P$ 处读取 7 字节固定与可变头部；
   - 提取 $L = \text{aac\_frame\_length}$；
   - 确定当前帧源区间范围为 $[P, P + L)$；
   - 预测下一帧起始位置为 $P' = P + L$；
   - 在 $P'$ 处检验前 2 字节：验证 `(byte0 == 0xFF) && ((byte1 & 0xF6) == 0xF0)`（同步字 `0xFFF` 且 `layer == 0`）；
   - 验证通过时，直接推进至 $P'$，无需逐字节扫描。
2. **重同步状态机（慢速路径）**：
   - 当 $P'$ 处同步字失效、或遇到码流初始化/损坏时：
     - 向前逐字节搜索候选模式 `0xFF` 紧随 `0xF?`（`layer == 0`）；
     - 进行 2 帧前瞻复核：提取候选 $L_0$，检查偏移 $P + L_0$ 处的候选 $L_1$，并复核 $P + L_0 + L_1$；
     - 连续确认有效后，锁定同步状态并恢复长度链步进。
3. **截断与错误隔离**：
   - 若 $P + L > \text{source\_size}$，该帧作为部分/截断帧物化，并附带源定位诊断；
   - CRC 校验不匹配（当 `protection_absent == 0` 时）及字段超值域异常均作为非致命诊断发布，不中断长度链推进。

### 2. 语言与 IR 扩展

1. **扫描器标识符扩展**：
   - 在 `src/rules/dsl.cpp` 的 `scan(...)` 表达式中接纳 `adts_frame` 作为合法扫描器；
   - 在 `src/rules/include/streamview/rules/dsl_ir.h` 扩展 `DslScannerKind`：
     ```cpp
     enum class DslScannerKind : quint8 {
         H264StartCode,
         AacAdtsFrame,
     };
     ```
   - 在 `src/rules/dsl_ir.cpp` 将 `scan(adts_frame)` 降级为 `DslTypedScan` 且 `scanner = DslScannerKind::AacAdtsFrame`。
2. **ADTS 规则中的序列声明**：
   ```svfmt
   @index(progressive)
   sequence<AdtsFrame> frames = scan(adts_frame);
   entry frames;
   ```

### 3. 核心扫描器与格式探测管线

1. **`AacAdtsScanner`**：
   实现 `AacAdtsScanner`（`src/rules/include/streamview/rules/aac_adts_scanner.h` 与 `src/rules/aac_adts_scanner.cpp`），批量产生包含精确 `frameSpan`、`headerSpan`、`payloadSpan`、`crcPresent` 与 `aacFrameLength` 的 `AacAdtsRecord` 批次。
2. **`AacAdtsDetector`**：
   实现 `detectAacAdtsCandidate`（`src/rules/include/streamview/rules/aac_adts_detector.h` 与 `src/rules/aac_adts_detector.cpp`），在首个 64 KiB 页面中探测有效 ADTS 同步字链并计算置信度。
3. **多格式会话选择入口**：
   扩展 `AnalysisSession` / `SessionDocument`，依据 `RulePackageCatalog` 中的可用规则包对源文件执行格式探测器多路匹配，实例化对应的格式分析执行器（`H264AnnexBAnalyzer` 或 `AacAdtsAnalyzer`）。

### 4. 官方 AAC 规则包 (`org.streamview.aac`)

1. **清单文件**（`src/rules/official/org.streamview.aac/rule.toml`）：
   ```toml
   manifest-version = 1

   [package]
   id = "org.streamview.aac"
   version = "0.1.0"
   authors = ["StreamView contributors"]
   license = "MIT"
   dependencies = []

   [compatibility]
   language = "0.1"
   engine = ">=0.1.0 <0.2.0"

   [[entrypoints]]
   id = "adts"
   format = "audio.aac.adts"
   source = "src/aac_adts.svfmt"
   profiles = ["lc"]
   depth = "adts-frame"
   detector = "aac-adts"
   ```
2. **规范引用基线**：
   所有字段与枚举严格对照 ISO/IEC 14496-3:2019（第 5 版）：
   - 表 1.11 — `AacProfile`（MPEG-4 音频对象类型减 1：`0` Main, `1` LC, `2` SSR, `3` LTP / 在 MPEG-2 AAC 中保留）
   - 表 1.16 — `AacSamplingFrequencyIndex`（`0` 96000 Hz .. `12` 7350 Hz, `15` 显式）
   - 表 1.17 — `AacChannelConfiguration`（`0` 自定义/PCE, `1` 单声道, `2` 立体声, `3` 3声道, `4` 4声道, `5` 5声道, `6` 5.1, `7` 7.1）
   - 子条款 1.A.1 — `adts_fixed_header`, `adts_variable_header`
   - 子条款 1.A.2 — `adts_error_check` (`crc_check`)

### 5. 边界约定与值域定级

1. **单原始数据块限定**：首版限定 `number_of_raw_data_blocks_in_frame == 0`（每帧 1 块）；多块结构及其块间 16 位 CRC 头部（`raw_data_block_position`）显式延期。
2. **值域定级与诊断策略**：
   遵循 ADR-0040 二分法（不影响头部布局的字段发出非致命警告而非中断解码，同时依据 ADR-0059 先例避免使用致命拦截的闭集 `@enum`）：
   - `sampling_frequency_index`：声明为 `@range(0, 12)`。非标值 13、14 及转义值 15（在 ADTS 中依据 ISO/IEC 14496-3 子条款 1.A.1 属非法值）统一发出非致命超范围诊断，不中断帧解码；
   - `profile`：声明为完整 4 值枚举 `enum AacProfile { main = 0; lc = 1; ssr = 2; ltp = 3; }`，注明当 `id == 1`（MPEG-2 AAC）时 `3` 为保留值；
   - `channel_configuration`：保留完整 8 值枚举 `enum AacChannelConfiguration`（`0` 自定义/PCE .. `7` 7.1）。`channel_configuration == 0` 指示载荷内包含程序配置元素（PCE），在任务 T18 实现；
   - `adts_buffer_fullness`：`0x7FF` 为合法规范特殊值，表示可变码率（VBR）码流。
3. **最小帧长源定位断言**：
   为防止码流损坏导致 `aac_frame_length` 小于头部字节数，规则通过 ADR-0090 布尔算术与源定位断言表达最小帧长检查：
   ```svfmt
   computed<u64> minimum_frame_length =
       (protection_absent == 1) * 7 +
       (protection_absent == 0) * 9;
   assert(aac_frame_length >= minimum_frame_length) at aac_frame_length;
   ```
   该语法已在 `scratch/probe_adts.svfmt` 经 `svtool rule check` 实测通过（`Rule OK`）。

## 分阶段实施序列

- **任务 T14（当前）**：架构探测报告与双语 ADR-0092 规范起草（Markdown-only）。
- **任务 T15a**：ADTS 帧枚举能力切片（`DslScannerKind::AacAdtsFrame`、`AacAdtsScanner`、`dsl.cpp`、`dsl_ir.cpp`、能力单元测试，纯能力层）。
- **任务 T15b**：ADTS 分析执行器与应用集成切片（`AacAdtsAnalyzer`、`detectAacAdtsCandidate`、`AnalysisSession` 多态选择，测试覆盖 H.264 零回归 / AAC 源正确选择 / 未知源行为不变 / `resolvedRule` 双路径）。
- **任务 T16**：规则包创建（`org.streamview.aac` v0.1.0）与 ADTS 头部结构化解码（ADR-0093）。
- **任务 T17**：AudioSpecificConfig (ASC) 与 GASpecificConfig 结构化解码（v0.1.1, ADR-0094）。
- **任务 T18**：Program Config Element (PCE) 与非 LC Profile 诊断（v0.1.2, ADR-0095）。

## 参考文档

- ISO/IEC 14496-3:2019, Edition 5, Subclauses 1.A.1, 1.A.2, Tables 1.11, 1.16, 1.17
- ADR-0010: C-Style Declarative Format Description Language
- ADR-0016: TOML Manifest And ZIP Rule Packages
- ADR-0027: Resume Cancelled Progressive H.264 Indexes In Place
- ADR-0030: Canonical Rule Package Identity and Catalog
- ADR-0040: Report Unsigned Exp-Golomb Range Violations Without Stopping Decoding
- ADR-0059: Add Bounded P-Slice Reference Picture List Modification Loop
- ADR-0090: Boolean Operands In Arithmetic Expressions
- ADR-0091: Strict Source Range Validation For Empty Slice Payload

## 条款引用更正

后续规范审查（任务 T17d）厘清了 ISO/IEC 14496-3:2019（第 5 版）的子条款归属：
1. `adts_fixed_header` 与 `adts_variable_header` 规范定义位于第 1 部分附录 1.A 的子条款 **1.A.1**（*Fixed and variable header of ADTS*），而非子条款 1.6.2.1（后者为 `AudioSpecificConfig`）；
2. `adts_error_check`（`crc_check`）规范定义位于第 1 部分附录 1.A 的子条款 **1.A.2**（*Error detection*）。

正文表述与参考资料已同步更正。


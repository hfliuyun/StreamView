# ADR-0095：AAC raw_data_block 压缩载荷与 Profile 处理

- **状态**：Proposed
- **日期**：2026-08-16
- **作者**：StreamView 贡献者
- **领域**：DSL 规则 / AAC 解码 / 分析器执行

## 背景

StreamView 实施计划阶段 4 规定了对 AAC-LC 音频（ISO/IEC 14496-3:2019, 第 5 版）的完整结构支持。在完成 ADTS 头结构化解码（ADR-0093，任务 T16）与 AudioSpecificConfig / Program Config Element 解码（ADR-0094，任务 T17c）之后，阶段 4 需完成最后三项目标（`docs/implementation-plan.md:206-208`）：
1. 将 `raw_data_block` 整体标记为压缩载荷区域，不隐藏实现 Huffman 解码；
2. 形式化处理并报告 HE-AAC、ELD 及其他 AAC profile；
3. 针对 ADTS 头部、ASC/PCE、截断码流、CRC 字段布局/截断以及 profile 约束进行逐 bit 验收审计。

### 探测实测结论

为在修改代码前确立确定性事实，通过独立探针验证了以下技术观察：

1. **DSL 语法表达能力与最小字段足迹（N1）**：
   在 `src/rules/official/org.streamview.aac/src/aac_adts.svfmt` 中，`AdtsHeader` 已声明：
   ```dsl
   computed<u64> minimum_frame_length = (protection_absent == 1) * 7 + (protection_absent == 0) * 9;
   assert(aac_frame_length >= minimum_frame_length) at aac_frame_length;
   ```
   直接复用 `minimum_frame_length` 追加 lazy 字节区域：
   ```dsl
   computed<u64> raw_data_block_bytes = aac_frame_length - minimum_frame_length;
   @lazy(raw_data_block_bytes) bytes raw_data_block
       @spec("ISO/IEC 14496-3:2019", "4.5.2.1")
       @description("Raw audio data payload block.");
   ```
   经 `svtool rule check` 实测通过（`Rule OK`，rc=0）。复用 `minimum_frame_length` 避免了引入冗余的 `header_bytes` 计算节点，且避免了 7/9 头部大小公式的双重维护。条件 `crc_check`（16 位）严格保持字节对齐，且 `@lazy` 位于结构末尾，完全符合 `docs/zh-CN/format-language/README.md:443-445` 中的字节边界对齐规则。

以下观察记录的是 T18 实施前的探测快照，仅作为决策历史保留；当前实现以“决策”和后续修订为准。

2. **分析器视图映射为真实阻塞项与 T18b 独立性（T18 前）**：
   探测时 `AacAdtsAnalyzer` 构造的 `SourceMapping` 仅覆盖 `*record.headerSpan`（7 或 9 字节）。当 `AdtsHeader` 包含 `@lazy(raw_data_block_bytes)` 时，以 20 字节真实 ADTS 帧在 7 字节头部视图下执行，会失败并报 `DslExecutionStatus::TruncatedSource` 与诊断 `Lazy byte region exceeds the available source range`。
   当源映射扩展至完整帧跨度（`*record.frameSpan`，20 字节）时，DSL VM 成功物化该帧，`raw_data_block` 处于 `MaterializationState::Lazy` 状态（无 CRC 时比特偏移 56，长度 104；有 CRC 时比特偏移 72，长度 88）。
   至关重要的是，将执行器源映射扩展为 `*record.frameSpan` 对当时的官方规则包（未包含 lazy 载荷的 `org.streamview.aac` 0.1.2）是严格的 no-op：7 字节头视图、20 字节整帧视图与 15 字节截断视图均完全一致地产出 `status=0 materialized=1`。

3. **截断语义契约转移与统一**：
   当 ADTS 帧在文件尾发生截断（例如声明 20 字节但在文件中仅有 15 字节）时，DSL VM 在 `DslExecutor::decodeStruct` 注册 lazy 区域时命中 `bitCount > reader.remainingBits()`，发出 Error 级别的 `DslExecutionStatus::TruncatedSource`。
   该行为取代了原先在 C++ 层面合成的 Warning 级诊断（`ADTS frame payload is truncated at EOF`），使 ADTS 截断契约与全库统一的 DSL VM 截断契约（ADR-0092 §1.3）对齐。

4. **下溢防御机制**：
   若损坏帧声明 `aac_frame_length < 7`（或含 CRC 时 `< 9`），既有断言 `assert(aac_frame_length >= minimum_frame_length)` 会在 lazy 载荷求值前被执行，以 `InvalidSyntax`（`Assertion condition is false`）终止，防止发生负数/下溢的 lazy 字节分配。

5. **T18-R 前的 Profile 报告能力边界**：
   T18 前的审计确认，当时 DSL 体系内不存在发出 `MaterializationState::Unsupported` 或 `DiagnosticCode::UnsupportedSyntax` 的机制；T18-R 已用 `unsupported("reason") at field;` 取代这一限制：
   - `@enum` 违规过去和现在都是致命的（`DiagnosticCode::InvalidSyntax`，Severity: Error，`Field value is not declared by its enum type`），因此不能替代“不支持能力”的报告；
   - 非连续取值集合（例如 AOT 2, 5, 29, 39）无法通过 `@range` 表达：parser 与 IR 校验要求恰好两个整数参数，且会拦截单个字段上的重复 `@range` 注解；
   - 在 ADTS 头中，2 位的 `profile` 字段（ISO/IEC 14496-3 表 1.A.1）仅能编码 0..3（Main, LC, SSR, LTP），无法直接编码 AOT 5 (SBR)、29 (PS) 或 39 (ELD)。标准广播码流中，ADTS 内传输的 HE-AAC 码流在 ADTS 头中一律声明 `profile = 1` (LC)，并在 `raw_data_block` 内部携带 SBR/PS 扩展。

6. **未识别注解被静默忽略（N2，历史）**：
   探测发现当时的 compiler 会静默忽略已声明 bit 字段上的未识别注解。例如 `bits<12> syncword @equalss(4095);` 可通过 `Rule OK`，而拼写正确的 `@equals(4095)` 会正确发出 `InvalidSyntax` / `Field value violates @equals constraint`。Task P5b 后续以统一注解注册表与宿主白名单关闭了 N2；本段只保留为推动该闸门的探测证据。

---

## 决策

### 1. `raw_data_block` 规范条款号归属（B2, C4）

依据 ISO/IEC 14496-3:2019（第 5 版）：
- **ADTS 载荷编排归属**：第 1 部分附录 1.A 子条款 **1.A.1**（*Fixed and variable header of ADTS*）及表 1.A.5（`adts_frame()`），其中 `adts_frame()` 依次编排 `adts_fixed_header()`、`adts_variable_header()`、可选 `adts_error_check()` 以及 `raw_data_block()`；
- **Raw Data Block 语法定义**：第 4 部分（General Audio）子条款 **4.5.2.1**（*raw_data_block* / *Syntactic elements*），定义了 `raw_data_block()` 的语法元素组成（SCE、CPE、LFE、DSE、PCE、FIL、TERM）。

*规范核实说明*：由于仓库不包含 ISO/IEC 规范正文（`docs/standards.md:3-4`），条款号 `4.5.2.1` 与表 `1.A.5` 系依据 ISO/IEC 14496-3 结构体系与二次文献推导，在此明确标注为未经规范正文核实。能确认的最小规范范围为第 1 部分附录 1.A（ADTS 传输分帧）与第 4 部分（通用音频语法）。

为保持严格的标准解耦，`raw_data_block` 字段分配 `@spec("ISO/IEC 14496-3:2019", "4.5.2.1")`，以清晰区分载荷自身的语法定义与 `1.A.1` 固定/可变头条款。

### 2. `raw_data_block` DSL 规则定义（N1）

在 `src/rules/official/org.streamview.aac/src/aac_adts.svfmt` 的 `AdtsHeader` 结构末尾追加以下字段定义：

```dsl
    computed<u64> raw_data_block_bytes = aac_frame_length - minimum_frame_length;
    @lazy(raw_data_block_bytes) bytes raw_data_block
        @spec("ISO/IEC 14496-3:2019", "4.5.2.1")
        @description("Raw audio data payload block.");
```

### 3. 分析器帧视图映射扩展（能力切片 T18b）

在 `src/rules/aac_adts_analyzer.cpp:346` 中，将逻辑视图构造由 `{*record.headerSpan}` 调整为 `{*record.frameSpan}`。
扫描器（`src/rules/aac_adts_scanner.cpp:134-148`）已将 `record.frameSpan` 约束于 `availableBytes`。映射整帧跨度使得 DSL VM 能够在解析头部字段后立即将 lazy 载荷区域注册在实际帧载荷字节上。

### 4. 统一截断契约决策与测试迁移（C1, C2, N5）

采纳全库通用的 DSL VM 截断契约（方案 A）：
- 当码流在帧载荷中间意外结束时，DSL VM 尝试分配 `raw_data_block_bytes` 并检测到 `bitCount > reader.remainingBits()`；
- DSL VM 发出 Error 级 `DslExecutionStatus::TruncatedSource`，诊断消息为 `Lazy byte region exceeds the available source range`；
- 帧节点被标记为 `Invalid`，但保留已解码的头部子字段及其源码锚定位置；
- `aac_adts_analyzer.cpp:456-467` 处的 C++ 合成 Warning 诊断被 VM 原生诊断取代。

**任务 T18c 需同步迁移的测试套件清单**：
- `tests/rules/aac_adts_analyzer_test.cpp:141-187`（`createsAnalyzerFromBundledPackageAndDecodesFieldsViaDsl` 帧 0）：子节点数（第 144 行）由 16 推进至 18；`expectedNames0` 追加 `raw_data_block_bytes` 与 `raw_data_block`；
- `tests/rules/aac_adts_analyzer_test.cpp:189-230`（`createsAnalyzerFromBundledPackageAndDecodesFieldsViaDsl` 帧 1）：子节点数（第 196 行）由 17 推进至 19；`expectedNames1` 追加 `raw_data_block_bytes` 与 `raw_data_block`；
- `tests/rules/aac_adts_analyzer_test.cpp:325-365`（`handlesPayloadTruncationAtEof`）：
  - 第 356 行：`DiagnosticSeverity::Warning` $\to$ `DiagnosticSeverity::Error`；
  - 第 357 行：`"ADTS frame payload is truncated at EOF"` $\to$ `"Lazy byte region exceeds the available source range"`；
  - 第 364 行：`header1->state() == MaterializationState::Materialized` $\to$ `header1->state() == MaterializationState::Invalid`；
  - 第 360 行：`node1->children().size() == 1` 保持不变。
- `tests/rules/aac_adts_analyzer_test.cpp:602`（`resolvesAscEntryPointFromBundledRulePackage`）：`loaded.package->identity().packageVersion()` 断言由 `"0.1.2"` 同步升级为 `"0.1.3"`。

*死代码清理、有条件可达性论证与行为收窄（G3，任务 T18c-2）*：
在 `src/rules/aac_adts_analyzer.cpp` 中，已彻底删除原载荷截断合成 Warning 分支（`if (record.truncated) { ... }`）。

1. **有条件可达性论证（官方包满足）**：三路径论证成立的严格前提是「生效规则声明了覆盖帧剩余字节的 `@lazy` 区域」（官方包 `org.streamview.aac` 0.1.3 满足该前提）：
   - **路径 A（DSL 执行失败隔离）**：在 `publishRecord` 中，DSL 执行失败（`!execution.materialized()`）时，第 426 行将 `frameNode` 标记为 `Invalid` 并附带 VM 诊断，加入 `batch.frameNodes`，随后第 452 行直接 `return true`（致命错误则更早返回 `false`），控制流绝不落入后续语句。
   - **路径 B（VM 截断判定的算术必然性）**：截断发生时 `availableBytes < aac_frame_length`（`src/rules/aac_adts_scanner.cpp:143`）。由于 `raw_data_block_bytes = aac_frame_length - minimum_frame_length`，在读取完 `minimum_frame_length` 字节头部后，Reader 剩余字节为 `availableBytes - minimum_frame_length < raw_data_block_bytes`。因此 `src/rules/dsl_vm.cpp:3249-3252` 中的 `bitCount > reader.remainingBits()` 判定无条件成立，恒定返回 `DslExecutionStatus::TruncatedSource`（`"Lazy byte region exceeds the available source range"`），进而无条件触发路径 A。（边界注意：当 `availableBytes == aac_frame_length` 时，scanner 第 140 行走非截断分支，`record.truncated` 为 false）。
   - **路径 C（守卫条件恒真不变式）**：在 `AacAdtsScanner` 中，任何产出的 record 都必须满足 `offset + 7 <= sourceSize`（`src/rules/aac_adts_scanner.cpp:57-65`），确保 `availableHeader = min(availableBytes, headerLength) >= 7` 字节（`src/rules/aac_adts_scanner.cpp:144`）。因此 `record.headerSpan->bitLength() >= 56 > 0` 恒真，证实第 329 行的 `record.headerSpan && record.headerSpan->bitLength() > 0` 守卫绝不会短路绕过 DSL 执行。
2. **自定义/第三方规则的行为收窄后果**：对不满足该条件的规则（未声明载荷区域的自定义包，准入门见 `src/app/analysis_session.cpp:144-147`），载荷截断的尾帧将以 `Materialized` 且无诊断呈现，其逻辑范围被 clamp 至实际可用字节数。该已知能力边界由回归测试 `materializesTruncatedTrailingFrameWhenRuleLacksPayloadDeclaration`（`tests/rules/aac_adts_analyzer_test.cpp:379-475`）严格钉住。
3. **架构理由**：被删代码块在 C++ 中硬编码合成格式专属诊断，违反「格式语义只能进 DSL/规则」全局禁令；截断上报的正当机制是规则声明载荷区域后由 VM 通用契约产生（ADR-0092 §1.3）。
4. **`record.truncated` 现状**：`record.truncated` 在 `src/` 内已无读取点，作为 scanner 层扫描事实保留，scanner 测试套件（`tests/rules/aac_adts_scanner_test.cpp`）继续断言。

### 5. Profile 处理与明确能力边界

1. **ADTS 传输流**：通过 `@enum(AacProfile)` 约束于 `AacProfile`（0=Main, 1=LC, 2=SSR, 3=LTP）。在 ADTS 中传输的 HE-AAC v1/v2 码流依据广播规范在 ADTS 头中均携带 `profile = 1` (LC)，作为 LC 帧正常解码，其 raw data 载荷保持未解析状态。
2. **AudioSpecificConfig (ASC)**：先解码 ASC 公共前缀。AOT 5（SBR）、AOT 29（Parametric Stereo）以及外层 AOT 31 的转义编码（包括 AOT 39）随后执行 `unsupported("reason") at audio_object_type;`。结构保留前缀，转为 `MaterializationState::Unsupported`，产生 `DiagnosticCode::UnsupportedSyntax`，并在解释任何 GA/PCE 后缀之前停止。
3. **未声明载荷区域的 ADTS 规则**：匹配 `audio.aac.adts` 格式但仅声明头部字段、未声明 `@lazy` 载荷区域的规则包无法获得载荷截断上报。载荷截断的尾帧将呈现为有效物化且零诊断，其比特长度被 clamp 至实际可用字节数。未来若要通用化区域级截断探测，必须通过格式中立的核心机制（例如在 scanner 标记截断时，将容器 region 节点置为部分物化并携带通用截断诊断）而非在 C++ 中合成格式专属文案。
4. **阶段 4 第 4 项正式闭环判定**：格式中立的 DSL/IR/VM 现已提供 `unsupported("reason") at field;` 及对应执行/会话状态。AAC analyzer 将其作为内容层结果，发布 Unsupported 子树并可继续扫描后续 ADTS 帧。官方 AAC 包版本为 `0.1.4`；SBR、PS 与 ELD 的专属 SpecificConfig 解码仍不在声明子集内。在官方 AAC 规则包（`profiles = ["lc"]`）中，声明 `audio_object_type == 31`（转义 AOT）的码流在 `audio_object_type` 处一律无条件发出 `unsupported("Escaped AAC object types require a profile-specific SpecificConfig")`；所有转义 AOT 取值（AOT 32, 34, 40, 41, 42 等）均统一判定为 Unsupported，不进行逐值分支细分解码。

### 6. 逐 bit 验收审计范围与覆盖矩阵（B1, C1, C3）

实施计划阶段 4 第 5 项（`docs/implementation-plan.md:208`）要求针对五大类进行逐 bit 验收。

#### CRC 字段布局与截断验收边界
StreamView 规则不进行 CRC-16 多项式除法或算术校验和计算。对 `number_of_raw_data_blocks_in_frame == 0`，验收边界严格限定为：
1. `protection_absent == 0`：16 位 `crc_check` 字段精确物化于 bit 偏移 56..71（字节 7..8）；
2. 字段存在时载荷从 bit 72 起；`protection_absent == 1` 时省略字段且载荷从 bit 56 起；
3. 16 位字段不完整时，在实际可用前缀上发出 `TruncatedSource`，且不崩溃；
4. 测试值 `0x1234` 只证明字段被原样解码，不将其分类为校验正确或错误。多 raw data block 与 CRC 算术校验需要单独 ADR 和格式中立的完整性检查 API。

#### 覆盖与缺口清单

| 类别 | 最终验证覆盖（`file:line`） | 审计状态与验证范围 |
| :--- | :--- | :--- |
| **1. ADTS 头部** | `tests/rules/aac_adts_analyzer_test.cpp:106`（`createsAnalyzerFromBundledPackageAndDecodesFieldsViaDsl`）及 `:2081`（`decodesAdtsHeaderBitByBitRangesAndZeroLengthPayload`）。 | **已关闭**：全面覆盖全部 18/19 个字段（包括 `original_copy`、`home`、`copyright_identification_bit`、`copyright_identification_start`）、bit 偏移 0..55/71、长度、数值、零长度载荷帧及非零 Lazy 载荷帧（断言位于 :2208 与 :2260）。 |
| **2. ASC / PCE** | `tests/rules/aac_adts_analyzer_test.cpp:2269`（`decodesAscAndPceFieldsBitByBit`）及既有正向/截断套件。 | **已关闭**：一个紧凑的 160-bit 向量直接验证全部可达 ASC/GASpecificConfig/PCE 源字段的值、逻辑范围、物化状态与空字段诊断，包括显式采样频率、core coder delay、全部 mixdown 字段、全部声道元素循环族、对齐位和 comment bytes。PCE 计算字段只验证值/状态，不虚构 source span。 |
| **3. 码流截断** | `tests/rules/aac_adts_analyzer_test.cpp:290`、`:325`、`:379`、`:477`、`:502` 及 `:2388`（`verifiesTruncatedFramesLogicalRangesAndDiagnosticLocations`）。 | **已关闭**：截断帧节点显式 `logicalRange()` 断言（15 字节载荷截断 [0, 120)，8 字节含 CRC 头部截断 [0, 64)）及诊断位置区间显式断言（载荷截断 [56, 120)，头部截断 [56, 64)）。 |
| **4. CRC 字段布局/截断** | `decodesAdtsHeaderBitByBitRangesAndZeroLengthPayload` 与 `verifiesTruncatedFramesLogicalRangesAndDiagnosticLocations`。 | **在上述边界内关闭**：验证字段出现/省略、bit 56..71 位置、载荷从 bit 56/72 起始及字段不完整时的截断；不作 CRC 正确性声明。 |
| **5. 不支持 Profile** | `reportsAscNonGaAot5SbrAsUnsupported`、`reportsAscNonGaAot29ParametricStereoAsUnsupported`、`reportsAscNonGaAot39EnhancedLowDelayAsUnsupported` 与 `reportsAscEscapedAudioObjectTypeAsUnsupported`。 | **已关闭**：每个用例都验证保留前缀的值/范围/状态、精确 Unsupported 诊断锚点，以及不存在伪造的 GA/PCE 后缀。标准 ADTS profile 0..3 仍由 `decodesAllStandardAdtsProfilesMainLcSsrLtp` 验证为 Materialized。 |

### 7. 阶段 4 任务切片与纪律规划

为保持严格的单职责提交与能力/规则解耦：

- **任务 T18a**：探测结论、双语 ADR-0095、实施计划记录（Markdown-only）；
- **任务 T18b**：分析器视图映射扩展至帧跨度（`src/rules/aac_adts_analyzer.cpp:346`），执行器能力切片，不改规则、不升包版本；
- **任务 T18c**：规则消费 `@lazy raw_data_block`（`aac_adts.svfmt`），包版本升级至 `0.1.3`，测试套件更新；
- **任务 T18c-2**：清理 `src/rules/aac_adts_analyzer.cpp:456-467` 死代码与可达性论证（执行器能力切片，不升版本）；
- **任务 T18d**：Profile 处理验证与文档对齐；
- **任务 T18e**：关闭类别 1、2、3、4、5 全部缺口、阶段 4 复选框全量勾选（`docs/implementation-plan.md:206-208`）、推进阶段至 Phase 5；
- **任务 T18f**：阶段 4 收尾校正：Markdown 卫生检查分号转义与行号校正、Frame 2/3 Lazy 状态断言补齐及 PCE 未抽样标量余量声明。

#### 测试引用防漂移纪律
为防止因测试用例增删改导致文档中的测试行号引用漂移失效：
1. **默认尾部追加**：新增测试用例默认一律追加至测试类的最末尾（已有用例的最后一个之后）。若有极充分理由必须在测试类中部插入，则必须在同一 commit 内全局检索并修正所有受影响文档中的行号引用。
2. **同 Commit 行号复核**：任何使被引用文件行号发生位移的 commit（含 `tests/` 与 `docs/` 自身），必须在提交前使用 `grep -n` 全量核验所有引用文档中的行号，发现漂移即刻就地修正。

---

## 验证矩阵与复现步骤（B4）

以下验证证据均通过自包含独立 C++ 探针在 `dev` 构建静态库上编译生成。

**复现命令模板**：
```bash
clang++ -std=c++20     -Isrc/rules/include -Isrc/core/include     -I/opt/homebrew/lib/QtCore.framework/Headers     -iframework /opt/homebrew/lib -F/opt/homebrew/lib -framework QtCore     build/dev/src/rules/libstreamview_rules.a build/dev/src/core/libstreamview_core.a     <probe_source.cpp> -o /tmp/probe_bin && /tmp/probe_bin
```

| 探针 / 验证项 | 输入源码与码流构造 | 实测输出 | 结论 |
| :--- | :--- | :--- | :--- |
| **P1: DSL 语法检查** | 复用 `minimum_frame_length` 追加 `@lazy` 载荷的完整 grammar | `Rule OK` (rc=0) | 语法有效；无需修改 DSL 编译器。 |
| **P2: 20 字节帧整帧映射（无 CRC）** | 20 字节 ADTS 帧（7 字节头 + 13 字节载荷），160 位映射 | `status=0 materialized=1, AdtsHeader state=6 children=18, child=raw_data_block state=0 bitOffset=56 bitLength=104` | 整帧视图解析出 18 个子节点，lazy 载荷位于 bit 56。 |
| **P3: 20 字节帧整帧映射（含 CRC）** | 20 字节 ADTS 帧（9 字节头 + 11 字节载荷），160 位映射 | `status=0 materialized=1, AdtsHeader state=6 children=19, child=crc_check state=6 bitOffset=56 bitLength=16, child=raw_data_block state=0 bitOffset=72 bitLength=88` | 含 CRC 帧解析出 19 个子节点，lazy 载荷位于 bit 72。 |
| **P4: 15 字节截断帧映射** | 声明 20 字节但在 15 字节处截断（120 位映射） | `status=1 materialized=0, AdtsHeader state=5 diags=1: code=0 sev=2 msg="Lazy byte region exceeds the available source range"` | 载荷截断触发 VM 原生 Error 级 TruncatedSource。 |
| **P5: 7 字节仅头部视图** | 20 字节 ADTS 帧在 56 位头部映射下执行 | `status=1 materialized=0, AdtsHeader state=5 diags=1: code=0 sev=2 msg="Lazy byte region exceeds the available source range"` | 证实当前执行器仅头部映射是真实阻塞项。 |
| **P6: T18b 视图映射 No-Op 验证** | 现有官方 `AdtsHeader`（无 lazy）分别在 56 位、160 位与 120 位视图下执行 | 三种视图输出完全一致：`status=0 materialized=1` | 证实 T18b 单独修改视图映射对既有规则输出完全无影响。 |
| **P7: 重复 @range 双闸门验证（N3）** | `bits<5> aot @range(0, 4) @range(23, 23);` | `diag code=14 msg="@range may appear at most once on a field"` | 证实单个字段不可并列多个 @range（`src/rules/dsl.cpp:2677` / `src/rules/dsl_ir.cpp:180`）。 |
| **P8: 多参数 @range 双闸门验证（N3）** | `bits<5> aot @range(2, 5, 29, 39);` | `diag code=14 msg="@range requires two integer arguments"` | 证实 @range 无法接收非连续离散值（`src/rules/dsl.cpp:2703` / `src/rules/dsl_ir.cpp:192`）。 |
| **P9: 拼写错误注解验证（N2）** | `bits<12> syncword @equalss(4095);` 作用于错误码流（`syncword=255`） | 错拼：`status=0 materialized=1 diags=0`<br>正拼 `@equals`：`status=2 materialized=0 diags=1 msg="Field value violates @equals constraint"` | 证实未识别注解被静默忽略。 |

---

## 参考资料

- ISO/IEC 14496-3:2019, Information technology — Coding of audio-visual objects — Part 3: Audio（第 1 部分附录 1.A，第 4 部分子条款 4.4.1, 4.4.1.1, 4.5.2.1）。
- [ADR-0040：非致命语法警告与范围注解](0040-non-fatal-syntax-warnings-and-range-annotations.md)
- [ADR-0092：AAC ADTS 分帧枚举与规则包架构](0092-aac-adts-frame-enumeration-and-rule-package.md)
- [ADR-0093：ADTS 头结构化解码与官方规则包](0093-adts-header-structured-decoding-and-official-rule-package.md)
- [ADR-0094：AudioSpecificConfig 与 Program Config Element 结构化解码](0094-audio-specific-config-and-program-config-element.md)

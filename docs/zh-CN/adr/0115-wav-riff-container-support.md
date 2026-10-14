# ADR-0115: WAV/RIFF 容器支持 —— 扫描器、探测器、官方规则包与激活

## 状态

已接受

## 日期

2026-10-14

## 背景

实施计划阶段 8 引入 WAV 音频文件的正式支持：采用 WAVE 表单类型的资源交换文件格式（RIFF）容器。WAV 是本项目第一个完全小端、且 chunk 链携带运行时载荷长度的格式，也是第一个以专用非 chunk 表单头（12 字节）开头的格式。

按 scratch 探测纪律，在决策前对当前语言与运行时做了三组探测：

1. **lazy 到小端的闸门**（`docs/format-language/README.md:618`）。
   `scratch/wav_probe2_chain.svfmt` 验证了运行时 `@lazy` 区域之后的大端
   `bits<32>` 字段可被接受，而 `scratch/wav_probe3_le_after_lazy.svfmt`
   复现了链式步进真正需要的小端字段被拒的原始报错：

   ```text
   scratch/wav_probe3_le_after_lazy.svfmt:14:5: error: Little-endian fields must begin at a byte boundary within the structure
   ```

   因此 RIFF chunk 链无法在 DSL 内自举：运行时载荷之后读不到下一个
   chunk 的小端长度字段。必须按 MP4 先例（`Mp4BoxScanner`，提交
   `a228304`）在 `src/rules/` 增加 host 端扫描器。

2. **条件与断言边界。** 首版分派探针
   （`scratch/wav_probe4_header_dispatch.svfmt`）暴露了四个塑造下文语法的语言限制：

   ```text
   wav_probe4_header_dispatch.svfmt:30:33: error: Conditions require a field or context_value equality
   wav_probe4_header_dispatch.svfmt:39:40: error: Invalid character in DSL source          （不支持三元 `?:`）
   wav_probe4_header_dispatch.svfmt:19:9: error: Assertions must be top-level or repeat-local items
   wav_probe4_header_dispatch.svfmt:51:21: error: Only h264_start_code, adts_frame, and mp4_box are supported
   ```

3. **switch 分支命名空间。** `scratch/wav_probe5_final_shape.svfmt` 首次
   运行因两个 switch 分支同时声明 `cb_size` 报 `Duplicate field name`，
   证实 switch 各分支共享同一结构级命名空间。修正后的探针通过全部静态
   检查，唯一剩余错误是扫描器白名单闸门，由 W2 能力切片关闭：

   ```text
   wav_probe5_final_shape.svfmt:68:21: error: Only h264_start_code, adts_frame, and mp4_box are supported
   ```

`AnalysisSession`（`src/app/analysis_session.cpp:353-359`）与
`svtool analyze`（`tools/svtool/main.cpp:137-149`）目前仲裁三种格式、
其余回退 H.264；提交 `11330e5` 已把未决情形显式提示，不再静默误标。

## 决策

### 1. 格式分类（契约第 2 节第 1 步）

- **容器**，chunk 分界；RIFF 表单以传输方式承载编解码载荷。本文的 WAV
  指 RIFF/WAVE 文件族，一律从字节判定，绝不依据文件扩展名推断。
- 除原始字节标识符（FourCC）外**全程小端**。
- **顶层格式专属入口点**，不是跨层 payload 视图。
- chunk 帧：`chunk_id`（4 原始字节）+ `chunk_size`（无符号 32 位小端
  字节数）+ 载荷 + 奇数载荷后的一个 pad 字节。表单由 12 字节文件头
  （`'RIFF'` + 表单长度 + `'WAVE'`）界定；表单终点为 `8 + riff_size`。

### 2. 扫描器能力切片（Task W2）

- 新增 `RiffChunkScanner`（`src/rules/include/streamview/rules/riff_chunk_scanner.h`、
  `src/rules/riff_chunk_scanner.cpp`）：从偏移 12 步进 chunk 链，
  `P' = P + 8 + size + (size & 1)`，载荷在源终点钳制并给出
  truncated-source 诊断；下一个头无法成帧时停止（不做重同步扫描）。
- 扫描器把 12 字节 RIFF 表单头作为**第一条记录**发出，跨 `[0, 12)`。
  这在既有 DSL 表面内解决了"文件头 + chunk 序列"缺先例的问题：序列
  元素结构按首 FourCC 分派。
- 扩展 `DslScannerKind` 增加 `RiffChunk`，在解析器与编译器白名单中
  接受 `scan(riff_chunk)`（`src/rules/dsl.cpp:3651-3655`、
  `src/rules/dsl_ir.cpp:3929-3938`），并同步更新英中语言参考。
- 渐进索引、可取消、可原地恢复，镜像 `Mp4BoxScanner`
  （经 `@index(progressive)` 获得 ADR-0027 语义）。

### 3. 探测器（Task W2）

`detectRiffWaveCandidate`（`src/rules/riff_wave_detector.cpp`）探测
前 64 KiB 页：

- **Strong**：偏移 0 为 `'RIFF'`、偏移 8 为 `'WAVE'`，且自偏移 12 起至少
  两个无缝成帧 chunk；末尾截断 chunk 不破坏 Strong（截断免疫，镜像 MP4）。
- **Probable**：双 magic 成立且恰好一个成帧 chunk。
- **Weak**：仅有 `'RIFF'` magic，链路断裂或无法验证。

**误报边界。** 恰好以 `'RIFF'` 开头但无法平铺的字节保持 Weak/Probable
并在仲裁中落败，检测结果为未决（由提交 `11330e5` 的未决横幅呈现）。
MP4 `mdat` 内出现的 `'RIFF'` 字节串被 MP4 平铺经包含谓词吸收。若 MP4
与 WAV 平铺同时 Strong（病态情形，二者锚点字节不相交），沿用 MP4 的
既定优先级。`RIFX`（`0x52494658`）、`RF64`（`0x52463634`）与 BW64 在
v0.1 **不产生检测结果** —— 见延期项。

### 4. 官方规则包（`org.streamview.wav`，Task W3）

清单 `src/rules/official/org.streamview.wav/rule.toml`，初始版本
**0.1.0**，`manifest-version = 1`，`language = "0.1"`，
`engine = ">=0.1.0 <0.2.0"`；单一入口点：

```toml
[[entrypoints]]
id = "wave"
format = "audio.wav"
source = "src/wav_riff.svfmt"
profiles = ["wave"]
depth = "chunks"
detector = "riff-wave"
```

语法形态（已探针验证）：序列元素结构读首 FourCC 后分派：

- `fourcc == 0x52494646`（`'RIFF'`）：表单头记录 —— `riff_size`（LE32）
  与 `wave_id`（`'WAVE'`，`0x57415645`）。
- `fourcc == 0x666D7420`（`'fmt '`）：16 字节公共 WAVEFORMATEX
  （`audio_format`、`num_channels`、`sample_rate`、`byte_rate`、
  `block_align`、`bits_per_sample`，全小端），随后 `switch (chunk_size)`
  分派 `case 18`（`fmt_cb_size`）、`case 40`（`extensible_cb_size`、
  `valid_bits_per_sample`、`channel_mask`、`subformat_guid_data1`、
  `subformat_guid_tail[12]`）、
  `default` 登记 `@lazy(chunk_size - 16) bytes fmt_tail`。
- `fourcc == 0x66616374`（`'fact'`）：`chunk_size == 4` 时为
  `sample_length`（LE32），否则为 opaque 惰性载荷。
- 其余（`data` `0x64617461`、`LIST` `0x4C495354`、`JUN K` `0x4A554E4B`、
  未知）：`@lazy(chunk_size) bytes chunk_payload`。

值域规则：

- `audio_format` 为**不带 `@enum` 的普通 16 位字段**：tag 空间开放，
  封闭枚举会致命拒绝合法文件。常见 tag（1 PCM、3 IEEE float、6 A-law、
  7 μ-law、17 IMA ADPCM、0xFFFE extensible）写入字段说明，遵循
  ADR-0040/ADR-0059 的非致命二分法。
- `audio_format == 0xFFFE` 以形态验证（`chunk_size == 40` 且
  `cb_size == 22` 展示），不用按 tag 分支。
- PCM 采样**永不解码**：`data` 保持 lazy opaque 区域，与只读 bit 级
  定位一致。

### 5. 检测与激活（Task W3）

- `DetectedFormat::RiffWave` 加入 `src/rules/format_selection.cpp`
  仲裁，成为第二个容器平铺：WAV 覆盖终点为最远成帧 chunk，现有基本流
  包含谓词扩展到 WAV。仲裁签名扩为四路检测结果；全部调用点
  （`AnalysisSession`、`svtool`、`format_selection_test`）同批更新。
- `SessionAnalyzerVariant` 增加 `WavRiffAnalyzer`
  （`src/rules/wav_riff_analyzer.cpp`）；`setupResolvedAnalyzer` 按包
  id `org.streamview.wav` 分发，未决路径保持提交 `11330e5` 的行为。
- `svtool analyze` 增加 WAV 分发分支。Qt 应用无需新增格式专属文案：
  覆盖对话框读取目录，时间线报告 "Container tracks unavailable"
  （WAV 无样本表；`fact` 样本计数属后续切片）。
- 经 `qt_add_resources` 在 `src/rules/CMakeLists.txt` 注册包，双语包
  文档置于 `src/rules/official/org.streamview.wav/docs/`。

### 6. 畸形、截断、不支持、取消与限额语义

- **头截断**（< 12 字节）或末尾 chunk 截断：钳制记录并给
  truncated-source 诊断；保留部分结果。
- **畸形长度**：越过表单或源终点的 `chunk_size` 在边界钳制；
  `riff_size < 4` 由扫描器拒绝（因嵌套断言被语言拒绝，从规则移到
  扫描器，见探测）；表单终点之后的字节为未认领的尾随数据，给一条
  非致命诊断。
- **不支持**：`RIFX`、`RF64` 与 `ds64` 在 v0.1 之外，不产生 WAV 候选
  （由未决提示覆盖）。为 RIFF 族 magic 提供专属 unsupported 诊断需要
  检测层分类学改动，与它们一起延期。
- **恢复**：长度链步进没有重同步点；chunk 无法成帧后扫描器停止。
  因此不适用"畸形单元后跟随有效单元"的 fixture，并作为明确边界记录。
- **限额与取消**：扫描器批次镜像 MP4 分析器的有界发布，节点与指令
  预算沿用现有 VM/运行时预算，取消后留下可恢复的渐进索引。

## 分阶段实施序列

- **Task W1（当前）**：探测报告与双语 ADR-0115（本提交，仅 Markdown；
  按 ADR-0019 跳过 Hosted CI）。
- **Task W2**：`RiffChunkScanner`、`RiffWaveDetector`、
  `DslScannerKind::RiffChunk` 降级与白名单、英中语言参考更新、
  扫描器/探测器/dsl 能力测试（`feat(rules)`）。
- **Task W3**：官方包 `org.streamview.wav` v0.1.0、双语包文档、资源
  注册、`WavRiffAnalyzer`、四路仲裁、session/svtool 激活、确定性
  fixture 生成器与矩阵、分析器/应用测试、fuzz 驱动（`feat(wav)`）。
- **Task W4**：dev/ci/sanitize 全量、Hosted 三平台 CI、
  `git diff --check`、实施计划回写（`docs: record`）。

### Fixture 与测试矩阵（契约第 5 节）

全部 fixture 由入库生成器（`tests/fixtures/generate_wav_fixtures.py`）产出：

1. `wav_w2_minimal_pcm.wav` —— 8-bit 单声道 8 kHz，16 字节 `fmt `，
   4 字节 `data`。
2. `wav_w2_pcm16_stereo.wav` —— 16-bit 立体声 44.1 kHz，含 `fact` 与
   `LIST`，以及触发 pad 字节的奇数长度未知 chunk。
3. `wav_w2_extensible.wav` —— `WAVE_FORMAT_EXTENSIBLE`，`fmt ` 长 40。
4. `wav_w2_truncated_header.wav` —— 8 字节（`'RIFF'` + 长度，无 `'WAVE'`）。
5. `wav_w2_truncated_chunk.wav` —— `data` 长度越过 EOF。
6. `wav_w2_malformed_sizes.wav` —— `riff_size < 4`；`0xFFFFFFFF` 的
   chunk 长度。
7. `wav_w2_trailing_bytes.wav` —— 合法表单后跟未认领字节。
8. `wav_w2_riff_in_mp4.mp4` —— `mdat` 内含 `'RIFF'` 字节的合法 MP4
   （探测器误报包含性）。

测试：`riff_chunk_scanner_tests`、`riff_wave_detector_tests`、
`dsl_test`/`dsl_ir_test` 白名单用例、`wav_riff_analyzer_tests`
（按 fixture 逐 bit 精确验收字段、截断/畸形诊断）、
`format_selection_tests` 四路仲裁、`analysis_session_tests` 端到端
打开 fixture 1-3，以及沿用 ADR-0112 模式的 `riff_chunk_scanner` fuzz
驱动。大文件/稀疏源用例沿用测试内稀疏源方案，不落仓库 fixture。

## 后果

### 正面

- WAV 文件以完整 chunk 树与 `fmt ` 字段语义、精确 bit 偏移打开；
  未识别文件保持诚实的未决横幅。
- 扫描器沿用已接受的 MP4 模式；除扫描器白名单外无核心或 DSL 语义改动。

### 负面

- fmt 切片按精确长度（16/18/40）：非规范长度的 fmt chunk 退化为
  opaque 尾部。v0.1 不结构化解码小于或大于 40 字节的 extensible chunk。
- 压缩 WAVE 载荷保持 opaque；RF64/RIFX/BW64 不被检测。

## 参考

- Microsoft & IBM，"Multimedia Programming Interface and Data
  Specifications 1.0"（1991 年 8 月）—— RIFF chunk 布局、WAVE 表单
  定义、`fmt `/`data`/`fact` chunk
- [MS-RIFF] —— Microsoft Resource Interchange File Format (RIFF) File Format
- Microsoft Learn，WAVEFORMATEX / WAVEFORMATEXTENSIBLE 结构参考
- RFC 2361 —— WAVE and AVI Codec Registries（信息性，tag 含义）
- ADR-0010（格式语言）、ADR-0011（双坐标）、ADR-0025（excluded
  spans）、ADR-0027（渐进恢复）、ADR-0040/0059（非致命二分法）、
  ADR-0092（ADTS 扫描器/探测器先例）、ADR-0099（MP4 包与激活先例）、
  ADR-0112（扫描器 fuzzing）、ADR-0114（格式扩展交付契约）

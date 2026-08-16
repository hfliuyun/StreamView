# ADR-0095：AAC raw_data_block 压缩载荷与 Profile 处理

- **状态**：Proposed
- **日期**：2026-08-16
- **作者**：StreamView 贡献者
- **领域**：DSL 规则 / AAC 解码 / 分析器执行

## 背景

StreamView 实施计划阶段 4 规定了对 AAC-LC 音频（ISO/IEC 14496-3:2019, 第 5 版）的完整结构支持。在完成 ADTS 头结构化解码（ADR-0093，任务 T16）与 AudioSpecificConfig / Program Config Element 解码（ADR-0094，任务 T17c）之后，阶段 4 需完成最后三项目标（`docs/implementation-plan.md:196-198`）：
1. 将 `raw_data_block` 整体标记为压缩载荷区域，不隐藏实现 Huffman 解码；
2. 形式化处理并报告 HE-AAC、ELD 及其他 AAC profile；
3. 针对 ADTS 头部、ASC/PCE、截断码流、CRC 错误/存在性以及 profile 约束进行逐 bit 验收审计。

### 探测实测结论

为确保严密的架构设计，通过独立探针建立了以下技术事实：

1. **DSL 语法表达能力（无需新增语言能力）**：
   在 `src/rules/official/org.streamview.aac/src/aac_adts.svfmt` 的 `AdtsHeader` 末尾追加：
   ```dsl
   computed<u64> header_bytes = (protection_absent == 1) * 7 + (protection_absent == 0) * 9;
   computed<u64> raw_data_block_bytes = aac_frame_length - header_bytes;
   @lazy(raw_data_block_bytes) bytes raw_data_block
       @spec("ISO/IEC 14496-3:2019", "1.A.1")
       @description("Raw audio data payload block.");
   ```
   经 `svtool rule check` 实测通过（`Rule OK`，rc=0）。条件 `crc_check` 为 16 位，严格保持字节对齐；且 `@lazy` 区域位于 `AdtsHeader` 结构末尾、其后无字段，不触发保守对齐拒绝规则（`docs/zh-CN/format-language/README.md:578-582`）。

2. **分析器视图映射为真实阻塞项**：
   `src/rules/aac_adts_analyzer.cpp:346` 当前构造的 `SourceMapping` 仅覆盖 `*record.headerSpan`（7 或 9 字节）。当 `AdtsHeader` 包含 `@lazy(raw_data_block_bytes)` 时，以 20 字节真实 ADTS 帧在 7 字节头部视图下执行，会在 `src/rules/dsl_vm.cpp:3143` 失败并报 `DslExecutionStatus::TruncatedSource` 与诊断 `Lazy byte region exceeds the available source range`。
   当源映射扩展至完整帧跨度（`*record.frameSpan`，20 字节）时，DSL VM 成功物化该帧，`raw_data_block` 处于 `MaterializationState::Lazy` 状态（比特偏移 56，比特长度 104）。

3. **截断语义契约转移**：
   当 ADTS 帧在文件尾发生截断（例如声明 20 字节但在文件中仅有 15 字节）时，DSL VM 在注册 lazy 区域时命中 `bitCount > reader.remainingBits()`（`src/rules/dsl_vm.cpp:3140-3149`），发出 Error 级别的 `DslExecutionStatus::TruncatedSource`。
   该行为将取代原先在 C++ 层面合成的 Warning 级诊断（`ADTS frame payload is truncated at EOF`，`src/rules/aac_adts_analyzer.cpp:456-467`），需同步更新 `tests/rules/aac_adts_analyzer_test.cpp:321-361` 中的测试断言。

4. **下溢防御机制**：
   若损坏帧声明 `aac_frame_length < 7`（或含 CRC 时 `< 9`），既有断言 `assert(aac_frame_length >= minimum_frame_length)` 会在 lazy 载荷求值前被执行，以 `InvalidSyntax`（`Assertion condition is false`）终止，防止发生负数/下溢的 lazy 字节分配。

5. **DSL 侧不支持 Profile 的报告机制现状**：
   对 `src/rules/` 全仓审计确认，DSL 体系内目前不存在发出 `MaterializationState::Unsupported` 或 `DiagnosticCode::UnsupportedSyntax` 的机制：
   - `@enum` 违规是致命的（`DiagnosticCode::InvalidSyntax`，Severity: Error），会导致帧解码完全被拒绝；
   - `@range(min, max)` 仅支持单段连续区间，无法表达非连续 profile ID（例如 AOT 2, 5, 29, 39）；
   - 在 ADTS 中，2 位的 `profile` 字段（表 1.A.1）仅能编码 0..3（Main, LC, SSR, LTP），无法直接编码 AOT 5 (SBR)、29 (PS) 或 39 (ELD)。实际广播码流中，ADTS 内传输的 HE-AAC 码流在 ADTS 头中一律声明 `profile = 1` (LC)，并在 `raw_data_block` 内部携带 SBR/PS 扩展。

---

## 决策

### 1. `raw_data_block` 规范条款号归属

依据 ISO/IEC 14496-3:2019（第 5 版）：
- **ADTS 载荷编排归属**：第 1 部分附录 1.A 子条款 **1.A.1**（*Fixed and variable header of ADTS*）及表 1.A.5（`adts_frame()`）；
- **Raw Data Block 语法定义**：第 4 部分（General Audio）子条款 **4.5.2.1**（*raw_data_block* / *Syntactic elements*）。

在 `aac_adts.svfmt` 规则中，`@lazy(raw_data_block_bytes) bytes raw_data_block` 标注为子条款 `1.A.1`。

### 2. `raw_data_block` DSL 规则定义

在 `src/rules/official/org.streamview.aac/src/aac_adts.svfmt` 的 `AdtsHeader` 结构末尾追加以下字段定义：

```dsl
    computed<u64> header_bytes = (protection_absent == 1) * 7 + (protection_absent == 0) * 9;
    computed<u64> raw_data_block_bytes = aac_frame_length - header_bytes;
    @lazy(raw_data_block_bytes) bytes raw_data_block
        @spec("ISO/IEC 14496-3:2019", "1.A.1")
        @description("Raw audio data payload block.");
```

### 3. 分析器帧视图映射扩展（能力切片 T18b）

在 `src/rules/aac_adts_analyzer.cpp:346` 中，将逻辑视图构造由 `{*record.headerSpan}` 调整为 `{*record.frameSpan}`。
扫描器（`src/rules/aac_adts_scanner.cpp:134-148`）已将 `record.frameSpan` 约束于 `availableBytes`。映射整帧跨度使得 DSL VM 能够在解析头部字段后立即将 lazy 载荷区域注册在实际帧载荷字节上。

### 4. 统一截断契约决策（方案 A）

我们采纳**方案 A**（统一 DSL VM 截断语义）：
- 当码流在帧载荷中间意外结束时，DSL VM 尝试分配 `raw_data_block_bytes` 并检测到 `bitCount > reader.remainingBits()`；
- DSL VM 发出 Error 级 `DslExecutionStatus::TruncatedSource`，诊断消息为 `Lazy byte region exceeds the available source range`；
- 帧节点被标记为 `Invalid`，但保留已解码的头部子字段及其源码锚定位置；
- `aac_adts_analyzer.cpp:456-467` 处的 C++ 合成 Warning 诊断被 VM 原生诊断取代；
- `tests/rules/aac_adts_analyzer_test.cpp:321-361`（`handlesPayloadTruncationAtEof`）迁移为断言 `DiagnosticSeverity::Error`、消息 `Lazy byte region exceeds the available source range` 以及 `header1->state() == MaterializationState::Invalid`。

### 5. Profile 处理与明确能力边界

1. **ADTS 传输流**：通过 `@enum(AacProfile)` 约束于 `AacProfile`（0=Main, 1=LC, 2=SSR, 3=LTP）。在 ADTS 中传输的 HE-AAC v1/v2 码流依据广播规范在 ADTS 头中均携带 `profile = 1` (LC)，作为 LC 帧正常解码，其 raw data 载荷保持未解析状态。
2. **AudioSpecificConfig (ASC)**：非 GA 音频对象类型（例如 SBR=5, PS=29, ELD=39）能够成功解析 GA 基础头语法且不报错（`MaterializationState::Materialized`），正如 ADR-0094 §3:315 所记录。在未来里程碑引入专属非 GA 载荷解析器之前，这被正式确认为已知且受控的已记录能力边界。

### 6. 阶段 4 任务切片与纪律规划

为保持严格的单职责提交与能力/规则解耦：

- **任务 T18a**（当前任务）：探测结论、双语 ADR-0095、实施计划记录（Markdown-only）；
- **任务 T18b**：分析器视图映射扩展至帧跨度（`src/rules/aac_adts_analyzer.cpp:346`），执行器能力切片，不升包版本；
- **任务 T18c**：规则消费 `@lazy raw_data_block`（`aac_adts.svfmt`），包版本升级至 `0.1.3`，测试套件更新；
- **任务 T18d**：Profile 处理验证与文档对齐；
- **任务 T18e**：5 类逐 bit 验收审计、阶段 4 复选框全量勾选（`docs/implementation-plan.md:196-198`）、推进阶段至 Phase 5。

---

## 验证矩阵

| 探针 / 验证项 | 目标文件 / 命令 | 实测输出与确认结论 |
| :--- | :--- | :--- |
| **P1: DSL 语法检查** | `svtool rule check scratch/probe_adts_lazy.svfmt` | `Rule OK` (rc=0) |
| **P2: 20 字节帧整帧映射** | `scratch/probe_lazy_raw_data_block` (Test A) | `status=0 materialized=1 AdtsHeader state=6 children=19 child=raw_data_block state=0 bitOffset=56 bitLength=104` |
| **P3: 15 字节截断帧映射** | `scratch/probe_lazy_raw_data_block` (Test B) | `status=1 materialized=0 AdtsHeader state=5 diag code=0 sev=2 msg="Lazy byte region exceeds the available source range"` |
| **P4: 7 字节仅头部映射** | `scratch/probe_lazy_raw_data_block` (Test C) | `status=1 materialized=0 AdtsHeader state=5 diag code=0 sev=2 msg="Lazy byte region exceeds the available source range"` |
| **P5: @range 非连续参数检查** | `scratch/probe_unsupported` (Probe 3) | `Parse succeeded=0 diag code=14 msg="@range requires two integer arguments"` |

---

## 参考资料

- ISO/IEC 14496-3:2019, Information technology — Coding of audio-visual objects — Part 3: Audio（第 1 部分附录 1.A，第 4 部分子条款 4.4.1, 4.4.1.1, 4.5.2.1）。
- [ADR-0040：非致命语法警告与范围注解](0040-non-fatal-syntax-warnings-and-range-annotations.md)
- [ADR-0092：AAC ADTS 分帧枚举与规则包架构](0092-aac-adts-frame-enumeration-and-rule-package.md)
- [ADR-0093：ADTS 头结构化解码与官方规则包](0093-adts-header-structured-decoding-and-official-rule-package.md)
- [ADR-0094：AudioSpecificConfig 与 Program Config Element 结构化解码](0094-audio-specific-config-and-program-config-element.md)

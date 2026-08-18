# ADR-0104：通用结构型 Payload 分派、变换视图与会话级上下文生命周期管理

- **Status**: Proposed
- **Date**: 2026-08-19
- **Authors**: StreamView Contributors

---

## Context

在任务 P5h（[ADR-0102](0102-mp4-sample-descriptions-and-codec-configurations.md)）中，`org.streamview.mp4` v0.1.3 引入了样本描述（`stsd`）、样本条目（`avc1`、`mp4a`）以及编解码配置 Box（`avcC`、`esds`）。在这些结构中，单个基本流元数据载荷被暴露为带有 `@target_format` 注解的 `@lazy` 字节区域：
- `avcC` 重复的序列参数集（`sequenceParameterSetNALUnit`）与图像参数集（`pictureParameterSetNALUnit`）载荷标注为 `@target_format("video.h264.nal")`；
- `esds` `DecSpecificInfo`（`asc_bytes1..4`）载荷标注为 `@target_format("audio.aac.asc")`。

在任务 P5i-1 与 P5i-2（[ADR-0103](0103-cross-layer-structured-entry-execution-and-navigation.md)）中，`StructuralEntryRunner` 与可复用的 `BoundedSourceView` 建立了孤立单结构体（如 `AudioSpecificConfig`）的格式中立执行能力。然而，任务 P5i-3 对独立 H.264 NAL 解析的可表达性探测发现了阻碍 DSL 0.1 表达独立 NAL 执行的五项基本能力门禁：

### 问题分析与探针证据

1. **缺乏结构体级子结构 / Switch 分派机制**：
   - `tests/probes/p5i3/probe_q1_structural_switch.svfmt` 尝试在 `struct` 定义内的 `switch` 分支中声明子结构体。
   - 编译在 `src/rules/dsl.cpp:1000-1004` 被拦截并报错：
     ```text
     error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type
     ```
   - *事实*：DSL 0.1 结构体体仅支持标量字段（`bits`、`ue`、`se`、`ff_coded`）、计算字段、lazy 区域、`compressed_payload`、断言以及尾部结构。结构体不支持实例化嵌套子结构体，也不支持从 `switch` 分支中调用结构体。

2. **Payload 分派静态绑定于扫描序列**：
   - `tests/probes/p5i3/probe_q2_unbound_payload.svfmt` 与 `tests/probes/p5i3/probe_q2_sequence_entry_mismatch.svfmt` 尝试在非序列目标上或入口不匹配的情况下声明 `payload<rbsp>`。
   - 编译在 `src/rules/dsl.cpp:3771-3775` 与 `src/rules/dsl.cpp:3778-3782` 被拦截并报错：
     ```text
     error: A payload dispatch must name a declared sequence
     error: A payload dispatch requires an entry naming its sequence
     ```
   - *事实*：`payload<view>` 静态限制于流式 `scan` 序列。

3. **单 Entry 声明限制**：
   - `tests/probes/p5i3/probe_q5_multiple_entries.svfmt` 尝试在单个 `.svfmt` 文件中声明多个 `entry` 语句。
   - 编译在 `src/rules/dsl.cpp:1765-1772` 被拦截并报错：
     ```text
     error: A DSL program may contain only one entry
     ```
   - *事实*：一个 `.svfmt` 文件只能声明一个顶层 `entry`。由于 DSL 0.1 缺乏跨文件模块导入，若为 NAL 单元创建独立 `.svfmt`，必须全量复制 1200+ 行 H.264 语法定义。

4. **脱壳变换耦合且缺乏结构型 EBSP 脱壳**：
   - *源码审计事实*：`H264EbspRbspMapper` 仅在 `src/rules/h264_annex_b_analyzer.cpp:688` 内部被实例化。结构型执行不存在通用变换视图层。`StructuralEntryRunner::execute`（`src/rules/structural_entry_runner.cpp:30-40`）直接通过 `BitReader(baseSource, sourceMapping)` 读取物理字节，不支持脱壳。

5. **无状态结构型执行与缺失的上下文解析**：
   - *源码审计事实*：`StructuralEntryRunner::execute`（`src/rules/structural_entry_runner.cpp:78-95`）使用空的 `DslContextValueResolver()` 调用 `DslExecutor::decodeStruct`。
   - 当结构体导入上下文时（例如 `src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt:393-403` 中 `PictureParameterSetRbsp` 的 `import SequenceParameterSet context ...`），`DslVirtualMachine`（`src/rules/dsl_vm.cpp:718-743`）因未注入上下文目录而返回 `DslExecutionStatus::InvalidDefinition` / `DependencyUnavailable`。
   - `RuleExecutionSession`（`src/rules/include/streamview/rules/rule_execution_session.h:71-100`）包含完整的上下文发布与查找引擎，但目前仅作为流式分析器的私有组件。

---

## 决策

为解决上述架构限制，同时避免引入 H.264 专属 C++ 逻辑或无边界的通用递归调用栈，StreamView 采用通用、格式中立的结构型 payload 分派架构。

### 1. 结构型 Payload 分派 DSL 语法、AST 与 Typed IR

1. **DSL 语法扩展**：
   泛化 `payload<view_kind>`，使其既可指向已声明的 `sequence`（用于流式扫描），也可指向已声明的 `struct`（用于结构型入口）：
   ```dsl
   payload<view_kind> TargetName switch (controller_field) {
       case V1: PayloadStruct1;
       case V2: PayloadStruct2;
       case V3: empty;
   }
   ```
   当 `TargetName` 为 `struct` 时：
   - `controller_field` 必须是在 `TargetName` 顶层无条件声明的无符号标量 `bits<N>` 字段。
   - 每个 `case` 将一个整型常量映射到一个已声明的 payload 结构体名称或 `empty`。
   - 结构型入口点可执行由 `TargetName` 及其所分派 payload 组成的复合结构。

2. **AST 与 IR 模型**：
   - `DslPayloadTargetKind` 枚举：`Sequence`、`Structure`。
   - `DslPayloadDispatch`：`targetKind`、`targetName`、`viewKind`、`controllerFieldName`、`cases`。
   - `DslTypedPayloadDispatch`：`targetKind`、`targetIndex`（结构体索引或扫描索引）、`controllerFieldIndex`、`viewKind`、`cases`（`std::vector<DslTypedPayloadCase>`）。

3. **编译器静态校验**：
   - `TargetName` 必须与 `program.structs` 中的已声明结构体（或 `program.scans` 中的扫描）匹配。
   - `controllerFieldName` 必须是 `TargetName` 中的无符号 bits 字段。
   - 所有 case 目标必须是已声明的有效结构体或 `empty`。
   - 重复的 case 视为 `DuplicateName` 予以拒绝。

---

### 2. 统一分析树层级与坐标归属

当执行带有 payload 分派的结构型入口点时：

1. **Header 阶段**：
   - 为头部结构体（例如 `NalUnit`）创建根 AST 节点。
   - 头部字段（`forbidden_zero_bit`、`nal_ref_idc`、`nal_unit_type`）解码到根节点中。
   - 其 `FieldLocation` 区间直接映射到输入 `SourceMapping` 的初始字节。

2. **分派与变换阶段**：
   - 执行器从已解码的头部字段中提取 `controller_field` 的标量值。
   - 头部之后输入视图中的剩余字节（从 `headerBitLength` 到 `sourceMapping.logicalBitLength()`）构成 payload 切片。
   - 若指定了 `view_kind`（如 `rbsp`），执行器为 payload 切片构建变换后的 `SourceMapping`（例如排除防竞争字节），将每个逻辑 payload bit 回映到根文件的物理源区间。

3. **Payload 阶段**：
   - 使用变换后的 `SourceMapping` 解码匹配的 payload 结构体。
   - Payload 结构体 AST 节点作为子节点挂载在根 header 节点下方。
   - 所有 payload 字段坐标通过变换映射正确反映物理根源坐标。

4. **部分树与诊断合同**：
   - 若头部解码失败：树中包含失败前已解码的头部字段并附带诊断；不执行 payload。
   - 若 payload 解码失败（截断、语法错误、依赖缺失）：根节点保留已物化的头部，payload 子节点记录部分物化状态与诊断。

---

### 3. 变换源视图与边界错误处理

1. **变换提供者**：
   - 将 `H264EbspRbspMapper` 中的脱壳逻辑抽取泛化为可复用、格式中立的视图变换接口（`PayloadTransformProvider` / `PayloadTransformRegistry`）。
   - 支持的内置变换类型：`None`（恒等切片）、`Rbsp`（H.264 / H.265 防竞争字节脱壳）。

2. **区间与坐标不变量**：
   - 输入 `SourceMapping` 必须字节对齐且非空。
   - 变换映射保留排除区间（`0x03` 字节），使得原始字节选择能够精确高亮不连续的物理区间。
   - bit 偏移算术必须严格对齐 `std::numeric_limits<quint64>::max()` 进行检查，防止坐标溢出。

3. **错误传播**：
   - Payload 解码期间遇到 `EndOfSource` / 截断触发 `DslExecutionStatus::TruncatedSource`。
   - 注入故障的 `SourceReadStatus::Error` 触发 `DslExecutionStatus::SourceError`。
   - 取消令牌检查在头部执行前、变换映射前以及 VM 指令循环中进行。
   - 资源限制（步骤预算）在头部与 payload 执行全过程中累计强制执行。

---

### 4. 会话级上下文生命周期管理

1. **会话归属原则**：
   - 上下文目录不能作为未托管的裸指针传递给无状态执行器。
   - `RuleExecutionSession` 被提升为流式与结构型语境下有状态规则执行的标准引擎。
   - `AnalysisSession`（或导航控制器）为每个活动格式流持有一个 `RuleExecutionSession`。

2. **结构型执行间的上下文生命周期**：
   - 当生产者结构体（如 SPS）成功执行时，`RuleExecutionSession` 发布其上下文定义（如 `seq_parameter_set_id`），并将导出的值存储在其内部 `ContextDirectory` 中。
   - 当后续的消费者结构体（如 PPS）在同一会话内执行时，`RuleExecutionSession` 从先前发布的定义中解析上下文导入。
   - 若消费者在生产者之前执行（如 SPS 之前执行 PPS），`RuleExecutionSession` 返回 `DependencyUnavailable` 并附带解释性诊断。
   - 上下文受 `contextScopeId` 作用域约束，在会话清除或导航栈重置时重置。

---

### 5. 规则包入口点目标解析

我们评估了多入口包的四种潜在复用方案：

1. **方案 1：单个 `.svfmt` 内多个 `entry` 语句**：
   - 在缺乏目标命名的情况下会导致哪个 entry 处于活动状态的语法二义性。
2. **方案 2：Manifest 目标选择（`rule.toml` entry 选择）**：
   - `rule.toml` `entrypoints` 在共享 `.svfmt` 路径内显式指定 `entry = "StructName"` 或 `entry = "SequenceName"`。
   - 编译器编译整个 `.svfmt`，并直接从 `program.structs` 或 `program.scans` 中解析所请求的 entrypoint 目标。
   - 若 `rule.toml` 中省略 `entry`，则回退到该文件的默认 `entry` 声明。
3. **方案 3：独立源文件（`path = "src/h264_nal.svfmt"`）**：
   - 全量复制 1200+ 行 H.264 语法定义，带来极高的维护负担与漂移风险。
4. **方案 4：跨文件结构体模块导入**：
   - 对于 v0.1 而言编译器复杂度过高。

**决策**：采用 **方案 2（Manifest 目标选择）**。
这使得 `org.streamview.h264` 能够从单一共享的 `src/h264_annex_b.svfmt` 文件中同时暴露 `video.h264.annex-b`（序列入口 `nal_units`）与 `video.h264.nal`（结构型入口 `NalUnit`），零代码重复。

---

### 6. 流式 Annex-B 运行器收敛

- 流式 `H264AnnexBAnalyzer` 与结构型执行路径将共享完全相同的 `RuleExecutionSession` 与 payload 变换例程。
- 不会存在并行或分歧的 EBSP/RBSP 映射逻辑。
- 既有 Annex B 解析行为与测试套件实现零回归。

---

### 7. 安全性、复杂度受限与格式中立

- **严格受限的执行**：结构型 payload 分派严格为单层（一个头部 + 至多一个 payload 结构体），禁止通用的递归调用栈。
- **格式中立的运行时**：C++ 运行时不包含任何格式特定名称（`nal_unit_type`、`FourCC`、`avcC`、`SPS`、`PPS`、`H264`）。所有格式语义纯粹存在于 `.svfmt` 声明式规则中。
- **受限的变换语言**：变换仅能从固定枚举的稳健、已测试映射类型（`None`、`Rbsp`）中选择。

---

## 验证矩阵

| 测试标识符 | 类别 | 输入 Fixture / 前置条件 | 执行路径 | 预期状态 | 关键断言 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `test_parser_structural_payload_dispatch` | DSL 解析器 / IR | 合法与非法的 `payload<rbsp> Struct switch` 声明 | `DslCompiler::compile` | 合法声明成功；非法声明产生特定诊断 | AST 与 Typed IR 正确编码 `DslPayloadTargetKind::Structure`、控制器索引与 cases。 |
| `test_structural_payload_header_and_payload_success` | 核心执行 | 包含 header + SPS 类 payload 的合成 NAL fixture | `RuleExecutionSession` 结构型执行 | `Materialized` | 头部字段与 payload 字段位于单棵树中；坐标精确匹配物理源区间。 |
| `test_structural_payload_ebsp_excluded_spans` | 变换 | 包含 `0x00000301` 的合成 NAL payload | 变换视图执行 | `Materialized` | 防竞争字节从逻辑长度中排除；payload 字段跨排除区间正确映射。 |
| `test_structural_payload_empty_and_default_cases` | 边界情况 | 控制器匹配 `empty` 分支与未处理分支 | 结构型 payload 运行器 | `Materialized` / `Unsupported` | Empty 分支生成已物化的 header 且无 payload 子节点；未处理分支发出 unsupported 诊断。 |
| `test_structural_payload_truncation_and_error` | 故障注入 | 截断的 payload 字节与注入 I/O 错误的源 | 结构型 payload 运行器 | `TruncatedSource` / `SourceError` | 部分树保留 header；payload 节点标记匹配的诊断与状态。 |
| `test_session_context_producer_consumer_chain` | 上下文生命周期 | 在同一 `RuleExecutionSession` 中先执行 SPS 生产者后执行 PPS 消费者 | 对 SPS 然后 PPS 调用 `RuleExecutionSession::run` | 两者均 `Materialized` | PPS 成功导入 SPS 上下文值（`profile_idc`、`chroma_format_idc`）。 |
| `test_session_context_missing_dependency` | 上下文生命周期 | 在无先前 SPS 生产者的前提下执行 PPS 消费者 | 仅在 PPS 上调用 `RuleExecutionSession::run` | `DependencyUnavailable` | 返回 `DependencyUnavailable` 状态并附带标明缺失上下文 ID 的诊断。 |
| `test_manifest_target_resolution` | 规则包 / Manifest | 声明具有不同 `entry` 目标的多个 entrypoint 的 `rule.toml` | `RulePackageCatalog::resolveByFormat` | 每个目标均 `Found` | 选定的 entrypoint 解析并编译目标结构体或序列，无需语法重复。 |
| `test_h264_annex_b_zero_regression` | 回归测试 | 完整的 `tests/rules/h264_annex_b_analyzer_test.cpp` 套件 | 在 Annex-B 码流上运行 `H264AnnexBAnalyzer` | `Success`（全部测试通过） | 完整 125/125 测试套件通过，AST 输出完全相同。 |
| `test_aac_mp4_zero_regression` | 回归测试 | 所有 AAC 与 MP4 分析器测试套件 | `AacAdtsAnalyzer`, `Mp4IsobmffAnalyzer` | `Success`（全部测试通过） | 所有官方格式零回归。 |

---

## 影响

### 正向影响
- **格式中立的模块化**：能够通过干净的声明式 DSL 规则解码 MP4 编解码配置载荷（`avcC` SPS/PPS），无需 C++ 格式耦合。
- **零语法重复**：Manifest 目标解析允许 `video.h264.annex-b` 与 `video.h264.nal` 共享 `src/h264_annex_b.svfmt` 中相同的 1200+ 行 H.264 语法。
- **确定性的上下文传播**：`RuleExecutionSession` 提供了受审查、会话管理的 SPS->PPS 上下文解析，不污染无状态运行器。
- **统一的变换引擎**：消除了 EBSP/RBSP 映射的双重实现。

### 负向影响 / 权衡
- 需要扩展 DSL 解析器、IR 与编译器以支持结构型 `payload` 声明。
- 需要更新 `RulePackageCatalog` 与 `RuleExecutionSession` 以支持 manifest entry 目标选择与结构型执行。

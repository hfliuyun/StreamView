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
   - *事实*：本探针所测试的结构型 switch 写法会被拒绝；但 DSL 0.1 仍有其它结构体控制流与容器形式，因此该探针不能证明所有嵌套表达方式都不可能。

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
   - *事实*：一个 `.svfmt` 文件只能声明一个顶层 `entry`。当前 manifest 也拒绝重复 source path（`src/rules/rule_package.cpp:384-425`），但这是规则包 schema 策略，并不能证明另一种 source 布局必须复制整个 Annex-B 文件。后续目标选择方案必须修改该策略，或采用其它复用方式。

4. **脱壳变换耦合且缺乏结构型 EBSP 脱壳**：
   - *源码审计事实*：`H264EbspRbspMapper` 在 `src/rules/h264_annex_b_analyzer.cpp:666-693` 内部实例化。结构型执行不存在通用变换视图层。`StructuralEntryRunner::execute` 在 `src/rules/structural_entry_runner.cpp:28-40` 校验入口，并在 `:87-103` 通过 `BitReader(baseSource, sourceMapping)` 读取传入的物理映射，不支持脱壳。

5. **无状态结构型执行与缺失的上下文解析**：
   - *源码审计事实*：`StructuralEntryRunner::execute`（`src/rules/structural_entry_runner.cpp:78-95`）使用空的 `DslContextValueResolver()` 调用 `DslExecutor::decodeStruct`。
   - 当结构体导入上下文时（例如 `src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt:393-403` 中 `PictureParameterSetRbsp` 的 `import SequenceParameterSet context ...`），当前结构型 runner 未注入 resolver（`src/rules/structural_entry_runner.cpp:87-103`），VM 会在 `src/rules/dsl_vm.cpp:718-743` 返回 `InvalidDefinition`，诊断为 `Imported context value resolver is unavailable`。真实会话中找不到生产者或依赖过期时，才是独立的 `DependencyUnavailable` 结果。
   - `RuleExecutionSession`（`src/rules/include/streamview/rules/rule_execution_session.h:71-100`）拥有流式分析器使用的上下文发布与查找能力，但目前不是可复用的复合结构执行器，并且绑定单一 source 指针与 tree identity（`src/rules/rule_execution_session.cpp:48-61`）。

---

## 决策

为解决上述架构限制，同时避免引入 H.264 专属编排逻辑或无边界的通用递归调用栈，StreamView 采用有界的结构型 payload 分派架构。“格式中立”适用于复合分派器及其合同；变换 provider 可以由格式能力注册，但不得把格式名称泄漏进通用 runner。

本 ADR 仅是设计合同。P5i-3b 必须先以可独立评审的能力提交实现这些合同，之后才能修改官方 H.264 规则包；P5i-4 在这些提交复审前继续阻断。

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
   - 结构型入口点可执行由 `TargetName` 及至多一个所分派 payload 组成的复合结构。
   - 选择字节型变换前，头部末尾必须字节对齐；否则在任何变换读取前以 `InvalidDefinition` 失败。

2. **AST 与 IR 模型**：
   - `DslPayloadTargetKind` 枚举：`Sequence`、`Structure`。
   - `DslPayloadDispatch`：`targetKind`、`targetName`、`viewKind`、`controllerFieldName`、`cases`。
   - `DslTypedPayloadDispatch`：`targetKind`、`targetIndex`（结构体索引或扫描索引）、`controllerFieldIndex`、`viewKind`、`cases`（`std::vector<DslTypedPayloadCase>`）。

3. **编译器静态校验**：
   - `TargetName` 必须与 `program.structs` 中的已声明结构体（或 `program.scans` 中的扫描）匹配。
   - `controllerFieldName` 必须是 `TargetName` 中的无符号 bits 字段。
   - 所有 case 目标必须是已声明的有效结构体或 `empty`。
   - 重复的 case 视为 `DuplicateName` 予以拒绝；缺少目标、非标量 controller 与目标名称二义性均为类型化定义失败。
   - `viewKind` 是不透明的 DSL 标识符。解析与编译阶段只保留该值，不查询或硬编码 provider 集合；运行时 registry 负责解析，未注册 provider 必须在 payload 解码前 fail closed 并返回 `InvalidDefinition`。

---

### 2. 统一分析树层级与坐标归属

当执行带有 payload 分派的结构型入口点时：

1. **复合事务与执行模式**：
   - 分析树保留已有 root container。头部结构体在两个阶段执行期间保持为 `Indexing` 子节点；这是因为 `AnalysisTree::appendChild` 只接受 `Indexing` parent（`src/core/analysis_model.cpp:118-135`）。
   - 复合执行器拥有一个累计 instruction/node budget 和一个 cancellation token。上下文发布必须暂存，只有 header 与 payload 都成功后才能提交。
   - tree snapshot 可以移除新追加的节点，但 context side effect 需要独立的 staged directory 或 transaction；仅调用 `AnalysisTree::restore` 不足以回滚上下文。
   - **执行模式**：执行器支持两种互斥的 payload 执行模式：
     - *显式 payload 模式*：调用方显式传入 `payloadStructureIndex`、`payloadMapping` 和 `transformProviderId`（保持向后兼容）。
     - *自动 typed-dispatch 模式*：调用方设置 `autoDispatchPayload = true` 并提供 `payloadMapping`。执行器从 `program.payloadDispatch` 和已解码头部 controller 字段值动态解析 payload 结构体与变换 provider。
     - 同时指定显式 `payloadStructureIndex` 与 `autoDispatchPayload = true` 时 fail closed，返回 `InvalidDefinition`。
     - 在 `program.payloadDispatch` 缺失、由 `dispatch.targetKind` 与 `dispatch.targetIndex` 解析出的结构体与 `headerStructureIndex` 不匹配、或缺少 `payloadMapping` 时请求自动分派均 fail closed，返回 `InvalidDefinition`。
     - Sequence 目标解析为其元素结构体；Structure 目标直接解析为自身，并可作为所选结构型 entry 执行，也可作为源文件默认 sequence entry 的元素结构体执行，从而支持 manifest 目标选择而无需复制共享规则源。

2. **Header 阶段**：
   - 头部字段解码到 header child，其 `FieldLocation` 直接映射到输入 `SourceMapping`。
   - header bit length 必须取自 VM 实际消费量。若末尾未字节对齐，必须在任何字节型变换前拒绝。

3. **分派与变换阶段**：
   - 在自动分派模式下，执行器从已解码的头部字段值中读取 `dispatch.controllerFieldIndex` 处的无符号标量 controller 值。执行器在信任公开或反序列化的 Typed IR 前，必须重新校验该索引仍指向解析后 header 结构体中已声明、无条件的无符号/枚举标量 controller。若字段缺失、未填充、畸形或指向其他字段，执行器将 header 标记为 `Invalid`、产生 `InvalidSyntax` 诊断、恰好回滚一次，并以 `InvalidDefinition` fail closed。
   - Case 决策分支：
     - **结构体分支（Structure case）**：映射到已声明的 payload 结构体索引。执行器使用 Typed IR 中保存的 `dispatch.viewKind` 标识符查询 `PayloadTransformRegistry`，切分剩余逻辑范围，变换载荷输入映射，并解码选中的 payload 结构体。执行结果暴露 `selectedPayloadStructureIndex` 与 `selectedPayloadCaseValue`。
     - **空分支（Empty case）**：映射为无结构体（`empty`）。不解码任何 payload 结构体；执行器直接提交仅包含 header 的事务（`selectedPayloadStructureIndex = std::nullopt`，记录 `selectedPayloadCaseValue`）。
     - **未匹配分支（Unmatched case）**：若 controller 值未匹配任何已声明 case，执行终止并返回 `Unsupported` 状态与 `UnsupportedSyntax` 诊断；Indexing header 标记为 unsupported 且回滚暂存的 context。
   - `PayloadTransformProvider` 返回转发的 `SourceMapping`、独立的排除物理 span 列表、inspection 计数、诊断和终态。排除字节不得塞进 `SourceMapping`，因为 `FieldLocation` 要求转发 span 的 bit 长度等于逻辑范围（`src/core/coordinates.cpp:45-61`）。
   - 变换 provider 查找严格保持格式中立：通用 runner 仅凭 Typed IR 中的不透明字符串标识符（`dispatch.viewKind`）从 registry 查询 provider；通用代码中禁止硬编码格式专属名称（如 H.264、NAL、SPS、PPS 或 RBSP）。未知 provider 返回 `InvalidDefinition`。

4. **Payload 阶段与终态**：
   - 当选定 payload 结构体时（显式指定或匹配到结构体分支），在 header 仍为 `Indexing` 时使用剩余的 compound instruction 和 node 预算解码该 payload。
   - payload 阶段的暂存上下文定义与导入动态绑定于实际选中的 payload 结构体。
   - header 失败时提交 header partial 且不执行 payload。payload 失败时提交 partial tree 与诊断，但回滚 staged context。
   - 仅当 header 与 payload（或 empty case 下仅 header）均成功且 commit hooks 通过后，复合执行器才将节点转为 `Materialized`。最终 header 状态必须按终态准确标记为 `Invalid`、`WaitingDependency`、`Cancelled`、`Unsupported` 或 `Materialized`；失败结果不得描述为已完整物化的 header。

---

### 3. 变换源视图与边界错误处理

1. **变换提供者**：
   - 泛化映射合同而不是 codec policy，抽取 `PayloadTransformProvider` / `PayloadTransformRegistry`。通用 runner 只看不透明的 `view_kind`、provider result 与终态，不包含 NAL、H.264 或 H.265 名称。
   - `none` 是恒等 provider；`rbsp` 是注册能力 provider，包装现有 H.264 mapper policy，其禁止序列诊断留在通用分派器之外。未知 provider 必须返回类型化的 `Unsupported`/`InvalidDefinition`，不得静默按恒等处理。
   - provider 必须接受可能不连续的逻辑 `SourceMapping`，不能假设当前 mapper 仅接收单一连续 `SourceSpan`。

2. **区间与坐标不变量**：
   - 输入 `SourceMapping` 必须字节对齐且非空。
   - 变换后的 `SourceMapping` 只包含转发 span。排除 span（注册的 RBSP provider 中的 `0x03` 字节）单独作为 `{sourceSpan, outputBitOffset}` 记录返回，使原始选择可以高亮这些字节而不违反 `FieldLocation` 长度不变量。
   - bit 偏移算术必须严格对齐 `std::numeric_limits<quint64>::max()` 进行检查，防止坐标溢出。

3. **错误传播**：
   - Payload 解码期间遇到 `EndOfSource` / 截断触发 `DslExecutionStatus::TruncatedSource`。
   - 注入故障的 `SourceReadStatus::Error` 触发 `DslExecutionStatus::SourceError`。
   - 取消令牌检查在头部执行前、变换映射前以及 VM 指令循环中进行。
   - 资源限制拆分为共享复合 instruction/node budget 与 provider inspection budget；两者都要累计、检查溢出。取消和 source error 对当前复合操作均为终态。
   - 只有实际选中结构体分支后才扣减 provider inspection budget。`empty` 或未匹配分支不执行任何 transform inspection，因此即使 session inspection budget 已耗尽也仍可执行。
   - Transform/context resolver factory 抛出的异常按非法运行时定义处理：runner 捕获异常、将 header 标记为 `Invalid`、恰好回滚一次，并返回 `InvalidDefinition`，不得让异常越过其 API。
   - 结构型取消是否可从已提交 prefix 恢复必须有独立合同，不能未经说明继承 Annex-B “已提交 NAL 永不重试”的规则。

---

### 4. 会话级上下文生命周期管理

1. **会话归属原则**：
   - 上下文目录不能作为未托管的裸指针传递给无状态执行器。
   - P5i-3b 必须围绕现有 `RuleExecutionSession` 增加复合结构执行 API；简单连续调用两次当前单结构 `run` 不足够，因为每次调用拥有独立限制，且 session 当前绑定单一 source 指针与 tree identity（`src/rules/rule_execution_session.cpp:48-61`）。
   - context owner 的作用域是一个 source identity、package/program 与 context scope。只有在 context definition 不再依赖含义不明确的 tree-local `AnalysisNodeId`，或所有关联节点都处于同一 tree 时，才可服务多个 presentation tree。不同 source 之间不得静默共享上下文。

2. **结构型执行间的上下文生命周期**：
   - 生产者定义必须暂存，直到消费、导入、依赖 generation 与注册全部成功；payload 失败不得把生产者泄漏进目录。
   - 消费者按 source position 与精确 dependency generation 查找，而不能只依赖调用顺序。缺少或过期的生产者返回 `DependencyUnavailable`；resolver 缺失则是已观察到的独立 `InvalidDefinition`（`src/rules/dsl_vm.cpp:718-743`）。
   - context reset 必须有显式 session clear/replace API。当前固定的 `contextScopeId` 没有 reset 操作，因此 P5i-3b 必须增加该 API，或明确 replacement 是唯一 reset 机制。
   - 自动分派中的 payload context 包络校验与 resolver 构造，只能在 controller 实际选中结构体分支后、对应 payload VM 启动前进行。未选中分支的 context 要求不得拒绝 `empty` 分支或实际选中的无 context 分支。

---

### 5. 规则包入口点目标解析

我们评估了多入口包的四种潜在复用方案：

1. **方案 1：单个 `.svfmt` 内多个 `entry` 语句**：
   - 在缺乏目标命名的情况下会导致哪个 entry 处于活动状态的语法二义性。
2. **方案 2：Manifest 目标选择（`rule.toml` target 选择）**：
   - 这是选定方向，但它是 manifest schema 变更，不是只在文档中增加字段。Manifest v2 为 entrypoint 增加可选 `target = "StructName"`/`"SequenceName"`；v1 继续可读，省略 target。
   - 编译器提供统一的 `compileForTarget(program, optional target)` 操作。省略 target 时使用文件默认 `entry`；指定 target 时必须精确解析一个 struct 或 scan 并绑定 typed entry。未知 target、struct/scan 二义性、非法标识符和 target/source 不匹配必须在执行前拒绝。
   - v2 允许多个 entrypoint 共享 source path，但 `(source, target-or-default)` 组合必须唯一；`RulePackageEntryPoint` 增加 optional target，所有 analyzer 与结构型导航都使用同一 target-aware 编译路径。
3. **方案 3：独立源文件（`path = "src/h264_nal.svfmt"`）**：
   - 避免 manifest schema 工作，但可能只复制所需的结构子集，并非必然复制整个文件；若 target-aware 编译无法保持包兼容，可作为 fallback。
4. **方案 4：跨文件结构体模块导入**：
   - 对于 v0.1 而言编译器复杂度过高。

**决策**：在采用上述 v2 schema 与编译器合同的前提下，采用 **方案 2（Manifest 目标选择）**。
这使得 `org.streamview.h264` 可以从共享的 `src/h264_annex_b.svfmt` 同时暴露 `video.h264.annex-b` 与 `video.h264.nal`，而无需语法重复。修改官方 manifest 会改变 content hash，因此后续规则切片必须同步 SemVer/content version。Catalog 只负责选择 entry，target binding 属于共享编译 API，必须单独测试。

---

### 6. 流式 Annex-B 运行器收敛

- 流式 `H264AnnexBAnalyzer` 与结构型执行路径共享 transform-provider 合同及其 policy 实现，但不必共享同一 compound-session 生命周期。
- 现有 Annex-B mapper 的增量 prefix、issue、取消和 retry 语义必须通过 adapter 保留，不能机械重命名。
- 既有 Annex-B 行为是后续回归门禁；本 ADR 不声称已经重新运行任何 analyzer 计数。

---

### 7. 安全性、复杂度受限与格式中立

- **严格受限的执行**：结构型 payload 分派严格为单层（一个头部 + 至多一个 payload 结构体），禁止通用的递归调用栈。
- **格式中立的编排**：通用 dispatcher 不包含格式字段名或 FourCC。现有 context kind 与注册的 transform provider 可以携带格式 policy，但必须留在通用 dispatcher 外并显式注册。
- **受限的变换语言**：变换从声明的 provider identifier 和有限 provider 合同中选择；未知 identifier fail closed。本 ADR 不声称硬编码 `Rbsp` 枚举是格式中立的。

---

## P5i-3b 验收矩阵（计划；P5i-3a 未执行）

| 测试标识符 | 类别 | 输入 Fixture / 前置条件 | 执行路径 | 预期状态 | 关键断言 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `test_parser_structural_payload_dispatch` | DSL 解析器 / IR | 合法与非法的 `payload<rbsp> Struct switch` 声明 | `DslCompiler::compile` | 合法声明成功；非法声明产生特定诊断 | AST 与 Typed IR 正确编码 `DslPayloadTargetKind::Structure`、控制器索引与 cases。 |
| `test_structural_payload_header_and_payload_success` | 核心执行 | 包含 header + SPS 类 payload 的合成 NAL fixture | 复合结构执行 | `Materialized` | payload append 前 header 保持 `Indexing`；最终树与坐标精确。 |
| `test_structural_payload_ebsp_excluded_spans` | 变换 | 包含 `0x00000301` 且输入映射不连续的合成 NAL payload | 注册的变换 provider | `Materialized` | 转发映射与独立排除 span 记录正确；畸形序列 fail closed。 |
| `test_structural_payload_empty_and_default_cases` | 边界情况 | 控制器匹配 `empty` 分支与未处理分支 | 结构型 payload 运行器 | `Materialized` / `Unsupported` | Empty 分支生成已物化的 header 且无 payload 子节点；未处理分支发出 unsupported 诊断。 |
| `test_structural_payload_truncation_and_error` | 故障注入 | 截断的 payload 字节与注入 I/O 错误的源 | 复合结构执行 | `TruncatedSource` / `SourceError` | partial tree 保留 header；staged context 回滚；payload 节点携带匹配诊断。 |
| `test_structural_payload_cancel_and_budget` | 限制 / 取消 | 预取消、变换中取消以及低累计 header+payload 限制 | 复合结构执行 | `Cancelled` / `ResourceLimit` | 共享 instruction/node budget 与变换 inspection budget 累计且不溢出。 |
| `test_session_context_producer_consumer_chain` | 上下文生命周期 | 在同一 `RuleExecutionSession` 中先执行 SPS 生产者后执行 PPS 消费者 | 对 SPS 然后 PPS 调用 `RuleExecutionSession::run` | 两者均 `Materialized` | PPS 成功导入 SPS 上下文值（`profile_idc`、`chroma_format_idc`）。 |
| `test_session_context_missing_dependency` | 上下文生命周期 | 在无先前 SPS 生产者的前提下执行 PPS 消费者 | 仅在 PPS 上调用 `RuleExecutionSession::run` | `DependencyUnavailable` | 返回 `DependencyUnavailable` 状态并附带标明缺失上下文 ID 的诊断。 |
| `test_manifest_target_resolution` | 规则包 / Manifest | 共享 source 且 target 不同的 Manifest v2 | Catalog 加 `compileForTarget` | 每个目标均 `Found` | 未知 target、重复 `(source,target)`、省略默认 target 与 struct/scan 二义性均被拒绝。 |
| `test_context_transaction_and_tree_identity` | 上下文生命周期 | producer/payload 失败、过期 generation 与第二个 presentation tree | 复合 context session | `DependencyUnavailable` / 回滚 | 失败复合执行不发布 definition；强制 source identity，避免含义不明的 tree-local ID。 |
| `test_h264_annex_b_zero_regression` | 回归测试 | 完整的 `tests/rules/h264_annex_b_analyzer_test.cpp` 套件 | 在 Annex-B 码流上运行 `H264AnnexBAnalyzer` | `Success`（全部测试通过） | 完整 174/174 Qt 测试用例通过，AST 输出完全相同。 |
| `test_aac_mp4_zero_regression` | 回归测试 | 所有 AAC 与 MP4 分析器测试套件 | `AacAdtsAnalyzer`, `Mp4IsobmffAnalyzer` | `Success`（全部测试通过） | 所有官方格式零回归。 |

---

## 影响

### 正向影响
- **格式中立的模块化**：能够通过干净的声明式 DSL 规则解码 MP4 编解码配置载荷（`avcC` SPS/PPS），无需 C++ 格式耦合。
- **条件性的 source 复用**：Manifest v2 的 target 解析允许 `video.h264.annex-b` 与 `video.h264.nal` 共享 `src/h264_annex_b.svfmt`；是否能保持单一 source 取决于后续 schema 与编译器实现，不把“复制 1200+ 行”作为必然结论。
- **确定性的上下文传播**：`RuleExecutionSession` 提供了受审查、会话管理的 SPS->PPS 上下文解析，不污染无状态运行器。
- **统一的变换边界**：通过 provider 合同复用 EBSP/RBSP policy，同时保留流式增量语义与结构型复合执行的边界。

### 负向影响 / 权衡
- 需要扩展 DSL 解析器、IR 与编译器以支持结构型 `payload` 声明。
- 需要更新 `RulePackageCatalog`、target-aware compiler、复合 tree transaction、变换 provider、`RuleExecutionSession` 以及 staged context 生命周期；官方 manifest 变化还会改变 content hash。

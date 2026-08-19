# ADR-0103: 跨层结构型入口执行、坐标映射与导航栈合同

- **状态**: Proposed
- **日期**: 2026-08-18
- **作者**: StreamView Contributors

---

## 背景

在任务 P5h（[ADR-0102](../adr/0102-mp4-sample-descriptions-and-codec-configurations.md)）中，`org.streamview.mp4` v0.1.3 实现了样本描述（`stsd`）、样本条目（`avc1`、`mp4a`）以及编解码配置 Box（`avcC`、`esds`）。在这些结构中，具体的基本流元数据载荷被声明为带有 `@target_format` 注解的 `@lazy` 字节区域：
- `avcC` 重复的序列参数集（`sequenceParameterSetNALUnit`）与图像参数集（`pictureParameterSetNALUnit`）载荷标注为 `@target_format("video.h264.nal")`；
- `esds` `DecSpecificInfo`（`asc_bytes1..4`）载荷标注为 `@target_format("audio.aac.asc")`。

任务 P5h 严格遵循边界，仅在 AST 中生成字段元数据，不触发跨格式解码器调用。本拟议 ADR 定义任务 P5i-2 至 P5i-4 将实现的合同；在 P5i-1 基线上，拟议的运行器、可复用有界源与导航栈 API 均不存在。

### 问题分析

1. **结构型载荷与流式执行器不兼容**：
   - `org.streamview.aac` 定义了 `audio.aac.asc` 入口（`src/rules/official/org.streamview.aac/src/aac_asc.svfmt:200` 中的 `entry AudioSpecificConfig;` 及 `src/rules/official/org.streamview.aac/rule.toml:22-27`），但 `AacAdtsAnalyzer::create` 仅接受 ADTS 序列入口（`src/rules/aac_adts_analyzer.cpp:155-215`）。因此将裸 ASC 字节传给 `AacAdtsAnalyzer` 是无效的。
   - `org.streamview.h264` 仅定义带起始码扫描序列的 `video.h264.annex-b`（`src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt:1210-1225` 与 `src/rules/official/org.streamview.h264/rule.toml:14-21`）。`H264AnnexBAnalyzer::create` 拒绝非序列入口（`src/rules/h264_annex_b_analyzer.cpp:315-329`），而 MP4 `avcC` 存放的是不带 Annex B 起始码的裸 NAL 单元。
   - 当前 H.264 规则没有 `NalUnit` 结构或结构型入口，其载荷分派依附于顶层 `nal_units` 扫描。因此 P5i-2 不得声称能够完整解码 SPS/PPS；P5i-3 必须先证明并实现独立单 NAL 规则形态，否则停止并申请独立语言能力切片。
   - 将结构型数据块传递给码流扫描分析器违反了格式模型，会导致扫描失败或挂起。

2. **目标格式解析合同**：
   - `@target_format("format")` 仅携带语义格式字符串。
   - 运行时必须通过 `RulePackageCatalog::resolveByFormat`（`src/rules/rule_catalog.cpp:92-150`）检索已安装规则包，并优雅处理该按格式路径可能返回的全部状态：`MissingContent`、`VersionConflict`、`IncompatibleLanguage` 与 `IncompatibleEngine`。

3. **坐标投影与源完整性**：
   - 解码子结构时，子 AST 字段位置必须直接回映到原根文件的物理源区间（[ADR-0011](../adr/0011-dual-coordinates-and-source-mappings.md)、[ADR-0024](../adr/0024-read-source-mapped-logical-views-without-copying.md)）。
   - 禁止将载荷复制到内存缓冲区并生成脱离根文件的局部 `[0, N)` 坐标，否则将破坏源映射并导致原始数据视图高亮失效。

4. **会话与交互导航栈**：
   - 导航进入子格式必须向会话栈压入导航帧，切换活动分析树与属性检查器，并保持与原始数据视图的双向同步。
   - 从子格式返回必须平滑弹出栈帧并精确恢复父级选择。
   - 子格式解码失败绝不能破坏或丢弃父级分析树或 UI 状态。

---

## 决策

### 1. 拟议的结构型入口运行器合同（`StructuralEntryRunner`）

P5i-2 将在 `src/rules/` 中引入格式中立的结构型入口运行器。下列 API 是拟议合同，并非 P5i-1 基线已经存在的符号；它刻意复用 `dsl_vm.h` 与 `dsl_ir.h` 中现有的 `DslExecutionLimits`、`core::CancellationToken`、`DslTypedProgram` 和 `DslExecutionResult`：

```cpp
namespace streamview::rules {

struct StructuralExecutionOptions final {
    DslExecutionLimits limits;
    std::optional<core::CancellationToken> cancellation;
};

struct StructuralExecutionResult final {
    DslExecutionResult execution;
    std::shared_ptr<core::AnalysisTree> tree;

    [[nodiscard]] bool succeeded() const noexcept {
        return execution.materialized() && tree != nullptr;
    }
};

class StructuralEntryRunner final {
public:
    [[nodiscard]] static StructuralExecutionResult execute(
        const core::RandomAccessSource& baseSource,
        const core::SourceMapping& sourceMapping,
        const DslTypedProgram& program,
        const StructuralExecutionOptions& options = {});
};

} // namespace streamview::rules
```

#### 行为不变式
1. `StructuralEntryRunner` 要求 `program.entry.kind == DslEntryKind::Structure`，并在读取前校验 `program.entry.targetIndex`；序列入口以 `InvalidDefinition` 拒绝。
2. 它校验 `sourceMapping.logicalBitLength()` 非零、字节对齐且可安全转换为字节数。运行器使用现有的映射感知路径 `core::BitReader(baseSource, sourceMapping)`，无复制地读取物理 spans，并保留 VM 的 reader/mapping 一致性校验。
3. 它以 `program.entry.targetIndex`、逻辑起点 0、新建的子树及由限制和取消令牌构造的 `DslExecutionOptions` 调用 `DslExecutor::decodeStruct`（`src/rules/include/streamview/rules/dsl_executor.h:13-42`）。`BoundedSourceView` 仍是供扫描器及其它面向字节的消费者使用的可复用 `RandomAccessSource` 视图；不得在 VM reader 中叠加第二层映射。
4. 每次显式导航动作使用独立的有界执行限制与取消令牌。P5i 不递归自动展开目标格式，也不与已经完成的父分析器共享已消耗预算。
5. 它**不**实例化或调用基于扫描器的分析器（`AacAdtsAnalyzer`、`H264AnnexBAnalyzer`、`Mp4IsobmffAnalyzer`）。流式序列分析器严格保留给顶层渐进式媒体文件。

---

### 2. 目标格式解析合同

当用户或会话请求导航进入标注有 `@target_format` 的惰性节点时，引擎调用：

```cpp
RuleCatalogLookupResult result = catalog.resolveByFormat(
    targetFormat, runningLanguageVersion, runningEngineVersion);
```

查找结果映射为特定的用户可见动作与诊断：

| `RuleCatalogLookupStatus` | 内部原因 | 用户可见诊断 / UX 动作 | 导航状态 |
| :--- | :--- | :--- | :--- |
| `Found` | 精确匹配到一个已安装包及其入口点 | 继续执行编译与结构型解码 | 仅在执行成功后压入栈帧 |
| `MissingContent` | 无已安装规则包导出 `entrypoint.format == targetFormat` | 错误提示条 / 对话框：`"No installed rule package matches format '<format>'"` | 导航拒绝；保留父级状态 |
| `VersionConflict` | 多个已安装规则包导出相同格式 | 错误提示条 / 对话框：`"Multiple installed package entry points match format '<format>'"` | 导航拒绝；保留父级状态 |
| `IncompatibleLanguage` | 匹配包要求不支持的 DSL 版本 | 错误提示条 / 对话框：`"Package '<id>' requires DSL <req>, running DSL is <curr>"` | 导航拒绝；保留父级状态 |
| `IncompatibleEngine` | 匹配包要求不兼容的引擎版本 | 错误提示条 / 对话框：`"Package '<id>' requires engine <req>, running engine is <curr>"` | 导航拒绝；保留父级状态 |

`@target_format` 保持纯粹合同：仅包含标准格式字符串（如 `"video.h264.nal"`、`"audio.aac.asc"`），绝不硬编码包标识符或版本约束。

---

### 3. 坐标映射与源投影合同

为建立直接回映到根源文件的子 AST 坐标：

1. **源区间提取**：父惰性字段节点必须拥有包含非空 `sourceSpans()` 的有效 `FieldLocation`（`src/core/include/streamview/core/coordinates.h:103-121`）。
2. **对齐与字节校验**：
   - 若 `sourceSpans().empty()`，导航预检以 `InvalidTargetLocation` 失败（`"Target node has no source location"`）；由于执行尚未开始，这不是 VM 的 `InvalidSyntax` 结果。
   - 若逻辑范围或任一物理 span 未按字节对齐，导航预检以 `InvalidTargetLocation` 失败（`"Target node source location is not byte-aligned"`）。
   - 总字节长度为 `byteLength = logicalRange().bitLength() / 8`。
3. **不连续多区间缝合**：
   - `core::SourceMapping::create(childViewId, targetNode.location()->sourceSpans())` 构建逻辑视图地址空间 `[0, byteLength * 8)`；若返回 `std::nullopt`，则在激活任何子树前拒绝导航。
   - `BoundedSourceView` 按映射顺序读取所有不连续 span，并对每个请求的逻辑范围使用 `SourceMapping::locate`。
   - 逻辑 bit 偏移 0 回映到首个 span 起点；其后的所有子范围（包括跨 span 范围）都使用 `locate` 返回的完整 `FieldLocation::sourceSpans()`，不得假设只有首个 span。
4. **边界与读取错误处理**：
   - 超出 `byteLength` 的读取返回 `SourceReadStatus::EndOfSource` 并在子树上触发 `DslExecutionStatus::TruncatedSource`。
   - 底层物理 I/O 故障返回 `SourceReadStatus::Error` 并触发 `DslExecutionStatus::SourceError`。
5. **可复用视图边界**：类似的 `BoundedSourceView` 当前只作为私有类存在于 `mp4_isobmff_analyzer.cpp:84-142`。P5i-2 必须将其抽取或替换为 rules/core 可复用模块，并加入所属 CMake target；本 ADR 不声称公共视图已经存在。
6. **零复制保证**：载荷字节绝不复制到脱离的内存缓冲区中。所有读取操作均通过可复用映射视图和 `core::BitReader` 进行。

---

### 4. 拟议的会话与 UI 导航栈合同

### 4. 拟议的会话与 UI 导航栈合同

`AnalysisSession` 引入语义导航栈（`src/app/analysis_session.h`）。`MainWindow` 继续负责 `AnalysisTreeModel`、`FieldInspector`、`RawDataView`、面包屑和展示状态快照；非 `QObject` 的 session 不得发送 UI 信号，也不得直接操作 widget。

```cpp
namespace streamview::app {

struct NavigationFrame final {
    core::AnalysisNodeId parentTargetNodeId;
    QString targetFormat;
    std::shared_ptr<const rules::RulePackage> package;
    rules::RulePackageEntryPoint entryPoint;
    core::SourceMapping sourceMapping;
    std::shared_ptr<core::AnalysisTree> tree;
    core::AnalysisNodeId childRootStructureNodeId;
};

enum class AnalysisSessionNavigationStatus : quint8 {
    Entered,
    NodeNotFound,
    MissingTargetFormat,
    InvalidTargetLocation,
    MissingContent,
    VersionConflict,
    IncompatibleLanguage,
    IncompatibleEngine,
    InvalidRulePackage,
    InvalidDefinition,
    Unsupported,
    TruncatedSource,
    InvalidSyntax,
    DependencyUnavailable,
    SourceError,
    Cancelled,
    ResourceLimit,
};

struct AnalysisSessionNavigationResult final {
    AnalysisSessionNavigationStatus status = AnalysisSessionNavigationStatus::InvalidDefinition;
    std::optional<core::AnalysisNodeId> childRootStructureNodeId;
    std::shared_ptr<core::AnalysisTree> tree;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == AnalysisSessionNavigationStatus::Entered && tree != nullptr;
    }
};

enum class AnalysisSessionReturnStatus : quint8 {
    Returned,
    AtRoot,
};

struct AnalysisSessionReturnResult final {
    AnalysisSessionReturnStatus status = AnalysisSessionReturnStatus::AtRoot;
    std::optional<core::AnalysisNodeId> restoredParentTargetNodeId;
    const core::AnalysisTree* activeTree = nullptr;

    [[nodiscard]] bool returned() const noexcept {
        return status == AnalysisSessionReturnStatus::Returned;
    }
};

} // namespace streamview::app
```

`navigationStack_` 只包含子级帧。栈为空时，根分析器树处于活动状态；否则 `navigationStack_.back().tree` 为活动树。每个帧的 `parentTargetNodeId` 属于此前活动的层级，因此弹出帧后能够确定性地得到父树，以及 `MainWindow` 必须重新选中的节点。

#### 导航操作与格式中立执行

1. **进入子格式（`AnalysisSession::enterChildFormat(core::AnalysisNodeId nodeId, const rules::RulePackageCatalog& catalog, const rules::StructuralExecutionOptions& options)`）**：
   - 校验活动树中的 `nodeId` 具备 `metadata().targetFormat`。
   - 通过 `RulePackageCatalog::resolveByFormat(targetFormat, runningLanguageVersion, runningEngineVersion)` 解析目标格式。
   - 读取并编译 manifest 入口：`DslCompiler::compileForTarget(parsed.program, entryPoint.target)`。
   - 从 `targetNode.location()->sourceSpans()` 构建 `SourceMapping`。
   - 格式中立执行路由：
     - 若 `program.entry.kind == DslEntryKind::Structure` 且无结构型 payload dispatch（`!program.payloadDispatch.has_value() || program.payloadDispatchHeaderStructureIndex() != program.entry.targetIndex`）：经由 `StructuralEntryRunner::execute(source, sourceMapping, program, options)` 执行。
     - 若 `program.entry.kind == DslEntryKind::Structure` 且带有类型化 payload dispatch（`program.payloadDispatchHeaderStructureIndex() == program.entry.targetIndex`）：经由 `RuleExecutionSession::runCompound` 并开启 `autoDispatchPayload = true` 执行。header 从逻辑 0 起消费自身字节，payload 起点由 header 消费比特自动派生，transform provider 按注册的 `dispatch.viewKind` 解析，并在会话中发布/解析 context definitions。
   - 解析、预检、编译或执行失败通过 `AnalysisSessionNavigationResult` 结构化返回，且不压入栈帧。
   - **安全保证**：失败时，`activeTree()`、`navigationDepth()` 和父级状态保持完全不变。
   - 若执行成功：
     - 将 `NavigationFrame` 压入 `navigationStack_`。
     - 返回 `AnalysisSessionNavigationStatus::Entered`、子树及 `childRootStructureNodeId`。

2. **返回父级（`AnalysisSession::returnToParent()`）**：
   - 若 `navigationStack_.empty()`，操作返回 `AnalysisSessionReturnStatus::AtRoot` 作为确定性空操作。
   - 弹出顶层 `NavigationFrame`。
   - 返回 `AnalysisSessionReturnStatus::Returned`、父级目标节点标识符及当前活动父树。

3. **双向坐标交互**：
   - **树到原始视图**：选中子 `AnalysisTree` 中的任一字段，获取 `node->location()->sourceSpans()`，并将 `RawDataView` 高亮更新为原根文件的精确物理字节偏移。
   - **原始视图到树**：点击 `RawDataView` 中的字节，检索当前活动 `AnalysisTree`（若处于导航中则为子树），并自动选中包含该字节的最具体叶子节点。

#### 4.3 MainWindow 展示栈、面包屑与双向视图集成合同

在任务 P5i-4b 中，`MainWindow` 集成 `AnalysisSession` 提供的语义导航栈：

1. **关注点分离与展示所有权**：
   - `AnalysisSession`（非 `QObject`）严格拥有语义导航栈（`navigationStack_`）、子格式执行流水线与共享会话上下文，不操作 Qt widget 或维护展示模型。
   - `MainWindow` 拥有 UI 展示栈、`AnalysisTreeModel` 生命周期、`FieldInspector` 展示、`RawDataView` 选择状态、面包屑栏与 `navigationBackButton`。

2. **分析树区域紧凑导航栏**：
   - 在 `AnalysisTreeView` 上方放置紧凑导航工具栏：
     - `QToolButton`（`objectName == "navigationBackButton"`），使用标准返回图标，提供 tooltip（`"Return to parent"`）与 accessible name（`"Return to parent format"`）。在根层（`navigationDepth() == 0`）禁用，在子格式中启用。
     - 面包屑 `QLabel`（`objectName == "navigationBreadcrumbLabel"`），显示当前导航层级链（如根格式或子格式链 `video.h264.nal`、`audio.aac.asc`），不放置说明性帮助文案。
   - 导航栏尺寸紧凑，在 900x600 与 1280x800 分辨率下均不挤压、覆盖分析树表头或 Dock 面板。

3. **子格式进入触发与流程**：
   - 带有 `@target_format` 的节点可通过以下方式进入：
     - 在 `AnalysisTreeView` 中双击该节点项；
     - 在该节点被选中并聚焦时按下键盘 Enter 或 Return 键。
   - 无 `targetFormat` 的普通节点忽略上述手势，不触发导航。
   - **进入成功（`Entered`）**：
     - `MainWindow` 接收到 `status == Entered` 及 `childRootStructureNodeId` 的 `AnalysisSessionNavigationResult`；
     - `AnalysisTreeModel` 将活动树切换为 `session.activeTree()`；
     - 自动选中 `childRootStructureNodeId` 节点；
     - `FieldInspector` 刷新并展示子根结构节点属性；
     - `RawDataView` 高亮同步为子根节点的完整 `sourceSpans()`；
     - 面包屑与 `navigationBackButton` 刷新为对应层级状态。
   - **失败处理**：
     - 若 `enterChildFormat` 返回任一失败状态（`MissingContent`、`VersionConflict`、`DependencyUnavailable`、`InvalidTargetLocation` 等）：
     - `AnalysisTreeModel`、当前选择、`FieldInspector`、`RawDataView` 高亮与面包屑保持 100% 不变；
     - 仅在状态栏显示 `result.errorMessage` 中的结构化错误信息。

4. **返回父层流程**：
   - 点击 `navigationBackButton`（或触发返回动作）时：
   - 调用 `session.returnToParent()`；
   - `AtRoot` 立即作为确定性 no-op 返回；
   - `Returned`：
     - `AnalysisTreeModel` 切换为恢复后的父级 `session.activeTree()`；
     - 精确恢复对 `restoredParentTargetNodeId` 的选中；
     - `FieldInspector` 与 `RawDataView` 高亮恢复为父节点的完整 `sourceSpans()`；
     - 面包屑与 `navigationBackButton` 刷新状态（返回到根层时禁用）。

5. **双向坐标映射与不连续区间**：
   - `selectAnalysisNode` 与 `selectSourceBit` 完全在 `session.activeTree()` 上执行。
   - **Tree -> Raw View**：在活动树中选择任一节点时，`RawDataView` 高亮 `node->location()->sourceSpans()` 返回的全部物理源区间。不连续区间绝不折叠或近似为单一连续区间。
   - **Raw View -> Tree**：在 `RawDataView` 中点击物理源 bit 时，仅在 `session.activeTree()` 中检索覆盖该物理 bit 的最具体 `Materialized` 叶子节点。若活动树中无匹配节点，清除树选择与 `FieldInspector`，但保留 `RawDataView` 中刚点击的原始 bit 选择。

6. **持久化与会话重置**：
   - UI 展示栈与导航面包屑为纯进程内交互状态，不修改 `SessionDocument` schema。
   - 打开新文件时清空 UI 展示栈，清空子格式导航帧，并将视图复位至根树。

---

### 5. 持久化边界

在 StreamView v0.1 中：
- 导航栈属于进程内临时 UI 交互状态。
- `SessionDocument`（`src/app/session_document.h:40-100`）在 v0.1 中只持久化根会话状态；P5i-4 不得修改其 schema。
- 未来版本扩展会话 schema 时：
  - 导航路径条目保存 `parent_node_path` 与 `target_format`。
  - 恢复会话时若规则包缺失或父路径找不到，会话优雅回退到根树打开，不导致文档恢复失败。

---

### 6. 后续切片划分计划

为避免单次提交过于庞大并保证单一职责评审：

```
[P5i-1 (文档)]: ADR-0103 架构规范与验证合同
      │
      ▼
[P5i-2 (规则/核心)]: 可复用映射源视图 + StructuralEntryRunner 与 CTest
      │
      ▼
[P5i-3 (规则/包)]: 独立 H.264 NAL 规则形态/入口 + AAC ASC 验证
      │
      ▼
[P5i-4a (应用/核心)]: AnalysisSession 语义导航栈与格式中立子格式执行
      │
      ▼
[P5i-4b (应用/UI)]: MainWindow 面包屑、树切换与双向坐标高亮
```

- **任务 P5i-1**：纯 Markdown 双语 ADR-0103 规范。
- **任务 P5i-2**：实现可复用映射源视图与 `StructuralEntryRunner`。
- **任务 P5i-3**（P5i-3a/3b/3c）：独立 H.264 NAL 规则包入口 `video.h264.nal`、复合结构执行与 RBSP payload transform provider。
- **任务 P5i-4a**：在 `AnalysisSession` 中实现非 `QObject` `NavigationFrame` 与语义导航栈，实现 AAC ASC 与 H.264 独立 NAL 格式中立子格式执行及会话上下文共享。
- **任务 P5i-4b**：实现 `MainWindow` 面包屑栏、树模型替换、选择快照及双向坐标同步。
  1. 结构型 `switch` 分支不能实例化子结构。`tests/probes/p5i3/probe_q1_structural_switch.svfmt` 在 `src/rules/dsl.cpp:1000-1004` 复现 `Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type`。
  2. `payload<rbsp>` 绑定已声明的 scan 序列，并要求 entry 指向该序列。`tests/probes/p5i3/probe_q2_unbound_payload.svfmt` 与 `probe_q2_sequence_entry_mismatch.svfmt` 复现 `src/rules/dsl.cpp:3763-3781` 的两条诊断。现有 EBSP/RBSP mapper 仅由 `H264AnnexBAnalyzer`（`src/rules/h264_annex_b_analyzer.cpp:666-693`）实例化；`StructuralEntryRunner` 没有转换钩子。
  3. 独立 PPS 执行无法通过当前运行器解析 SPS import：`StructuralEntryRunner::execute` 没有提供 context resolver（`src/rules/structural_entry_runner.cpp:87-103`），VM 返回 `InvalidDefinition` 及 `Imported context value resolver is unavailable`（`src/rules/dsl_vm.cpp:718-743`）。该限制适用于 PPS 和依赖它的 slice，不适用于没有 import 的孤立 SPS。
  4. 保持运行器格式中立意味着该缺口必须由新的通用 DSL/运行时能力解决，不能向 C++ 加入 H.264 专属分派；当前运行器本身没有格式专属分派。
  5. 一个 `.svfmt` 程序只接受一个 `entry`（`tests/probes/p5i3/probe_q5_multiple_entries.svfmt`、`src/rules/dsl.cpp:1758-1777`）。manifest 已支持多个 entrypoint，但每个 entry 必须使用不同 source 路径（`src/rules/rule_package.cpp:371-424`）；仓库没有跨文件结构导入。因此不能严格声称新入口必然复制整个 Annex-B 文件。
  根据停止与上报协议，任务 P5i-3 停止规则与生产代码实现，不修改 `org.streamview.h264`（保持 v0.1.39）且不增加 `video.h264.nal`。现有 `org.streamview.aac` v0.1.4 `audio.aac.asc` 已通过 bundled package、`resolveByFormat`、manifest source 和 `StructuralEntryRunner` 验证，继续作为 P5i-4 导航基线。
- **任务 P5i-3a**：Docs-first 设计结构型 payload 入口所需的最小通用能力：header/controller 解析、映射字节转换钩子、payload 结构分派和 session-owned context resolver。本能力切片不得实现官方 H.264 规则包。
- **任务 P5i-4**：在 `AnalysisSession` 中实现 `NavigationStack`，连接 `MainWindow` 面包屑导航，并使用已验证的 `audio.aac.asc` 路径测试树与原始视图的双向坐标同步。

---

## 验证矩阵

| 测试标识符 | 类别 | 输入 Fixture / 前置条件 | 执行路径 | 预期状态 | 关键断言 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `executesOfficialAacAscOnEsdsFixture` | P5i-2 核心执行 | `mp4_p5h_mp4a_esds.mp4`（`asc_bytes1`，2 字节 `0x12 0x10`） | 直接编译现有 `AudioSpecificConfig` source 并执行 `StructuralEntryRunner` | `DslExecutionStatus::Materialized` | 完整有序六字段列表与六个精确根 bit spans `[1168,1173)`、`[1173,1177)`、`[1177,1181)`、`[1181,1182)`、`[1182,1183)`、`[1183,1184)`。 |
| `executesCatalogResolvedAacAscOnEsdsFixture` | P5i-3 包验证 | 同一 ASC fixture | bundled AAC v0.1.4 -> `resolveByFormat("audio.aac.asc")` -> manifest source -> `StructuralEntryRunner` | `DslExecutionStatus::Materialized` | 断言包/版本、`asc` entry 元数据、完整 child 列表、`audio_object_type == 2` 及首字段根 span。 |
| `resolvesPackageByFormat` | Catalog 查找回归 | 合成规则包 catalog 场景 | `RulePackageCatalog::resolveByFormat` | `Found`、`MissingContent`、`IncompatibleLanguage`、`IncompatibleEngine` | 现有规则包测试断言各状态及诊断。 |
| `rejectsAmbiguousPackagesWhenResolvingByFormat` | Catalog 查找回归 | 同 format 的两个已安装版本 | `RulePackageCatalog::resolveByFormat` | `VersionConflict` | 现有规则包测试拒绝歧义内容且不返回包。 |
| `p5i3_h264_standalone_probes` | P5i-3 可表达性门禁 | `tests/probes/p5i3/` 下四个已提交 `.svfmt` 探针 | `svtool rule check`，预期失败 | 负向诊断 | Q1/Q2/Q5 诊断可复现；H.264 SPS/PPS 物化在 P5i-3a 前明确阻断。 |
| `test_bounded_source_view_truncated` | P5i-2 坐标/源 | 通过单字节映射暴露两字节 ASC | `StructuralEntryRunner` 执行 | `DslExecutionStatus::TruncatedSource` | 产生 `TruncatedSource` 诊断；未激活的部分节点保留精确源区间。 |
| `test_bounded_source_view_io_fault` | P5i-2 坐标/源 | 注入故障的 `RandomAccessSource` | `StructuralEntryRunner` 执行 | `DslExecutionStatus::SourceError` | 返回 `SourceError`；任一部分树保持未激活且父级不受影响。 |
| `test_navigation_enter_and_return_cycle` | P5i-4 会话/UI | `mp4_p5h_mp4a_esds.mp4` 的 `AnalysisSession` | 调用 `enterChildFormat(ascNodeId)` 后调用 `returnToParent()` | 成功 | 栈深度经历 0 -> 1 -> 0；活动树切换至 ASC AST 后返回 MP4 AST；父级选中节点恢复。 |
| `test_navigation_enter_failure_preserves_parent` | P5i-4 会话/UI | 目标节点携带无效/缺失格式 `"invalid.format"` | 调用 `enterChildFormat(nodeId)` | `MissingContent` / 失败 | 返回结构化失败；栈深度保持 0；父树与当前选择不变。 |
| `test_bidirectional_coordinate_selection` | P5i-4 坐标/UI | 选中子 ASC 树节点 | 检查 `FieldLocation` 源区间 | 成功 | ASC 载荷映射到已提交 fixture 的根字节 `[146, 148)`；选择绝对源 bit 1173 时选中 `AudioSpecificConfig.sampling_frequency_index`。 |
| `test_nested_navigation_independent_limits` | P5i-4 预算/取消 | 使用较低子级限制显式触发嵌套导航 | 第二次由用户触发的子执行 | `ResourceLimit` | 每次导航动作独立执行自身限制；子级取消/资源失败后，已完成的父帧仍可使用。 |

---

## 影响

### 正向影响
- **格式中立的模块化设计**：基本流格式（H.264 NAL、AAC ASC）由纯声明式规则解码，不污染 MP4 分析核心。
- **精确的源溯源**：子树中解码出的每个语法字段均直接高亮容器原文件中的物理字节。
- **健壮的 UI 交互体验**：清晰的面包屑导航、完全的失败隔离以及无缝的双向坐标映射。

### 负向影响 / 权衡
- 需要引入 `StructuralEntryRunner`，并在现有规则形态不足时为相关官方包增加结构型入口。

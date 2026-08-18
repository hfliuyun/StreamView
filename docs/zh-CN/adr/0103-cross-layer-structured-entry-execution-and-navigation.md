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
2. 它校验 `sourceMapping.logicalBitLength()` 非零、字节对齐且可安全转换为字节数，再在该精确逻辑长度上创建可复用的映射 `BoundedSourceView`。
3. 它在映射视图上初始化 `core::BitReader`，并以 `program.entry.targetIndex`、逻辑起点 0、新建的子树及由限制和取消令牌构造的 `DslExecutionOptions` 调用 `DslExecutor::decodeStruct`（`src/rules/include/streamview/rules/dsl_executor.h:13-42`）。
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

P5i-1 基线的 `AnalysisSession` 尚无导航 API（`src/app/analysis_session.h:64-177`）。P5i-4 将增加下述语义导航栈。`MainWindow` 继续负责 `AnalysisTreeModel`、`FieldInspector`、`RawDataView`、面包屑和展示状态快照；非 `QObject` 的 session 不得发送原草案中的信号，也不得直接操作 widget。

```cpp
namespace streamview::app {

struct NavigationFrame final {
    core::AnalysisNodeId parentTargetNodeId;
    QString targetFormat;
    std::shared_ptr<const rules::RulePackage> package;
    rules::RulePackageEntryPoint entryPoint;
    core::SourceMapping sourceMapping;
    std::shared_ptr<core::AnalysisTree> tree;
};

} // namespace streamview::app
```

`navigationStack_` 只包含子级帧。栈为空时，根分析器树处于活动状态；否则 `navigationStack_.back().tree` 为活动树。每个帧的 `parentTargetNodeId` 属于此前活动的层级，因此弹出帧后能够确定性地得到父树，以及 `MainWindow` 必须重新选中的节点。

#### 导航操作

1. **进入子格式（`AnalysisSession::enterChildFormat(core::AnalysisNodeId nodeId, const RulePackageCatalog& catalog)`）**：
   - 校验活动树中的 `nodeId` 具备 `metadata().targetFormat`。
   - 通过 `RulePackageCatalog::resolveByFormat` 解析目标格式。
   - 构建 `SourceMapping` 并执行 `StructuralEntryRunner::execute`。
   - 解析、预检、编译或执行失败通过结构化结果返回，且不压入栈帧。失败执行可以保留未激活的部分树用于诊断，但绝不替换活动树。
   - **安全保证**：失败时，`MainWindow` 保持当前 model、选择、`FieldInspector` 和原始视图高亮不变，并展示返回的错误。
   - 若执行成功：
     - 将 `NavigationFrame` 压入 `navigationStack_`。
     - 向 `MainWindow` 返回子树及其根结构节点。
     - `MainWindow` 记录父节点/选择快照、切换 `AnalysisTreeModel`、选中子树根节点并更新面包屑。

2. **返回父级（`AnalysisSession::returnToParent()`）**：
   - 若 `navigationStack_.empty()`，操作为空操作（no-op）。
   - 弹出顶层 `NavigationFrame`。
   - 向 `MainWindow` 返回父级目标节点标识符和当前活动父树。
   - `MainWindow` 恢复保存的父级选择，并根据恢复节点的位置重新派生原始视图高亮。

3. **双向坐标交互**：
   - **树到原始视图**：选中子 `AnalysisTree` 中的任一字段，获取 `node->location()->sourceSpans()`，并将 `RawDataView` 高亮更新为原根文件的精确物理字节偏移。
   - **原始视图到树**：点击 `RawDataView` 中的字节，检索当前活动 `AnalysisTree`（若处于导航中则为子树），并自动选中包含该字节的最具体叶子节点。

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
[P5i-4 (应用/UI)]: AnalysisSession NavigationStack、MainWindow 面包屑与双向选择
```

- **任务 P5i-1**（本任务）：纯 Markdown 双语 ADR-0103 规范。
- **任务 P5i-2**：实现可复用映射源视图与 `StructuralEntryRunner`，使用本地结构型规则和现有 `audio.aac.asc` 入口证明通用路径。本能力切片不得修改官方包 manifest，也不得声称完整 H.264 NAL 解码。
- **任务 P5i-3**：Docs-first 探测独立单 NAL 规则形态；若可表达，则为 `org.streamview.h264` 增加 `video.h264.nal` 并在规则输出实际变化时从 v0.1.39 升至 v0.1.40。若现有语言无法脱离扫描序列表达 header + RBSP 分派，则停止并申请独立能力切片，禁止增加 H.264 专属 C++ dispatch。现有 `org.streamview.aac` v0.1.4 ASC 入口按原样验证，只有本切片实际修改其 manifest 或解码输出时才升级。
- **任务 P5i-4**：在 `AnalysisSession` 中实现 `NavigationStack`，连接 `MainWindow` 面包屑导航，并测试树与原始视图的双向坐标同步。

---

## 验证矩阵

| 测试标识符 | 类别 | 输入 Fixture / 前置条件 | 执行路径 | 预期状态 | 关键断言 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `test_structural_runner_aac_asc_success` | P5i-2 核心执行 | `mp4_p5h_mp4a_esds.mp4`（`asc_bytes1`，2 字节 `0x12 0x10`） | 使用现有 `AudioSpecificConfig` 入口执行 `StructuralEntryRunner` | `DslExecutionStatus::Materialized` | 根节点名为 `AudioSpecificConfig`；`audio_object_type == 2`、`sampling_frequency_index == 4`、`channel_configuration == 2`；源区间与父级字节偏移精确吻合。 |
| `test_structural_runner_h264_sps_nal_success` | P5i-3 规则集成 | `mp4_p5h_avc1_avcC.mp4`（`sequenceParameterSetNALUnit[0]`，25 字节） | 由 `video.h264.nal` 选择未来的独立单 NAL 入口；不假设当前存在 `NalUnit` 结构 | `DslExecutionStatus::Materialized` | `nal_unit_type == 7`、`profile_idc == 100`、`level_idc == 41`；子字段源区间与根 SPS 字节偏移精确吻合。 |
| `test_structural_runner_h264_pps_nal_success` | P5i-3 规则集成 | `mp4_p5h_avc1_avcC.mp4`（`pictureParameterSetNALUnit[0]`，4 字节） | 由 `video.h264.nal` 选择未来的独立单 NAL 入口 | `DslExecutionStatus::Materialized` | `nal_unit_type == 8`、`pic_parameter_set_id == 0`、`seq_parameter_set_id == 0`；源区间与根 PPS 字节偏移精确吻合。 |
| `test_resolve_by_format_all_statuses` | Catalog 查找 | 格式：`"audio.aac.asc"`、`"video.h264.nal"`、`"unknown.fmt"`、模拟冲突、不兼容版本 | `RulePackageCatalog::resolveByFormat` | `Found`、`MissingContent`、`VersionConflict`、`IncompatibleLanguage`、`IncompatibleEngine` | 返回正确查找状态与非空诊断信息。 |
| `test_bounded_source_view_truncated` | P5i-2 坐标/源 | 通过单字节映射暴露两字节 ASC | `StructuralEntryRunner` 执行 | `DslExecutionStatus::TruncatedSource` | 产生 `TruncatedSource` 诊断；未激活的部分节点保留精确源区间。 |
| `test_bounded_source_view_io_fault` | P5i-2 坐标/源 | 注入故障的 `RandomAccessSource` | `StructuralEntryRunner` 执行 | `DslExecutionStatus::SourceError` | 返回 `SourceError`；任一部分树保持未激活且父级不受影响。 |
| `test_navigation_enter_and_return_cycle` | P5i-4 会话/UI | `mp4_p5h_avc1_avcC.mp4` 的 `AnalysisSession` | 调用 `enterChildFormat(spsNodeId)` 后调用 `returnToParent()` | 成功 | 栈深度经历 0 -> 1 -> 0；活动树切换至 SPS AST 后返回 MP4 AST；父级选中节点恢复。 |
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

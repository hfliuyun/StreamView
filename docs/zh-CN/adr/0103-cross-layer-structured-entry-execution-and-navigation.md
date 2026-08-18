# ADR-0103: 跨层结构型入口执行、坐标映射与导航栈合同

- **状态**: Proposed
- **日期**: 2026-08-18
- **作者**: StreamView Contributors

---

## 背景

在任务 P5h（[ADR-0102](../adr/0102-mp4-sample-descriptions-and-codec-configurations.md)）中，`org.streamview.mp4` v0.1.3 实现了样本描述（`stsd`）、样本条目（`avc1`、`mp4a`）以及编解码配置 Box（`avcC`、`esds`）。在这些结构中，具体的基本流元数据载荷被声明为带有 `@target_format` 注解的 `@lazy` 字节区域：
- `avcC` 重复的序列参数集（`sequenceParameterSetNALUnit`）与图像参数集（`pictureParameterSetNALUnit`）载荷标注为 `@target_format("video.h264.nal")`；
- `esds` `DecSpecificInfo`（`asc_bytes1..4`）载荷标注为 `@target_format("audio.aac.asc")`。

任务 P5h 严格遵循边界，仅在 AST 中生成字段元数据，不触发跨格式解码器调用。任务 P5i 负责实现这些目标格式的运行时执行、坐标回映以及交互式 UI 导航栈。

### 问题分析

1. **结构型载荷与流式执行器不兼容**：
   - `org.streamview.aac` 在 [`src/rules/official/org.streamview.aac/src/aac_asc.svfmt:200`](file:///Users/yun/code/streamview/src/rules/official/org.streamview.aac/src/aac_asc.svfmt#L200) 和 [`src/rules/official/org.streamview.aac/rule.toml:23-28`](file:///Users/yun/code/streamview/src/rules/official/org.streamview.aac/rule.toml#L23-L28) 中定义了 `audio.aac.asc` 入口（`entry AudioSpecificConfig;`），但 [`AacAdtsAnalyzer`](file:///Users/yun/code/streamview/src/rules/aac_adts_analyzer.cpp#L157) 仅能执行连续 ADTS 同步字扫描序列。将裸 ASC 字节传递给 `AacAdtsAnalyzer` 会立即失败。
   - `org.streamview.h264` 在 [`src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt:1210-1226`](file:///Users/yun/code/streamview/src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt#L1210-L1226) 和 [`src/rules/official/org.streamview.h264/rule.toml:14-21`](file:///Users/yun/code/streamview/src/rules/official/org.streamview.h264/rule.toml#L14-L21) 中仅定义了带有起始码扫描序列的 `video.h264.annex-b`。`H264AnnexBAnalyzer` 要求 3/4 字节起始码（`0x000001` / `0x00000001`），而 MP4 `avcC` 中存放的是无 Annex B 起始码的定长/变长裸 NAL 单元。
   - 将结构型数据块传递给码流扫描分析器违反了格式模型，会导致扫描失败或挂起。

2. **目标格式解析合同**：
   - `@target_format("format")` 仅携带语义格式字符串。
   - 运行时必须通过 [`RulePackageCatalog::resolveByFormat`](file:///Users/yun/code/streamview/src/rules/rule_catalog.cpp#L92-L150) 检索已安装规则包，并优雅处理所有解析状态（`MissingContent`、`VersionConflict`、`IncompatibleLanguage`、`IncompatibleEngine`）。

3. **坐标投影与源完整性**：
   - 解码子结构时，子 AST 字段位置必须直接回映到原根文件的物理源区间（[ADR-0011](../adr/0011-dual-coordinates-and-source-mappings.md)、[ADR-0024](../adr/0024-read-source-mapped-logical-views-without-copying.md)）。
   - 禁止将载荷复制到内存缓冲区并生成脱离根文件的局部 `[0, N)` 坐标，否则将破坏源映射并导致原始数据视图高亮失效。

4. **会话与交互导航栈**：
   - 导航进入子格式必须向会话栈压入导航帧，切换活动分析树与属性检查器，并保持与原始数据视图的双向同步。
   - 从子格式返回必须平滑弹出栈帧并精确恢复父级选择。
   - 子格式解码失败绝不能破坏或丢弃父级分析树或 UI 状态。

---

## 决策

### 1. 结构型入口运行器合同（`StructuralEntryRunner`）

我们在 `src/rules/` 中引入格式中立的结构型入口运行器（`streamview::rules::StructuralEntryRunner`）：

```cpp
namespace streamview::rules {

struct StructuralExecutionOptions final {
    RunnerLimits limits;
    core::CancellationSource::Token cancellation;
};

struct StructuralExecutionResult final {
    DslExecutionStatus status = DslExecutionStatus::InvalidDefinition;
    std::shared_ptr<core::AnalysisTree> tree;
    std::optional<core::AnalysisNodeId> rootStructNode;
    std::size_t nodesCreated = 0;
    std::size_t instructionsExecuted = 0;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == DslExecutionStatus::Complete && tree != nullptr && rootStructNode.has_value();
    }
};

class StructuralEntryRunner final {
public:
    [[nodiscard]] static StructuralExecutionResult execute(
        const core::RandomAccessSource& baseSource,
        const core::SourceMapping& sourceMapping,
        quint64 byteLength,
        const DslProgram& program,
        quint32 entryStructIndex,
        const StructuralExecutionOptions& options = {});
};

} // namespace streamview::rules
```

#### 行为不变式
1. `StructuralEntryRunner` 接收 `baseSource`、`sourceMapping` 和 `byteLength`，在精确映射的源区间上创建 `BoundedSourceView`。
2. 它初始化绑定到 `BoundedSourceView` 的 `core::BitReader`，并直接针对 `entryStructIndex` 执行 [`DslExecutor::decodeStruct`](file:///Users/yun/code/streamview/src/rules/include/streamview/rules/dsl_executor.h#L58-L75)。
3. 它**不**实例化或调用基于扫描器的分析器（`AacAdtsAnalyzer`、`H264AnnexBAnalyzer`、`Mp4IsobmffAnalyzer`）。流式序列分析器严格保留给顶层渐进式媒体文件。

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
| `Found` | 精确匹配到一个已安装包及其入口点 | 继续执行编译与结构型解码 | 压入新 `NavigationFrame` |
| `MissingContent` | 无已安装规则包导出 `entrypoint.format == targetFormat` | 错误提示条 / 对话框：`"No installed rule package matches format '<format>'"` | 导航拒绝；保留父级状态 |
| `VersionConflict` | 多个已安装规则包导出相同格式 | 错误提示条 / 对话框：`"Multiple installed package entry points match format '<format>'"` | 导航拒绝；保留父级状态 |
| `IncompatibleLanguage` | 匹配包要求不支持的 DSL 版本 | 错误提示条 / 对话框：`"Package '<id>' requires DSL <req>, running DSL is <curr>"` | 导航拒绝；保留父级状态 |
| `IncompatibleEngine` | 匹配包要求不兼容的引擎版本 | 错误提示条 / 对话框：`"Package '<id>' requires engine <req>, running engine is <curr>"` | 导航拒绝；保留父级状态 |

`@target_format` 保持纯粹合同：仅包含标准格式字符串（如 `"video.h264.nal"`、`"audio.aac.asc"`），绝不硬编码包标识符或版本约束。

---

### 3. 坐标映射与源投影合同

为建立直接回映到根源文件的子 AST 坐标：

1. **源区间提取**：父惰性字段节点必须拥有包含非空 `sourceSpans()` 的有效 [`FieldLocation`](file:///Users/yun/code/streamview/src/core/include/streamview/core/coordinates.h#L103-L121)。
2. **对齐与字节校验**：
   - 若 `sourceSpans().empty()`，导航以 `InvalidSyntax` 失败（`"Target node has no source location"`）。
   - 若 `logicalRange().bitLength() % 8 != 0`，导航以 `InvalidSyntax` 失败（`"Target node source location is not byte-aligned"`）。
   - 总字节长度为 `byteLength = logicalRange().bitLength() / 8`。
3. **不连续多区间缝合**：
   - `core::SourceMapping::create(childViewId, targetNode.location()->sourceSpans())` 构建逻辑视图地址空间 `[0, byteLength * 8)`。
   - `BoundedSourceView` 按照逻辑顺序透明跨越所有不连续物理区间进行读取。
   - 子 AST 中的逻辑 bit 偏移 0 自动回映到 `targetNode.location()->sourceSpans().front().start()`。
4. **边界与读取错误处理**：
   - 超出 `byteLength` 的读取返回 `SourceReadStatus::EndOfSource` 并在子树上触发 `DslExecutionStatus::TruncatedSource`。
   - 底层物理 I/O 故障返回 `SourceReadStatus::Error` 并触发 `DslExecutionStatus::SourceError`。
5. **零复制保证**：载荷字节绝不复制到脱离的内存缓冲区中。所有读取操作均通过 `BoundedSourceView` 和 `core::BitReader` 进行。

---

### 4. 会话与 UI 导航栈合同

导航栈由 [`AnalysisSession`](file:///Users/yun/code/streamview/src/app/analysis_session.h#L64-L177) 拥有：

```cpp
namespace streamview::app {

struct NavigationFrame final {
    core::AnalysisNodeId parentTargetNodeId;
    QString targetFormat;
    std::shared_ptr<const rules::RulePackage> package;
    rules::RulePackageEntryPoint entryPoint;
    core::SourceMapping sourceMapping;
    std::shared_ptr<core::AnalysisTree> tree;
    std::optional<core::AnalysisNodeId> lastSelectedNodeId;
    std::optional<core::SourceSelection> lastSourceSelection;
};

} // namespace streamview::app
```

#### 导航操作

1. **进入子格式（`AnalysisSession::enterChildFormat(core::AnalysisNodeId nodeId)`）**：
   - 校验活动树中的 `nodeId` 具备 `metadata().targetFormat`。
   - 通过 `RulePackageCatalog::resolveByFormat` 解析目标格式。
   - 构建 `SourceMapping` 并执行 `StructuralEntryRunner::execute`。
   - 若执行产生 `ResourceLimit`、`Cancelled`、`SourceError`、`InvalidRule` 或 `MissingContent`：
     - 发送 `childNavigationFailed(QString errorMessage)` 信号。
     - **安全保证**：不压入栈帧；完全保持活动树、当前选择与 `FieldInspector` 不变。
   - 若执行成功：
     - 在当前活动帧中记录当前选择。
     - 将 `NavigationFrame` 压入 `navigationStack_`。
     - 更新 `AnalysisTreeModel` 以展示子树。
     - 将活动选择设置为子树根结构节点。
     - 发送 `navigationStackChanged()` 信号。

2. **返回父级（`AnalysisSession::returnToParent()`）**：
   - 若 `navigationStack_.empty()`，操作为空操作（no-op）。
   - 弹出顶层 `NavigationFrame`。
   - 在 `AnalysisTreeModel` 中恢复父帧的 `AnalysisTree`。
   - 恢复 `lastSelectedNodeId` 与原始数据选择高亮。
   - 发送 `navigationStackChanged()` 信号。

3. **双向坐标交互**：
   - **树到原始视图**：选中子 `AnalysisTree` 中的任一字段，获取 `node->location()->sourceSpans()`，并将 `RawDataView` 高亮更新为原根文件的精确物理字节偏移。
   - **原始视图到树**：点击 `RawDataView` 中的字节，检索当前活动 `AnalysisTree`（若处于导航中则为子树），并自动选中包含该字节的最具体叶子节点。

---

### 5. 持久化边界

在 StreamView v0.1 中：
- 导航栈属于进程内临时 UI 交互状态。
- [`SessionDocument`](file:///Users/yun/code/streamview/src/app/session_document.h#L40-L100) 持久化根分析文件、校验指纹、根级书签与注释。
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
[P5i-2 (规则/核心)]: BoundedSourceView + StructuralEntryRunner 实现与 CTest
      │
      ▼
[P5i-3 (规则/包)]: H.264 video.h264.nal 入口与 AAC ASC 结构型 Manifest 升级
      │
      ▼
[P5i-4 (应用/UI)]: AnalysisSession NavigationStack、MainWindow 面包屑与双向选择
```

- **任务 P5i-1**（本任务）：纯 Markdown 双语 ADR-0103 规范。
- **任务 P5i-2**：实现 `StructuralEntryRunner`、源坐标投影以及 `streamview_dsl_executor_tests` 单元测试。
- **任务 P5i-3**：在 `org.streamview.h264` v0.1.40 中新增 `video.h264.nal` 结构型入口，并验证 `org.streamview.aac` v0.1.5 结构型执行。
- **任务 P5i-4**：在 `AnalysisSession` 中实现 `NavigationStack`，连接 `MainWindow` 面包屑导航，并测试树与原始视图的双向坐标同步。

---

## 验证矩阵

| 测试标识符 | 类别 | 输入 Fixture / 前置条件 | 执行路径 | 预期状态 | 关键断言 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `test_structural_runner_aac_asc_success` | 核心执行 | `mp4_p5h_mp4a_esds.mp4`（`asc_bytes1`，2 字节 `0x12 0x10`） | 针对 `AudioSpecificConfig` 调用 `StructuralEntryRunner` | `DslExecutionStatus::Complete` | 根节点名为 `AudioSpecificConfig`；`audio_object_type == 2`、`sampling_frequency_index == 4`、`channel_configuration == 2`；源区间与父级字节偏移精确吻合。 |
| `test_structural_runner_h264_sps_nal_success` | 核心执行 | `mp4_p5h_avc1_avcC.mp4`（`sequenceParameterSetNALUnit[0]`，25 字节） | 针对 `NalUnit` 调用 `StructuralEntryRunner` | `DslExecutionStatus::Complete` | `nal_unit_type == 7`、`profile_idc == 100`、`level_idc == 41`；子字段源区间与根 SPS 字节偏移精确吻合。 |
| `test_structural_runner_h264_pps_nal_success` | 核心执行 | `mp4_p5h_avc1_avcC.mp4`（`pictureParameterSetNALUnit[0]`，4 字节） | 针对 `NalUnit` 调用 `StructuralEntryRunner` | `DslExecutionStatus::Complete` | `nal_unit_type == 8`、`pic_parameter_set_id == 0`、`seq_parameter_set_id == 0`；源区间与根 PPS 字节偏移精确吻合。 |
| `test_resolve_by_format_all_statuses` | Catalog 查找 | 格式：`"audio.aac.asc"`、`"video.h264.nal"`、`"unknown.fmt"`、模拟冲突、不兼容版本 | `RulePackageCatalog::resolveByFormat` | `Found`、`MissingContent`、`VersionConflict`、`IncompatibleLanguage`、`IncompatibleEngine` | 返回正确查找状态与非空诊断信息。 |
| `test_bounded_source_view_truncated` | 坐标/源 | 25 字节 SPS 被 `BoundedSourceView` 限制为 10 字节 | `StructuralEntryRunner` 执行 | `DslExecutionStatus::TruncatedSource` | 产生 `TruncatedSource` 诊断；已解析部分节点保留精确源区间。 |
| `test_bounded_source_view_io_fault` | 坐标/源 | 注入故障的 `RandomAccessSource` | `StructuralEntryRunner` 执行 | `DslExecutionStatus::SourceError` | 返回 `SourceError` 状态；树构建安全中止。 |
| `test_navigation_enter_and_return_cycle` | 会话/UI | `mp4_p5h_avc1_avcC.mp4` 的 `AnalysisSession` | 调用 `enterChildFormat(spsNodeId)` 后调用 `returnToParent()` | Complete | 栈深度经历 0 -> 1 -> 0 切换；活动树在 SPS AST 与 MP4 AST 间切换；父级选中节点精确恢复。 |
| `test_navigation_enter_failure_preserves_parent` | 会话/UI | 目标节点携带无效/缺失格式 `"invalid.format"` | 调用 `enterChildFormat(nodeId)` | `MissingContent` / Failure | 发送 `childNavigationFailed`；栈深度保持 0；父树与当前选择完全不变。 |
| `test_bidirectional_coordinate_selection` | 坐标/UI | 选中子 ASC 树节点 | 检查 `FieldLocation` 源区间 | Complete | 子节点源区间对应根文件字节偏移 `[184, 186)`；点击根文件第 185 字节选中 `AudioSpecificConfig.sampling_frequency_index`。 |
| `test_multilevel_navigation_budget_sharing` | 预算/限制 | 节点/指令预算较低的多层嵌套导航 | 递归子格式执行 | `ResourceLimit` | 跨层扣减共享预算；严格强制执行上限，无堆溢出。 |

---

## 影响

### 正向影响
- **格式中立的模块化设计**：基本流格式（H.264 NAL、AAC ASC）由纯声明式规则解码，不污染 MP4 分析核心。
- **精确的源溯源**：子树中解码出的每个语法字段均直接高亮容器原文件中的物理字节。
- **健壮的 UI 交互体验**：清晰的面包屑导航、完全的失败隔离以及无缝的双向坐标映射。

### 负向影响 / 权衡
- 需要引入 `StructuralEntryRunner` 并在各官方规则包中定义结构型入口点。

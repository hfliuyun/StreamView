# ADR-0108: 时间线样本导航、Dock 集成与格式歧义呈现

- **状态**: Accepted
- **日期**: 2026-09-06
- **作者**: StreamView 贡献者

---

## 背景

阶段 5 实现了非分片 ISO BMFF MP4/MOV 容器检查、元数据树物化、`mdat` 惰性封装、窗口化样本表解析，以及从媒体样本跨层导航到底层编解码器基元流。

在 Task P5j-4 中，`AnalysisSession` 建立了底层样本导航引擎：
1. `tracks()`：提取并返回索引轨道（`AnalysisSessionTracksResult`）。
2. `samplesForTrack(request)`：返回窗口化 `SampleDescriptor` 页面（`AnalysisSessionSamplePageResult`）。
3. `enterSample(trackId, sampleIndex, catalog, options)`：针对解析后的规则包（`video.h264.nal`、`audio.aac.asc`）执行样本负载，建立 `SampleNavigationFrame`，并公开样本子树。
4. `returnToParent()`：展开导航栈并提供 `restoredSample` 以支持 UI 恢复。

### Task P5j-5 识别的需求与差距

1. **时间线与样本导航 Dock UI**：
   - 用户需要在 `MainWindow` 底部拥有一个专用的停靠部件（`timelineDock`），用于展示轨道列表、分页样本、时间线坐标（DTS、PTS、duration）、同步样本/关键帧徽标以及字节跨度。
   - UI 内存占用必须有界：样本表必须通过 `AnalysisSession::samplesForTrack` 进行虚拟化与分页（例如每页 256 个样本），防止在大型文件上无界分配节点。
2. **交互式跨层样本导航**：
   - 双击或在样本行上按回车键必须调用 `AnalysisSession::enterSample`。
   - `AnalysisTreeModel` 中的活动树必须无缝切换到样本访问单元子树。
   - 面包屑必须清晰反映容器到样本的路径：例如 `video.mp4 > Track 1 (video/mp4) > Sample #0 [Sync]`。
   - 点击 `navigationBackButton`（`returnToParent`）必须恢复容器树，重新选中源轨道与样本行，并在不丢失上下文的情况下将表格滚动至恢复位置。
3. **双向高亮同步**：
   - 在时间线表格中选中样本行必须在 `RawDataView` 中高亮其物理 `SourceSpan`。
   - 从样本返回时，在 `RawDataView` 中恢复样本负载区间的物理高亮。
4. **格式歧义与裁定呈现（ADR-0106 与复审裁定）**：
   - `FormatSelection::ambiguous()` 与 `decided()` 必须在 `MainWindow` UI 表面显式消费。若仲裁遇到具有竞争力的锚定候选（例如 `AmbiguousContainerVersusElementaryStream`），必须通过歧义警告标签/徽标提示用户。
5. **截断轨道列表呈现（P2-13）**：
   - 当 MP4 文件在样本表处或之前截断时（如由 `core::DiagnosticCode::TruncatedSource` 或 `AnalysisSessionSampleStatus::TruncatedSource` 记录），轨道列表 UI 必须明确报告：*“文件已截断，轨道列表可能不完整”*，而非误导性地显示*“无轨道”*。

---

## 决策

### 1. 在 `AnalysisSession` 上公开格式选择结果

`AnalysisSession` 应保存打开源时计算得到的（或在显式规则恢复会话时合成的）`rules::FormatSelection`，并通过以下接口公开：

```cpp
[[nodiscard]] const rules::FormatSelection& formatSelection() const noexcept;
```

这允许 UI 组件直接检查 `ambiguous()` 与 `decided()`，而无需重复仲裁逻辑。

### 2. `TimelineTableModel` 架构

在 `src/app/` 中引入专用的 `TimelineTableModel`（派生自 `QAbstractTableModel`），用于管理分页的 `SampleDescriptor` 记录：

- **列定义**：
  - `0: Sample #` — 0 起始的样本序号（`sampleIndex`）。
  - `1: Type` — 关键帧显示 `[Sync]`（`isSyncSample == true`），否则显示 `-`。
  - `2: DTS` — 轨道 timescale 单位的解码时间戳。
  - `3: PTS` — 轨道 timescale 单位的呈现时间戳。
  - `4: Duration` — timescale 单位的样本时长。
  - `5: Size (bytes)` — 由 `sourceSpans` 的 bit 长度衍生（$L / 8$）。
  - `6: Bit Offset` — 来自 `sourceSpans` 的起始绝对 bit 偏移。
  - `7: Description Index` — 1 起始的 `sampleDescriptionIndex`。
- **性能**：
  - 最多仅保留一页描述符（默认每页 256 项）。
  - 快速的列查找与工具提示格式化。

### 3. `MainWindow` 时间线 Dock 与控件

`MainWindow` 在 `Qt::BottomDockWidgetArea` 区域新增一个 `QDockWidget`（`timelineDock`，对象名 `timelineDock`）：

1. **头部工具栏**：
   - 轨道选择组合框（`timelineTrackComboBox`）：列出可用轨道及其 ID、格式和样本数量。
   - 状态/警告标签（`timelineStatusLabel`）：显示状态消息，特别是截断警告（P2-13）。
   - 分页控件：上一页按钮（`timelinePrevPageButton`）、下一页按钮（`timelineNextPageButton`）以及页面概况标签（`timelinePageLabel`）。
2. **表格视图**：
   - `QTableView`（`timelineTableView`）：展示 `TimelineTableModel` 的数据行，配置为整行选中、交替行底色并支持自动滚动。

### 4. 交互导航与同步合同

1. **行选中**：
   - 在 `timelineTableView` 中选中某行会更新 `MainWindow` 的源选区为 `sample.sourceSpans`，在 `RawDataView` 中即时高亮该样本字节。
2. **激活进入**：
   - 在样本行上双击或按回车键触发 `enterSample(trackId, sampleIndex, catalog_)`。
   - 成功时：
     - `analysisModel_->resetFromTree(session_->activeTree())` 展示样本单元。
     - 面包屑更新为 `format > Track <id> (<targetFormat>) > Sample #<index> [Sync]`。
     - 选区聚焦到样本子树的根结构节点。
3. **返回容器**：
   - 激活 `navigationBackButton_` 触发 `session_->returnToParent()`。
   - 返回的 `restoredSample`（`SampleNavigationFrame`）提供 `trackId` 与 `sampleIndex`。
   - `MainWindow` 恢复 `timelineTrackComboBox` 中的轨道选择，请求包含 `sampleIndex` 的页面，在 `timelineTableView` 中选中对应行，并恢复 `RawDataView` 中的字节高亮。

### 5. 格式歧义与截断处理

1. **歧义呈现**：
   - 在 `statusBar()` 中增加永久状态标签（`formatAmbiguityLabel`）。当 `session_->formatSelection().ambiguous()` 为真时显示：
     *“Warning: Ambiguous format (container vs elementary stream detected)”*。
   - 当不存在歧义时隐藏该标签。
2. **截断轨道列表渲染（P2-13）**：
   - 评估轨道时，若根树携带 `core::DiagnosticCode::TruncatedSource` 或轨道结果报告 `TruncatedSource`：
     - `timelineStatusLabel` 显示：*“File is truncated; track list may be incomplete”*。
     - 若部分轨道可用，则在组合框中展示已解析轨道的同时显式呈现截断警告。
     - 若无可用轨道，则显著显示该警告，避免仅显示孤立的“无轨道”。

---

## 效果与影响

### 积极影响
- 完整闭环容器样本导航的 UI 流程，达成阶段 5 清单 `:246`（*“从 MP4 sample 进入 H.264/AAC 规则，并可返回容器字段”*）。
- 消除截断容器文件的静默漏报（P2-13）。
- 按 ADR-0106 要求向最终用户公开格式检测仲裁歧义。
- 虚拟化分页确保对海量超大媒体文件的内存占用严格可控。

### 权衡与代价
- 在 `MainWindow` 中新增一个停靠部件，需处理树视图与表格视图之间的焦点管理与事件过滤。

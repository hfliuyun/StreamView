# ADR-0109：会话生命周期管理、格式手动覆盖与 Schema 演进策略

- **状态**：Proposed
- **日期**：2026-09-06
- **作者**：StreamView Contributors

---

## 背景（Context）

StreamView 阶段 5 完整交付了非分片 ISO BMFF MP4/MOV 容器解析、元数据树物化、超大 `mdat` 惰性封装、窗口化样本表索引以及从容器轨道深入到 H.264/AAC 基本码流的跨层样本导航。

在容器与基本流核心解析引擎就绪后，阶段 6 聚焦于桌面会话生命周期、规则管理与桌面用户体验：
1. **桌面会话生命周期与持久化**：
   - 脏状态跟踪（`isDirty()`）、文件 > 保存（`Ctrl+S`）、文件 > 另存为（`Ctrl+Shift+S`）以及文件 > 打开会话（`Ctrl+O` 会话变体）。
   - 未保存修改保护防线：拦截窗口关闭、文件切换与应用退出，向用户弹窗确认（保存 / 放弃 / 取消）。
2. **格式手动覆盖与歧义裁决**：
   - 解决阶段 5 遗留的格式检测歧义（落实评审项 P2-17 与 P2-19）：当自动检测报告 `formatSelection.ambiguous()` 时（如同时存在容器与基本流特征证据），用户需要一条交互式 UI 路径来裁决歧义并选择目标格式。
   - 允许显式格式覆盖：无论自动检测结果如何，用户均可从已安装/内置规则包中显式指定规则入口。
   - 架构上分离「固定规则执行」与「检测仲裁」（落实评审项 P2-20）。
3. **规则版本管理**：
   - 可视化查看已安装与内置规则包，检查版本、内容哈希与可用入口。
   - 通过 `RulePackageStore` 导入并安装外部 `.svrule` 规则包。
4. **桌面体验增强**：
   - 后台分析进度展示与取消操作处理。
   - 全局诊断面板汇总警告与错误，支持与分析树和原始数据视图的双向跳转。
   - 明暗主题切换与运行时动态中英双语国际化切换。

### 项目纪律约束与关键问题回答

1. **Schema 演进约束（§4.3）**：
   - 阶段 6 是否变更 `SessionDocument` 的 schema？若变更，旧版本 1 文档如何读取？
   - 项目纪律 §4.3 严格限制：*「不把 UI 导航栈写入 SessionDocument，除非新 ADR 明确定义其序列化语义」*。
   - 项目纪律 §4.3 同样规定：*「不把 capability 与第一个格式消费者塞进同一提交」*。
2. **历史审查项清点**：
   - **P2-17**：`formatSelection.ambiguous()` 必须在 UI 表面作为格式手动覆盖的触发条件被消费。
   - **P2-18**：检测仲裁中的 AAC 歧义分支为白盒构造覆盖，事实已明确。
   - **P2-19**：模式流二元优先级与锚定检测判定脱节（`format_selection.cpp:145`），在手动格式覆盖机制确立后由用户显式选择终极解决。
   - **P2-20**：测试中的交叉比对走 `openPinnedMp4Fixture()`；在阶段 6 中，固定规则构建正式成为受支持的功能路径（「手动规则指定」），将固定规则测试与端到端检测仲裁测试明确区分。
   - **P2-21**：单一参考工具（`ffprobe`）的局限已如实记录并采纳。

---

## 决策（Decision）

### 1. `SessionDocument` Schema 保持不变：继续使用 Schema Version 1

**决策**：`SessionDocument` 序列化结构**严格保持在版本 1（`schemaVersion: 1`）**。阶段 6 不引入任何 schema 版本升级或字段结构改动。

**设计理由与论据**：
1. **格式手动覆盖在 Schema Version 1 中已具备 100% 表达能力**：
   在 [ADR-0033](0033-save-exact-analysis-sessions-as-atomic-json.md) 与 [docs/zh-CN/session-format.md](../session-format.md) 定义的 `SessionDocument` 版本 1 中，`rule` 对象结构如下：
   ```json
   "rule": {
       "packageId": "org.streamview.h264",
       "packageVersion": "0.1.0",
       "contentSha256": "3a7b...",
       "entryPointId": "h264_annex_b"
   }
   ```
   当会话保存时——无论当前激活的规则是来自自动检测仲裁还是来自用户手动覆盖——它所序列化的都是*当前实际绑定的 `RuleEntryPointIdentity`*。
   在恢复会话时，ADR-0033 明确规定：直接根据该 `rule` 身份在规则目录中精确查找并直接构造分析器，**完全绕过格式自动检测**。
   因此，手动覆盖格式后保存的会话，在恢复时必然精确恢复该覆盖格式。额外添加 `"formatOverride": true` 布尔标记或 schema 字段纯属冗余。
2. **UI 导航栈严格保持为瞬态状态（不进行持久化）**：
   依据 ADR-0103 §6、ADR-0105 §6 以及 [src/app/analysis_session.h](../../src/app/analysis_session.h#L238)：
   - 样本下钻帧（`SampleNavigationFrame`）与子格式帧（`NavigationFrame`）属于 GUI 交互探索的临时覆盖视图，并非持久化的底层事实。
   - 根容器格式是权威的文档锚点。
   - 将子会话执行上下文或样本导航栈写入 `.svsession`，会把可重建的运行时状态泄漏进原本设计为不可变坐标紧凑记录的文档中，破坏 ADR-0033 的核心原则。
   - 当用户在查看样本期间执行保存时，会话文档完整持久化根容器的书签、注释、展开路径、当前 64 KiB 原始数据页索引以及所选内容的绝对起始 bit 偏移。
   - 恢复会话时，根容器树被无损重建，且所选 byte/bit 偏移在高亮区直观呈现，用户可通过时间线面板随时再次进入样本。
3. **完美向后兼容与零迁移负担**：
   保持 Schema Version 1 可确保自 M5 以来生成的所有 `.svsession` 文件继续 100% 正常读取，无须编写复杂脆弱的数据迁移逻辑，亦无格式碎片化风险。

---

### 2. 前向/后向兼容性与 Schema 演进策略

1. **严格的封闭模式闸门（Closed Schema）**：
   `SessionDocument::parse()` 继续对根对象及所有子对象执行 `hasExactKeys()` 校验：
   ```cpp
   hasExactKeys(root, {u"schemaVersion", u"source", u"rule", u"bookmarks",
                       u"annotations", u"expandedPaths", u"view"});
   ```
   任何包含未知、意外或拼写错误字段的 `.svsession` 文件必须被 `SessionDocumentLoadStatus::InvalidSchema` 拒绝，以保障确定性回放与安全边界。
2. **未来版本演进策略（Version N 规范）**：
   若后续阶段确需持久化全新类别的持久用户数据（如多源工作区、跨轨道对齐锚点等）：
   - **强制后向兼容**：解析器首个解析 token 必须为 `schemaVersion`。若 `schemaVersion == 1`，必须严格走不可变的版本 1 解析路径。旧版文档绝不能因软件升级而无法打开。
   - **无双写复杂度**：在新的 schema 版本通过显式 ADR 批准前，StreamView 仅生成规范的版本 1 JSON。
   - **迁移政策**：新版本必须提供从版本 1 到版本 N 的显式映射转换规范。

---

### 3. 会话生命周期与脏状态（Dirty State）跟踪合同

`AnalysisSession` 提供显式的脏状态维护与文档路径绑定 API：

```cpp
namespace streamview::app {

class AnalysisSession {
public:
    [[nodiscard]] bool isDirty() const noexcept;
    void markDirty() noexcept;
    void clearDirty() noexcept;

    [[nodiscard]] const std::optional<QString>& sessionFilePath() const noexcept;
    void setSessionFilePath(QString path);

    [[nodiscard]] bool save(QString* errorMessage = nullptr);
    [[nodiscard]] bool saveAs(const QString& path, QString* errorMessage = nullptr);
};

} // namespace streamview::app
```

#### 状态变迁与判定规则
1. **干净状态（`isDirty() == false`）**：
   - 刚通过 `AnalysisSession::openFile()` 打开新的媒体文件时；
   - 刚通过 `AnalysisSession::restoreSession()` 成功恢复会话时；
   - 成功执行 `save()` 或 `saveAs()` 保存操作后。
2. **脏状态（`isDirty() == true`）**：
   - 添加、修改或删除书签（`addBookmark`, `removeBookmark`）；
   - 添加、修改或删除注释（`addAnnotation`, `removeAnnotation`）；
   - 对已打开的文件执行格式手动覆盖（使用另一套规则重新分析）。
3. **非变迁行为（不标记脏状态）**：
   - 在分析树中展开或折叠节点（瞬态浏览行为）；
   - 在原始数据视图或时间线表格中滚动或翻页（瞬态浏览行为）；
   - 选中节点或源区间（瞬态高亮行为）；
   - 进入或退出子样本/结构型入口（瞬态下钻行为）。

#### UI 未保存修改保护协议（`maybeSave()`）
在 `MainWindow` 中，任何会丢弃当前会话的操作（`openFile()`, `openSession()`, `closeEvent()`, 或 `QApplication::quit()`）：
1. 检查 `session_ && session_->isDirty()`；
2. 若会话为干净状态，直接放行；
3. 若会话为脏状态，弹出模态确认对话框：
   - 提示文案：*「当前会话有未保存的修改。是否在继续前保存修改？」*
   - 按钮组：`[保存]`（默认）、`[放弃]`、`[取消]`。
4. **操作分支处理**：
   - `[保存]`：调用 `saveSession()`。若保存成功，继续原操作；若保存失败或用户取消文件选择，中止原操作。
   - `[放弃]`：丢弃未保存修改，直接放行。
   - `[取消]`：立即中止原操作；当前会话保持原样不变。

---

### 4. 格式手动覆盖架构（闭环 P2-17、P2-19、P2-20）

#### 1. 歧义交互式裁决（P2-17）
在 `MainWindow` 中，当 `session_->formatSelection().ambiguous()` 为真时：
- Task P5j-5 引入的歧义横幅将显示交互式 **「解决歧义...」** 按钮；
- 点击后弹出格式手动覆盖对话框，预选发生竞争冲突的候选格式（例如 `MP4 (ISOBMFF)` 与 `H.264 (Annex B)`）。

#### 2. 显式菜单动作
在主菜单 `分析 > 覆盖格式...`（`actionOverrideFormat`）提供常驻入口，在会话处于活跃状态时可用。

#### 3. 格式覆盖执行契约
当用户选择目标规则入口后：
1. `AnalysisSession::overrideFormat(const rules::RuleEntryPointIdentity& targetRule)`：
   - 请求取消正在进行的后台分析任务；
   - 清空导航栈并重置当前选择；
   - 在 `RulePackageCatalog` 中查找选中的 `targetRule`；
   - 重新构建对应的分析器（如 `H264AnnexBAnalyzer`, `AacAdtsAnalyzer`, 或 `Mp4IsobmffAnalyzer`）；
   - 从字节 0 重新启动分析；
   - 重新初始化 `AnalysisTreeModel` 与 `TimelineTableModel`；
   - 将会话标记为**脏状态**（`markDirty()`）。
2. **P2-19 彻底解决**：
   - `format_selection.cpp` 中的启发式模式流优先级（如 `aacStrong && !h264Strong`）仅作为自动检测的降级逻辑。
   - 手动覆盖机制赋予用户最高决定权，彻底绕过所有启发式妥协。
3. **P2-20 彻底解决**：
   - 测试严格划分为两条独立路径：
     a) 端到端自动检测仲裁测试（`openFile()`），验证未加干预下的正确仲裁；
     b) 手动规则覆盖/固定规则测试（`openWithExplicitRule()`），验证显式指定规则能无条件强制按目标分析器执行。

---

### 5. 规则版本管理架构

通过专用的 `RuleManagerDialog`（位于菜单 `工具 > 规则管理器...`）管理格式规则：
1. **目录检查**：
   - 列出 `RulePackageStore` 发现以及注册在 `RulePackageCatalog` 中的所有规则包；
   - 展示每个规则包的元数据：`packageId`、`packageVersion`、`description`、`contentSha256`（缩略显示并带 tooltip 完整哈希）以及支持的 `entryPoints` 列表；
   - 标示规则包来源：`[内置]`（官方不可变资产）与 `[已安装]`（用户本地存储）。
2. **规则包安装**：
   - 提供 **「安装规则包 (.svrule)...」** 功能；
   - 文件对话框允许选择外部 `.svrule` ZIP 压缩包；
   - 调用 `RulePackageStore::install()` 执行完整安全检查（路径穿越防御、zip bomb 限制、SHA-256 完整性校验、TOML manifest 格式校验，严格遵循 ADR-0015 与 ADR-0016）；
   - 安装成功后注册至 `RulePackageCatalog` 并刷新视图。
3. **内置规则保护**：
   - 内置官方规则包受写保护，不可卸载或覆写。

---

### 6. 阶段 6 任务拆分与依赖编排（P6a – P6i）

为严格遵守项目纪律（单任务闭环 SOP、能力与消费者不混杂提交、固定独立评审门禁），阶段 6 划分为以下串行任务切片：

- **Task P6a**（规范编写与生命周期架构 —— *当前任务*）：
  - 编写双语 ADR-0109，定义会话生命周期、脏状态、格式手动覆盖、规则管理与 Schema 演进策略（Markdown-only）。
  - 触发纪律第 5 条的里程碑收官独立评审门禁。
- **Task P6b**（会话生命周期与脏状态核心切片）：
  - `AnalysisSession` / `SessionDocument` 脏状态跟踪（`isDirty`, `markDirty`, `clearDirty`）、文件路径绑定、`save()` 与 `saveAs()` 核心接口。
  - 在 `analysis_session_test` 与 `session_document_test` 中编写单测验证脏状态变迁与原子保存。
- **Task P6c**（UI 动作与未保存保护切片）：
  - `MainWindow` 的文件 > 保存、另存为与打开会话动作集成。
  - `closeEvent` 与 `maybeSave()` 确认对话框协议（保存 / 放弃 / 取消）。
  - 在 `main_window_test` 中编写针对脏会话关闭、放弃与取消交互分支的 UI 测试。
- **Task P6d**（格式手动覆盖与歧义裁决切片）：
  - `AnalysisSession::overrideFormat` 核心 API 与 `FormatOverrideDialog`。
  - 歧义横幅「解决歧义...」按钮集成（闭环 P2-17、P2-19、P2-20）。
  - 端到端验证手动覆盖及重新分析流程。
- **Task P6e**（规则版本管理器切片）：
  - `RuleManagerDialog` 界面，展示内置与已安装规则包列表。
  - 接入 `RulePackageStore` 实现外部 `.svrule` 的选择与安装。
  - 规则管理器单测与集成测试。
- **Task P6f**（分析进度、取消与诊断汇总面板切片）：
  - 分析进度条显示与异步取消按钮。
  - `DiagnosticsSummaryDock` 全局诊断面板，展示全文件的警告与错误，支持与分析树和原始视图的双向定位。
- **Task P6g**（桌面体验、主题与双语国际化切片）：
  - 明暗主题切换支持。
  - 基于 `QTranslator` 与编译后 `.qm` 资源的运行时中英双语动态切换。
  - 主题与语言切换的 UI 测试。
- **Task P6h**（端到端回归与大源会话恢复验证切片）：
  - 全会话生命周期端到端回放：保存、修改、重载、手动覆盖持久化恢复以及 100 GB 虚拟稀疏源会话恢复验证。
- **Task P6i**（阶段 6 审查与里程碑收官门禁）：
  - 双语文档同步、检查清单 100% 达成确认与独立评审门禁裁定。

---

## 后果（Consequences）

### 正向收益
- **保障兼容性**：保留 Schema Version 1 彻底消除了版本颠簸与迁移风险，确保现有会话文件永远可用。
- **防止数据丢失**：未保存修改拦截协议有效保障了用户标注与书签的安全性。
- **确定性裁决歧义**：格式检测歧义（P2-17/P2-19）获得由用户驱动的清晰裁决途径。
- **架构清晰解耦**：自动检测仲裁与固定规则覆盖在代码与测试中得到明确划分（P2-20）。
- **动静分离**：瞬态 UI 导航栈与持久化坐标文档保持清晰正交解耦。

### 潜在代价 / 取舍
- 重新加载会话时不会自动恢复深入到样本内部的临时下钻栈，而是恢复在根容器级别并精准高亮对应 byte/bit 偏移（此为保证系统稳健与避免脆弱反序列化的既定设计）。

---

## 被否决的备选方案（Alternatives Rejected）

1. **将 Schema 升级到版本 2 以添加 `"formatOverride": true` 字段**：
   - *否决原因*：`rule` 对象本身已精准记录了分析所用的完整 `RuleEntryPointIdentity`。恢复会话时直接根据该身份从规则目录构建分析器，并不经过自动检测。显式布尔标志属于完全冗余的信息。
2. **将 UI 导航栈序列化到 `SessionDocument` 中**：
   - *否决原因*：依据 ADR-0103 §6、ADR-0105 §6 与 §4.3，子样本执行上下文是动态检查覆盖层而非持久锚点。将瞬态执行树序列化会导致 `.svsession` 与特定版本的运行时实现紧密耦合。
3. **在 Schema Version 1 中允许未知字段以换取「前向兼容」**：
   - *否决原因*：ADR-0033 的封闭 Schema 设计（`hasExactKeys`）是保障安全性（拒绝畸变文件）、捕获打字错误和确保确定性回放的核心防线。
4. **在滚动原始视图或展开树节点时将会话标记为脏**：
   - *否决原因*：翻页与展开属于探索性浏览行为，并非用户创作内容。每次滚动都提示保存会导致严重的假阳性骚扰。

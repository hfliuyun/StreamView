# StreamView UI 整改任务书：交给快速模型直接执行

状态：2026-10-12，A06-A09 与时间线轨道下拉框修复已实现并提交为 `b8db0eb`；
本文件保留为历史执行任务与验收依据。Any CLI 的交付报告已记录 dev/UI 定向验证，
主代理仍需完成 ci/sanitize 与三平台 Hosted CI。
本文原本是自包含执行任务，不要求读取之前的对话；现阶段不应重复实现已完成整改。

## 角色、基线与开工检查

你负责 UI/边界整改，主代理随后复核。工作区 `/Users/yun/code/streamview`。
核心修复代码基线为 `7135a1ac3d3174edf6b599bcfc234b5dd90e6821`。
当前 HEAD 可以是该提交，也可以是其后仅增加 Markdown 文档的后代提交；本文自身的
文档提交不改变代码基线。禁止退回审计前的 `be153e6` 或重新实现已提交的核心修复。

先运行并在交付报告中记录：

```sh
git status --short --branch
git rev-parse HEAD
git log -6 --oneline
git merge-base --is-ancestor 7135a1ac3d3174edf6b599bcfc234b5dd90e6821 HEAD
git diff --name-only 7135a1ac3d3174edf6b599bcfc234b5dd90e6821 HEAD
```

如果基线不是 HEAD 的祖先，或上述差异包含非 Markdown 文件，停止并说明差异，
不要自行 reset、checkout、stash 或换分支来“修正”现场。已有无关改动必须保留；
如果你需要修改的同一段代码已有他人变更，先理解并兼容，无法安全兼容时报告阻塞。

你不是独占代码库，不要撤销或重写他人改动。
不要创建子代理，最多单个实现进程。不要提交、push、创建 PR、安装依赖或修改 Git 配置。
不要读取或修改 `.omo/` 与 `scratch/`。

主代理已经修复并验证：
- 保存会话覆盖媒体源、序列化结果超过 1 MiB；
- 冲突规则安装落盘及存储失败时的 catalog 原子性；
- 临时 MP4 表读取的预算/节点隔离；
- 样本配置绑定、上下文隔离、乱序依赖重绑定、缓存及失败回滚。

这些修复的组合已经通过本地 dev/ci/sanitize 各 59/59 测试，不代表下面四项 UI
缺陷已关闭，也不是当前修改的三平台 Hosted CI 证明。不要重做这些核心功能。

权威文档：
- `docs/implementation-plan.md`
- `docs/product-requirements.md`
- `docs/adr/0109-session-lifecycle-management-format-override-and-schema-evolution.md`
- `docs/adr/0110-phase-7-fuzzing-hardening-performance-and-release.md`
- `docs/adr/0113-audit-remediation-resource-and-context-boundaries.md`

## 改动范围与顺序

建议串行顺序：`A07 -> A09 -> A06 -> A08`，语言切换放在其他 UI 文本变更之后验收。
只做下面四项，不推进 Phase 7。允许修改 `src/app/` 的 UI 文件：
`main_window`、`format_override_dialog`、`field_inspector`、`raw_data_view`、
`analysis_tree_model`、`timeline_table_model`、`diagnostics_summary_dock`、
`rule_manager_dialog`、`localization_manager` 及对应头文件、已有的对应 `tests/app/`。
确有新增或变更翻译文本时，可以同步 `src/app/translations/streamview_zh_CN.ts`。

禁止修改 `analysis_session.*`、`session_document.*`、`src/core/`、`src/rules/`、
规则资产、schema、CMake、fixture 生成脚本、implementation-plan 与 ADR。
保留 `rule_manager_dialog.cpp::installPackage()` 已提交的先暂存注册、后落盘、再发布流程。
不改主题设计、不重构无关模块、不升级依赖。若必须改变核心 API 或规范才能继续，停止
该项并提供证据，交给主代理裁决；其他不依赖它的任务可以继续。

## A06: Installed Rule Selection

定位：`src/app/format_override_dialog.cpp:79` 的 `populateOptions()` 当前重新加载三个官方包，
只枚举硬编码 identity，所以 `catalog.allPackages()` 中第三方兼容根入口不会显示。
行号只对应上述代码基线，发生位移时按符号名定位。

从真实 catalog 枚举兼容、可用于根分析的 entrypoint，保留精确
package/version/hash/entrypoint 身份与当前/候选标记。不能把 `nal` /
`video.h264.nal`、`asc` / `audio.aac.asc` 等结构子入口当根分析器。
参考 `analysis_session.cpp:145` 的 `setupResolvedAnalyzer()` 及各 Analyzer 创建约束，
使用已有编译/枚举能力校验，不改核心。仅有 manifest 兼容声明不能证明入口可执行，
不要只按包名或展示名称添加一个新的白名单。

保留三个官方选项现有可读名称及顺序或合理稳定排序；第三方名称须显示
package/version/entrypoint 以区分。不兼容或不可运行入口不可接受。

验收：
- 注册可运行第三方 ADTS 或 Annex B 包，真实 Dialog 可选并返回精确 identity。
- 通过既有显式规则打开/覆盖接口确认选中的第三方根入口确实可以构造 analyzer。
- 同包多版本保持独立选项，当前规则标记匹配完整 identity，不丢 content hash。
- 结构子入口、不兼容入口不可选。
- 空选项时 OK 禁用。

测试放在现有 `tests/app/main_window_test.cpp`，复用
`formatOverrideDialogConstructsAndSelectsCandidate()` 的真实对话框模式。
当前没有独立的 FormatOverrideDialog CTest target，不要创建未注册的测试文件。

## A07: Diagnostic Span Selection

定位：`src/app/main_window.cpp:1676` 的 `onDiagnosticSelected()`。
它先 `setSourceSelection(location.sourceSpans)`，
又调用 `selectSourceBit(firstStart)`，后者把选区覆盖成一个 bit，还可能把树选择跳到另一节点。

保留诊断所属 node selection/inspector 和诊断完整 sourceSpans；缺失诊断 location 时
回退所属 node 完整 location。`RawDataView::setSourceSelection` 已会导航到第一页，
不需要再调用 `selectSourceBit`。

验收使用真实 MainWindow：
- 多 bit 与两个不连续 span 完整高亮，gap 不高亮。
- 树仍选择诊断所属节点。
- 缺失 location 的 fallback 正确。

扩展 `tests/app/main_window_test.cpp` 的诊断定位测试；必须逐一比较 span 起点和长度，
不能仅断言“选区非空”。不连续用例的间隙 bit 应明确不被选中，inspector 仍属于原诊断节点。

## A08: Runtime Translation

字典覆盖率不是热切换覆盖。`MainWindow::retranslateUi` 只更新菜单和部分 dock；
FieldInspector 表单 label、RawDataView 模式/tooltip/page label、
AnalysisTreeModel/TimelineTableModel 表头、timeline track/page 文字、
已打开的 FormatOverrideDialog/RuleManagerDialog 仍可能保留旧语言。

完善 `QEvent::LanguageChange` 或已有 LocalizationManager 信号更新，保留数据、选区、
展开、表行/页、对话框选中 identity。禁止通过销毁重建窗口解决。
新 `tr` 字面量同步字典，theme_localization_test 计数须反映真实扫描结果，
禁止弱化为大于等于或删除校验。避免翻译媒体字段名或规则语义原文。

验收覆盖已构造且含数据的控件 EN -> ZH -> EN，不仅是菜单。
至少点验 inspector 静态 label、raw 模式/tooltip/页文本、两个 model 表头、timeline
轨道/分页文本和已打开的两个管理对话框。model 表头变化要通过 Qt 的 model 通知传到
现有 view，不能只让新建对象显示正确。语言恢复后各选区、页码和选中规则 identity 不变。

使用现有 `tests/app/theme_localization_test.cpp`、`main_window_test.cpp` 及对应控件测试。
沿用精确的词典/字面量差集与计数守卫；不得冻结旧计数后绕过新字符串扫描。

## A09: Save Root View From Child Or Sample

定位：`src/app/main_window.cpp:582` 的 `currentUserState()` 和 `:684` 的
`saveSessionToPath()`。当前从活动 child tree/raw selection 收集状态，
写出的 session 却绑定 root 规则，恢复时路径不属于 root。

进入第一层 child/sample 前保留根 UI 视图快照：expandedPaths、selectedAnalysisPath、
raw page/mode/selected source bit。在 navigationDepth > 0 保存时使用根快照，
bookmarks/annotations 仍取最新全局值。返回 root 后正常更新根状态；新 source、
openSession、override 清理或重建快照；失败导航不得改变快照。

禁止把 navigation stack 写入 SessionDocument，禁止改 schema。
已有 pendingSelectedAnalysisPath/pendingExpandedPaths 异步恢复与用户选择抢占必须保持。

验收使用真实 MainWindow：
- root 展开/选择，进入 sample 或 ASC child，改变 child 视图并添加 bookmark 后保存。
- 解析文档确认 root paths/view 与最新 bookmark 都保留。
- 新窗口恢复正确 root，navigationDepth = 0。
- 退出再进入或新文件不泄漏旧 root 快照。

分别覆盖 `tests/fixtures/mp4_p5h_mp4a_esds.mp4` 的 ASC child 与已有真实 sample
导航 fixture。不要用手工构造的 SessionUserState 替代 MainWindow 的实际保存路径。
同时保留 `childFormatNavigationDoesNotApplyRootPendingPath()`、主动点击抢占与格式
覆盖清理的现有回归；保存失败必须保持 dirty，导航失败不得覆盖先前 root 快照。

## Verification And Delivery

每项先加具名回归测试，运行取得真实 RED，再实现转 GREEN。若前提不成立，
停止该项并提供 file:line 证据，不伪造 RED。
每项使用独立测试函数名；旧基线已通过的补充用例明确标为 coverage-only。
不要删除、放宽或跳过现有断言来制造通过，尤其不能以 `A || B` 接受互斥结果。

单测可用 `env QT_QPA_PLATFORM=offscreen build/dev/tests/<test_target> <test_name>`
精准运行。四项完成后运行：

```sh
cmake --build build/dev -j2
ctest --preset dev --parallel 2 --output-on-failure -R "^streamview_(main_window|rule_manager_dialog|diagnostics_summary_dock|theme_localization|field_inspector|raw_data_view|analysis_tree_model|session_persistence_regression|analysis_session|session_document)_tests$"
ctest --preset dev --parallel 2
git diff --check
git diff --stat
git status --short --branch
```

若尚无 `build/dev`，先按仓库现有配置执行 `cmake --preset dev`，不要自行安装依赖。
不要同时跑三构建矩阵，主代理在复核后负责最终 dev/ci/sanitize 和后续交付门禁。

最终报告必须包含：
1. 开工 HEAD 与工作树状态。
2. A06、A07、A08、A09 逐项的完成/阻塞状态及修改文件。
3. 每项测试名、真实 RED 命令与关键失败断言、GREEN 命令与结果；coverage-only 单列。
4. 最终 dev 全量结果、`git diff --check` 结果及未运行的检查。
5. 遗留风险、需要主代理裁决的问题；没有则明确写无。

保留未提交的工作树差异，完成后停止，等待主代理独立复核。
不要继续 Phase 7，不要声称项目已经发布就绪。

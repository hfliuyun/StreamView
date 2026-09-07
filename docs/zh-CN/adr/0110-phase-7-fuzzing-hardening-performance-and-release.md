# ADR-0110: 阶段 7 安全加固、模糊测试策略、性能基线与发布自动化

- **状态**：Proposed
- **日期**：2026-09-07
- **作者**：StreamView 贡献者

---

## 背景

随着阶段 6（会话生命周期、显式规则覆盖、规则包版本管理以及桌面用户体验）的顺利收官与里程碑门禁放行，StreamView v0.1 已完整组装了核心模型、声明式 DSL 引擎、官方媒体规则包（H.264 Annex B、AAC ADTS/ASC、ISO BMFF MP4/MOV）以及 Qt GUI 桌面应用的全部功能。

阶段 7 是 StreamView v0.1 的最终收官阶段：**安全、性能与发布**。其使命是全面加固软件供应链与运行时解析管线以防御恶意输入、在超大媒体源上实测验证性能基线、实现三平台发布包自动化构建，并严格满足公开发布所需的所有法律与开源合规要求。

### 里程碑放行约束与前置准入项

阶段 6 独立评审在裁定放行里程碑的同时，附加了两项必须在 Task P7a 内部优先闭环的硬性前置约束：

1. **P1-1-R1（本地化守卫正则对跨行拼接字面量盲视）**：
   - `tests/app/theme_localization_test.cpp` 中的扫描正则依赖 `\btr\(\s*"((?:[^"\\]|\\.)*)"\s*\)`，其在闭合引号后要求紧随 `)`。这导致 C++ 跨行拼接字面量（如 `tr("part1 " "part2")`）对守卫测试完全隐形。
   - `format_override_dialog.cpp` 与 `rule_manager_dialog.cpp` 中的两处跨行拼接 UI 对话框文案因此漏译，而守卫测试却误判为全量通过。
   - 要求：重构守卫正则，解析 `tr(` 直至对应的 `)`，并拼接其内部全部字符串字面量。执行双阶段反向验证：必须先使测试转红（证明真实拦截未录入的拼接字面量），随后补齐词典使测试恢复绿灯，并将断言升级为与词典总数的严格相等校验。

2. **P1-2-R1（同名兄弟消歧变异实证与表述限定）**：
   - `tests/app/main_window_test.cpp` 中的 H.264 SPS 测试断言了 `offset_for_ref_frame[1]` 的节点恢复。然而在 DSL 规则引擎中，`repeat` 维度无条件追加了 `[i]` 索引后缀（即 `offset_for_ref_frame[0]` 与 `offset_for_ref_frame[1]`），其显示名称本身已然互异。
   - 要求：通过单变量反向变异实证（例如证明去除 `#<row>` 消歧将在存在同名兄弟或合成重名冲突时导致节点恢复失败），为兄弟消歧机制提供坚实的有效性证据；同时限定实施计划记录中的表述。

3. **性能基线与深层路径观测**：
   - `MainWindow::currentUserState()` 在每次保存时均对整棵已物化树执行全量递归遍历（`collectExpanded`）。
   - `MainWindow::openSessionFile()` 在多批次解析未完全结束时便触发路径展开（`expandNodeByPath`）与选区恢复。
   - 要求：在阶段 7 的性能基线与基准测试中，将上述两条关键路径显式纳入耗时与内存观测。

---

## 架构决策

### 1. 模糊测试（Fuzzing）架构与执行策略

**决策**：构建覆盖全部非信任数据输入边界的模块化、多目标模糊测试框架。

#### 1.1 模糊测试目标矩阵
StreamView 在六个关键边界直接接触非信任或畸变的二进制输入：

| 目标名称 | 被测核心组件 | 输入接口 | 威胁模型与不变式 |
| :--- | :--- | :--- | :--- |
| `fuzz_dsl_parser` | `DslParser` | 原始 `.svfmt` 文本 | 拒绝畸形语法；严防递归深度爆炸与未捕获异常。 |
| `fuzz_dsl_compiler` | `DslCompiler` | 语法 AST | 优雅拒绝语义/类型错误；确保确定性 IR 生成。 |
| `fuzz_dsl_vm` | `DslVirtualMachine` / `DslExecutor` | 编译后字节码 + 合成比特流 | 严格执行循环与指令预算；杜绝越界位读取与崩溃。 |
| `fuzz_rule_package_store` | `RulePackageStore` | 原始 `.svrule`（ZIP 归档 + TOML） | 防御路径穿越（Zip Slip）、解压炸弹（压缩比 > 100x）与损坏的 manifest。 |
| `fuzz_format_detectors` | Annex B, ADTS, MP4 探测器 | 原始文件前缀（最多 4 MiB） | 面对任意字节流不崩溃、不挂起；仲裁输出保持确定性。 |
| `fuzz_mp4_sample_extractor` | `Mp4SampleTableExtractor` | 合成 `stbl` box 字节块 | 防范 sample/chunk 索引计算中的整数溢出；限制内存分配。 |

#### 1.2 框架实现与跨平台可移植性
- **引擎集成**：在 Clang 环境下支持 LLVM `libFuzzer`（`-fsanitize=fuzzer,address,undefined`）；
- **独立回放模式**：为每个 fuzz 目标内置独立执行驱动（`main()` 入口），使测试用例语料库（Corpus）可在 Ubuntu、macOS、Windows 三平台的常规 CTest 流水线中作为确定性回归测试无缝运行，无需依赖外部 fuzzer 库；
- **语料库管理**：以现有测试夹具（`tests/fixtures/`）作为初始种子，补充极限边界样本（零长流、极大整数头、截断 NAL、环状 box 树）。
- **路径消歧语料目标（Task P7c）**：在 Task P7c 中，规则包与格式扫描器 fuzz 语料库将显式纳入字段与节点名包含 `#<数字>`（例如 `tag#1`、`entry#0`）的合成语法树，验证路径寻址鲁棒性并实证其不与 `#<row>` 同名兄弟消歧语法发生冲突。

---

### 2. 全矩阵加固与静态分析

**决策**：在全量构建矩阵中全面提升编译器警告级别、强制 ASan/UBSan 零告警，并引入自动化静态代码检查。

1. **编译器警告级别提升**：
   - GCC / Clang：`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`（针对项目源码目标）；
   - MSVC：`/W4 /WX /permissive-`，启用严格结构化异常安全检查。
2. **Sanitizer 零容忍准则**：
   - `cmake --preset sanitize`（ASan + UBSan）必须保证全量测试 0 告警、0 内存泄漏、0 未定义行为；
   - 评估在 Windows CI 中引入 AddressSanitizer（`/fsanitize=address`）。
3. **自动化静态分析（Clang-Tidy）**：
   - 通过 CMake `CMAKE_CXX_CLANG_TIDY` 检查：
     - `bugprone-*`：截断窄化转换、未处理返回值、use-after-move；
     - `modernize-*`：遵循现代 C++20 惯用法；
     - `performance-*`：消除不必要的深拷贝与冗余临时分配；
     - `clang-analyzer-*`：死代码、空指针解引用、未初始化内存。

---

### 3. 性能基线与诊断观测体系

**决策**：确立确定性、防抖动的性能回归基准，设立硬性耗时与资源预算门禁。

#### 3.1 硬性性能预算

| 指标 | 预算上限 | 测量场景 | 适用平台 |
| :--- | :--- | :--- | :--- |
| **初始视图可用耗时** | $\le 2.0\text{ s}$ | 应用冷启动并完成首屏典型媒体（`mp4_p5j4_avc_multi_nal.mp4`）根树渲染。 | dev / ci / sanitize |
| **100 GB 虚拟稀疏源内存占用** | $\text{RSS} \le 512\text{ MiB}$ | 打开 100 GB 稀疏源、计算指纹、完成首轮扫描与树渲染全生命周期。 | macOS / Linux / Windows |
| **已知偏移页面读取延迟** | $p95 < 100\text{ ms}$ | 通过 `SourcePager` 随机点播 64 KiB 页面，连续执行 1,000 次查询。 | dev / ci |

#### 3.2 专项路径观测点
1. **`collectExpanded` 递归性能观测**：
   在树节点数量达到超大深度（$\ge 10,000$ 节点）时，实测保存会话时序列化展开路径的总耗时，断言不超过 $50\text{ ms}$；
2. **异步状态恢复观测**：
   在需要跨批次分析的大型文件上，观测 `MainWindow::openSessionFile()` 视图恢复与后续批次到达时的状态一致性。

---

### 4. 发布打包自动化与开源合规

**决策**：在 Windows、macOS 与 Linux 三平台上自动化构建开箱即用的发布包，附带完整的许可证说明与完整性校验码。

#### 4.1 交付产物矩阵

| 目标系统 | 架构 | 包格式 | 打包工具链 | 包含内容 |
| :--- | :--- | :--- | :--- | :--- |
| **Windows** | x64 | `.zip` | CMake `CPack` / `windeployqt` | `streamview.exe`, `svtool.exe`, Qt 运行时 DLL, 官方规则包, 许可证。 |
| **macOS** | ARM64 (Apple Silicon) | `.app.zip` | `macdeployqt` | 包含内嵌 Qt Framework 与官方规则包的独立 `StreamView.app` 捆绑包。 |
| **Linux** | x86_64 | `AppImage` | `linuxdeployqt` / AppImageKit | 面向 glibc 2.35+（Ubuntu 22.04+）的独立可执行单一 AppImage 文件。 |

#### 4.2 发布合规与完整性资产
每个正式分发版本均附带：
1. `SHA256SUMS.txt`：全部分发归档文件的 SHA-256 校验哈希；
2. `SBOM.json`：SPDX 2.3 JSON 规范的软件物料清单，详列 Qt、SQLite、zlib 等全部依赖项；
3. `LICENSE`：StreamView 核心的 MIT 许可证；
4. `LICENSES/`：Qt（LGPLv3）、SQLite（Public Domain）及内置规则资产的细粒度许可证文本；
5. `README_QT_SOURCE.txt`：遵循 LGPLv3 规范，提供获取分发二进制中所链接 Qt 源码的明确书面指引。

#### 4.3 梯次发布管道
发布严格依照版本标签有序推进：
1. `v0.1.0-alpha`：Fuzzing 与静态分析完成；打包工作流初跑验证；
2. `v0.1.0-beta`：性能基线全绿；社区预览体验；
3. `v0.1.0-rc`：发布候选版本；双语文档封版与法律合规审查；
4. `v0.1.0`：正式生产级通用可用版本（GA）。

---

### 5. 分析路径消歧与树状态恢复语义

**决策**：正式规范并实施 `MainWindow::findIndexByPath(const QString& path)` 的三级节点解析语义：
1. **第一级（同名兄弟行号消歧）**：若路径分段在 `lastIndexOf('#') > 0` 处包含 `#<row>`（`<row>` 为一至多位数字），尝试匹配行号为 `<row>` 且名称与 `#` 前缀严格一致的子节点；
2. **第二级（纯字面量兜底回退）**：若第一级未能匹配（例如该节点不在 `<row>` 行、`<row>` 越界，或节点原本的字面名称即为 `prefix#<digits>`），则回退遍历全部子节点执行 `data(Qt::DisplayRole).toString() == part` 纯字面量匹配。这确保字面名称合法包含 `#<digits>` 的节点依然完全可寻址；
3. **第三级（历史行号兜底）**：若第二级未命中且该分段为纯整数字符串，则视为行号索引以保持与历史测试夹具的向后兼容。

同时对会话恢复生命周期提供以下硬性保证：
- **子会话导航隔离**：在 `session_->navigationDepth() != 0` 时严禁执行 `applyPendingTreeState()`，杜绝根路径污染 sample/child 子树（P2-43）；
- **格式覆盖清理**：在 `MainWindow::overrideFormat()` 切换格式时，无条件重置待恢复选区与待展开路径，杜绝跨格式脏路径匹配（P2-44）；
- **用户主动点选抢占**：流式分析进行中，用户在 `analysisTreeView` 中的任何主动点选均立即清空 `pendingSelectedAnalysisPath_`，防止后续解析批次到达时强行夺走用户光标（P2-45）。

---

## 阶段 7 任务分解（WBS）

- [ ] **Task P7a**（规范与前置守卫闭环）：编写双语 ADR-0110；闭环 P1-1-R1（本地化正则跨行拼接先红后绿修复）；闭环 P1-2-R1（同名兄弟消歧变异实证与表述限定）；确立阶段 7 WBS（固定独立评审门禁）。
- [ ] **Task P7b**（模糊测试框架与核心目标）：实现 DSL parser、compiler、VM 的 fuzz 驱动，并将独立 fuzz 回放接入 CTest。
- [ ] **Task P7c**（规则包与格式扫描器 Fuzzing）：实现 `RulePackageStore`（ZIP/manifest）、H.264、AAC 与 MP4 box/sample 提取器的 fuzz 驱动。
- [ ] **Task P7d**（静态分析与警告级别提升）：配置 Clang-Tidy 检查规则并提升 dev/ci 的编译器告警门禁。
- [ ] **Task P7e**（性能基线与基准测试体系）：实现初始视图延迟、100 GB 稀疏内存占用及已知偏移页面读取延迟的自动化回归基准；观测 `collectExpanded` 与异步恢复。
- [ ] **Task P7f**（Windows 打包自动化）：配置 CPack 与 `windeployqt` 自动化生成 Windows x64 独立 ZIP。
- [ ] **Task P7g**（macOS 打包自动化）：配置 `macdeployqt` 自动化生成 macOS ARM64 `.app.zip`。
- [ ] **Task P7h**（Linux 打包与打包门禁）：配置 Linux x86_64 AppImage 生成流水线；完成 SBOM 与合规许可证清单（固定独立评审门禁）。
- [ ] **Task P7i**（预发布试跑与 `v0.1.0-alpha`）：打标签并生成 alpha 阶段分发资产。
- [ ] **Task P7j**（跨环境验证与 `v0.1.0-beta`）：在目标环境中全量验证 beta 候选包。
- [ ] **Task P7k**（发布候选与 `v0.1.0-rc`）：最终完整性核验、双语文档冻结与 RC 打标。
- [ ] **Task P7l**（最终发布门禁与 `v0.1.0` GA）：全量清单 100% 验收、发布签字并发布 GA（固定独立评审门禁）。

---

## 历史与整改审查项对应

- **P1-1-R1**：跨行拼接字符串字面量本地化守卫与严格词典映射（在 `tests/app/theme_localization_test.cpp:140-164` 经先红后绿验证，commit `6e8f9a0`）；
- **P1-2-R1**：同名兄弟消歧变异实证与表述限定（在 `tests/app/main_window_test.cpp:2248-2281` 经删除 `#<row>` 反向变异转红验证，commit `6e8f9a0`）；
- **P2-36**：报告与仓库文档定性完全对齐；
- **P2-37**：同步历史任务在 `docs/implementation-plan.md:253` 中的状态为「P2-19 缓解」；
- **P2-38**：统一测试用例计数格式为「N 槽（QTest totals M）」；
- **P2-39**：修正文档引用文件名为 `docs/adr/0019-skip-ci-for-markdown-only-changes.md`；
- **P2-40**：记录节点路径中 `#<row>` 分界解析语义，并将 `#<digits>` 节点名纳入 Task P7c 模糊测试语料；
- **P2-43**：在 `advanceAnalysis()` 终态完成逻辑中补充 `rootTreeIsActive` 守卫（`src/app/main_window.cpp:1092-1096`）；
- **P2-44**：在 `MainWindow::overrideFormat()` 中彻底重置待恢复路径与展开状态（`src/app/main_window.cpp:820-821`）；
- **P2-45**：流式解析期间用户主动点选立即抢占并清空待恢复选区（`src/app/main_window.cpp:307-309`）；
- **P2-46**：`FormatOverrideDialog` 内置格式文案全部包裹 `tr()`，词典扩容至 154 键，并将 `ThemeLocalizationTest` 提升为 154 键与 227 次调用严格断言（`src/app/localization_manager.cpp:35-43`、`tests/app/theme_localization_test.cpp:170-171`）。

## 延期审查项说明

- **P2-41**：正式 Release 构建耗时基准与性能回归测试延期至 Task P7e（性能基线与基准测试体系）专项建立，不以波动的 CI runner 墙钟耗时作为基线依据。

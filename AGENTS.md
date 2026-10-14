# StreamView Agent Instructions

本文件是所有 AI 代理（AGY 及其子代理、Codex、CodeBuddy 等）在 StreamView 项目中的**强制行为约束**，由原 GEMINI.md 开发规范合并而来。
违反任何标记为「禁止」的规则即视为任务失败。

---

## 1. 恢复与定位

每次会话开始或恢复时，代理**必须**：

1. 读取 `docs/implementation-plan.md` 顶部 header（`Current Phase`、`Last Completed Step`、`Next Action`、`Blockers`），确定当前断点。
2. 检查仓库状态：`git branch`、`git status`（staged/unstaged）、`git log --oneline -5`。
3. 确认 `main` 与 `origin/main` 的偏差；如有未推送提交，先理解其内容再继续。
4. 重读与当前任务相关的文件（ADR、svfmt、测试），不依赖上下文中的陈旧内容。

## 2. 分支模型

**单主干（trunk-based）**：所有提交线性落在 `main` 分支上。禁止 feature branch、merge commit。提交图必须是一条直线。

## 3. Conventional Commits 格式

```
<type>[(<scope>)]: <description>

[optional body]
```

- description 使用**小写英文祈使句**，不加句号。
- 风格必须对齐近期 `git log`（`git log --oneline -10` 作为参考）。

### 类型

| 类型    | 用途                                                     |
| ------- | -------------------------------------------------------- |
| `docs`  | 规范编写（specify/define）、里程碑记录（record）、CI 记录 |
| `feat`  | 实现功能、解析器、DSL 表达式、验证逻辑                   |
| `test`  | 独立测试补充（多数测试随 feat 一起提交）                 |
| `fix`   | 缺陷修复                                                |
| `ci`    | CI 流水线与跨平台构建配置                                |
| `chore` | 项目基础设施变更                                         |

### 常用 scope

| Scope     | 含义                                   |
| --------- | -------------------------------------- |
| `h264`    | H.264 Annex B 格式规则与解析           |
| `rules`   | DSL 规则引擎（编译器、VM、执行器）     |
| `dsl`     | DSL 语言层面的表达式和类型扩展         |
| `core`    | 核心模型（源、坐标、分析节点、上下文） |
| `app`     | Qt 桌面应用                            |
| `tool(s)` | 内部工具 `svtool`                      |
| `ci`      | 持续集成                               |
| 无 scope  | `docs` 类型通常不带 scope              |

### 提交体规范

非 trivial 的提交必须附带详细 body，模式：

1. **做了什么**（一段描述变更内容）
2. **为什么这样做**（设计理由、引用 ADR 编号）
3. **版本号变更**（如 `Package version becomes 0.1.29`）
4. **测试结果**（如 `H.264 analyzer suite 125/125, local dev/ci/sanitize 32/32`）

## 4. 单任务闭环 SOP（六步，顺序固定，不得省略任何步骤）

每个功能增量**严格按以下顺序执行**：

### 步骤 1：写文档 → `docs: specify ...`

**先写规范，再写代码。**

- 需要设计决策时：新增双语 ADR（`docs/adr/NNNN-*.md` + `docs/zh-CN/adr/NNNN-*.md`）+ 更新英文语言参考（`docs/format-language/README.md`）+ 更新中文语言参考（`docs/zh-CN/format-language/README.md`）。
- 仅应用既有合同时：更新 bundled-profile 英中字段说明。
- 单独 commit：`docs: specify ...` 或 `docs(<scope>): specify ...`。

### 步骤 2：编码 → `feat(<scope>): ...`

- 修改 `.svfmt` 格式规则文件、C++ 源码（编译器/VM/解析器）。
- **每次改变解码输出必须升级 `rule.toml` 的 version**（逐任务 patch +1）。
- **测试随实现一起提交**，不单独拆分（除少数例外用 `test:` 前缀）。
- 提交消息：`feat(h264): ...` / `feat(dsl): ...` / `feat(rules): ...`。

### 步骤 3：本地测试（全部必须通过才能继续）

```bash
# DSL 规则静态校验
svtool rule check

# 定向测试套件
ctest -R "dsl_parser|dsl_ir|dsl_executor|streamview_h264_annex_b_analyzer" --preset dev

# 三套完整构建 + 全部 CTest
cmake --preset dev   && cmake --build --preset dev   && ctest --preset dev    # Debug
cmake --preset ci    && cmake --build --preset ci    && ctest --preset ci     # Release
cmake --preset sanitize && cmake --build --preset sanitize && ctest --preset sanitize  # ASan/UBSan
```

- 三套 CTest 均须 **32/32**（或当前全量）且**无 sanitizer 报告**。
- Fixture 必须由生成脚本装配并用 `svtool analyze` 回读实测（禁止手算 bit 位置）。
- 新测试须验证为有效信号（在旧规则下应 red）。

### 步骤 4：Commit & Push

- 实现 commit：`feat(h264): ...` / `feat(dsl): ...` / `feat(rules): ...`
- 测试记录 commit：`docs: record ... coverage`
- push 失败时先尝试本地代理回退。

### 步骤 5：Hosted CI 验证

- 本环境无 `gh` CLI，使用公共 GitHub API 查询 hosted run。
- 等待三平台**全部成功**：Ubuntu 24.04 / Qt 6.11.1、Windows 2022 / Qt 6.10.1、macOS 15 / Qt 6.11.1。
- Markdown-only 提交可按 ADR-0019 跳过 hosted CI。

### 步骤 6：验收 → `docs: record ...`

- 把 run/job ID、测试数字、下一步写回 `docs/implementation-plan.md`：
  - 勾选对应复选框
  - 刷新顶部 header：`Last Completed Step`、`Next Action`、`Last Verification`
  - 追加执行记录条目
- commit：`docs: record hosted ... verification` 或 `docs: record ... coverage`
- **只有完成步骤 6 后才能进入下一任务。**

## 5. 全局禁止事项

以下行为**绝对禁止**，违反即任务失败：

1. **禁止修改分析核心以塞入格式专属逻辑**——格式语义只能进 DSL/规则层（`.svfmt`、compiler、VM）。
2. **禁止删除既有执行记录**——`implementation-plan.md` 中的已完成记录只追加不删除。
3. **禁止跳过 sanitize 配置**——三套构建（dev/ci/sanitize）全部必跑。
4. **禁止手算 bit 位置**——fixture 必须由生成脚本装配。
5. **禁止跳过规范步骤**——必须先 `docs: specify` 再 `feat:`，不能直接写代码。
6. **禁止自行扩展核心合同**——发现语言/合同不可表达时立即停止并上报（见第 7 节）。
7. **测试断言优先使用「完整有序 child 名字列表」**而非位置索引（ADR-0067 教训）。
8. **禁止测试名、ADR 描述、计划记录与 fixture 脱节**——三者必须与 fixture 实际内容逐条对得上，Review 会 diff 到码流构造层（本次审查教训，写进每个任务的验收）。
9. **禁止以既有测试作为覆盖证据而不显式引用**——依赖既有测试时必须将其测试名写入 ADR 描述与计划记录。
10. **禁止只改文档不升版本**——`rule.toml` 的任何字段（含 `depth`）变更等同包内容变更，必须升级 `version`。

## 6. 实施计划驱动

开发由 `docs/implementation-plan.md` 驱动，该文件是**实施与恢复入口**：

- 顶部 header 维护当前状态：`Current Phase`、`Last Completed Step`、`Next Action`、`Blockers`。
- 按 M0–M7+ 里程碑排列，每个里程碑包含验收标准。
- 每次中断前**必须**更新 header。
- 代理恢复工作时从此文件定位断点，不依赖记忆。

## 7. 停止与上报协议

任一任务中发现以下情况，**立即停止该任务**并输出精确证据（报错原文 + `file:line`），交回用户决策：

- 当前 DSL 语言能力不可表达目标语义。
- 核心行为（ContextDirectory、analyzer runner 等）与预期不符。
- 需要扩展 context export/import 合同。
- 任何计划外的架构决策。

这是本项目多次 ADR 被探测推翻的既定流程（参见 ADR-0065/0066 先例），禁止自行绕过。

## 8. 文档策略

- **双语（中英文）**：所有 ADR、格式语言参考、bundled-profile 文档同时维护英文和中文版本。
- **ADR 编号连续**：`docs/adr/NNNN-*.md` + `docs/zh-CN/adr/NNNN-*.md`。
- **不删除已完成记录**：需求变更通过追加修订记录追踪。
- **计划外决策先暂停**：实现前先补充 ADR 和中英文文档。

## 9. 子代理使用规则

- 子代理**仅用于**探索性读取（跨文件检索、大文件扫描、独立核验）。
- **所有实现、提交、push 必须由主代理完成**。
- 奠基性文档（架构、设计、交接）无论多长都由主代理直接读取，不委托子代理。
- 子代理返回的信息视为线索而非事实，主代理须独立验证后才能采纳。

## 10. 探测先行原则

涉及 DSL 语言能力边界的任务，必须先在 scratch 副本用 `svtool rule check` 实测当前语法是否可表达，再决定是否需要新增语言能力。探测结论必须附实际报错输出作为证据。

参考先例：ADR-0065、ADR-0066 均被探测推翻了最初的设计假设。

## 11. 报告纪律与证据引用规范

1. **标识符原样复制**：所有 Commit SHA、Run ID、Job ID 等标识符必须从终端输出或 API 响应中直接复制粘贴，严禁手工键入或臆造。
2. **源码引用精确到行**：探测报告、ADR 描述及计划记录中的源码文件路径、行号和报错原文，必须与当前最新代码和编译器实际输出严格一致。
3. **全链路闸门并引**：当涉及语法或合同放宽时，必须全面探测并逐一引用该语法在全链路中的所有拦截闸门（如注解参数解析闸门、表达式三元 arity 检查闸门、IR 支配分析闸门、VM 执行校验等），禁止仅凭单一表层报错断定语言边界。

## 12. 新增音视频格式专项契约

当任务新增或实质扩展容器、编解码器、基本流、检测器、扫描器、规则包入口点或
跨层 payload 时，必须先完整阅读
`docs/format-extension-contract.md`，并遵守其双语 ADR、格式文档、fixture、
测试、版本升级、dev/ci/sanitize、Hosted CI 和实施计划记录要求。

该契约是完成门禁，不是建议清单。WAV、RF64、FLAC、MKV 等后续格式都必须先按契约
说明支持子集和延期边界，再实现。格式语义只能放在官方规则包和 `src/rules/`；
格式中立的 `src/core/`、Qt UI 和 session/schema 合同不得被塞入格式专属分支。
如果 DSL 或运行时能力不足，必须先做 scratch `svtool rule check` 探测并报告原始
错误，等待单独能力 ADR，不能绕过契约直接修改核心。

### English contract

When a task adds or materially changes a media format, read
`docs/format-extension-contract.md` in full before editing. That contract is
the completion gate: bilingual ADR, normative format documentation,
deterministic fixtures, tests, local dev/ci/sanitize verification, affected
Hosted CI, and an `implementation-plan.md` record are all required.

Keep format semantics in official rule packages and `src/rules/`. Change
`src/app/` only for format-neutral integration and presentation. Change
`src/core/` or the DSL only under a separately scoped capability ADR backed by
a scratch probe. If the existing contract cannot express the format, stop and
report the original probe error.

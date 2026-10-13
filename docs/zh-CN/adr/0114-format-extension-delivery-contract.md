# ADR-0114：格式扩展交付契约

- 状态：已接受
- 日期：2026-10-13
- 作者：StreamView contributors

## 背景

StreamView 已经具备稳定的 DSL、官方规则包、格式中立的 core 模型、跨平台测试
和 Release workflow。未来仍可能加入 WAV 等格式。一个格式即使“能打开”，也可能
缺少规范性支持子集、检测边界、截断语义，或者没有证明大输入仍受资源上限保护。

项目已有“先写架构决策，再写实现”的严格规则，且英文技术文档是规范性来源。现在
需要一份明确的格式交付契约，避免代理只完成解析器或只完成 UI 就宣布格式完成。

## 决策

采用 [`docs/format-extension-contract.md`](../../format-extension-contract.md) 作为
所有新增或实质修改媒体格式的规范性流程与边界契约，并维护
`docs/zh-CN/format-extension-contract.md` 中文伴随版本。

每个格式任务都必须：

1. 在编码前说明格式分类与分帧方式；
2. 在实现前新增连续编号的双语 ADR；
3. 定义包身份、规范依据、支持子集、延期项、检测证据、source span、诊断、资源
   限制、取消语义和版本后果；
4. 把格式语义放在官方 DSL 规则包与 `src/rules/`；
5. 只为格式中立集成和展示修改 `src/app/`；
6. 只有在单独能力决策和 scratch 探测支持下才修改 `src/core/` 或 DSL；
7. 增加确定性 fixture、定向测试和相关 fuzz 覆盖；
8. 运行本地 dev/ci/sanitize 矩阵、受影响的 Hosted CI 和 `git diff --check`；
9. 在验收前把真实证据写回 `docs/implementation-plan.md`。

规则包的解码输出或 manifest 发生任何变化（包括 `depth`），都必须升级包版本。
规则包保持纯数据，不能加入原生代码、进程/网络访问、符号链接或运行时依赖解析。

## 后果

每个格式任务都会增加文档和验证工作，但支持子集、失败行为和所有权边界将可审查。
例如 WAV 实现必须先决定覆盖 RIFF/WAVE PCM、WAVE extensible、RF64 或压缩变体中的
哪些范围；这些选择会成为 ADR 和规则包文档，而不会变成偶然行为。

如果现有 DSL 和规则 runner 已能表达目标格式，不要求新增 scanner 或修改 core。
反之，代理必须停下来报告探测失败，不能把格式专属解析塞进 core。

## 验证

本 ADR 只修改文档。验收要求：

- 英文与中文契约文档存在；
- 根目录 Agent 指针及现有 AGY 规则都指向该契约；
- 两个文档索引都包含链接；
- Markdown hygiene 测试和 `git diff --check` 通过。

未来格式 ADR 必须引用 ADR-0114 和格式扩展契约。

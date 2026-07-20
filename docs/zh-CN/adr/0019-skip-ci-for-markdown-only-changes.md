# 仅 Markdown 修改跳过 CI

状态：已接受
日期：2026-07-20

## 背景

hosted 矩阵会在三个桌面环境构建和测试。只修改 Markdown 文档的提交不会改变
C++ 目标、Qt 输入、规则、测试或发布二进制，但之前仍会消耗完整矩阵资源。实施计划
和双语技术文档会频繁更新，因此会产生不必要的排队时间和 runner 消耗。

## 决策

- `push` 和 `pull_request` 触发器都使用 `paths-ignore: '**/*.md'`。
- 只有当提交的所有变更路径都以 `.md` 结尾时，workflow 才会跳过。
- 只要提交包含源码、构建、workflow、配置、测试或规则包文件，即使同时修改 Markdown，
  仍运行完整矩阵。
- Markdown-only 修改通过 review 和本地 `git diff --check` 检查；不假定它们会产生可执行行为。

## 影响

计划更新和纯文档修正不会再启动重复的 Build/Test/Install job。workflow 不会校验 Markdown
语法；如果未来需要，必须显式加入文档工具。workflow 自身的修改不是 Markdown，因此仍会
触发矩阵。

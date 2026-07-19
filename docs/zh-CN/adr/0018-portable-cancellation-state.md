# 可移植的取消状态

状态：已接受
日期：2026-07-19

## 背景

阶段 1 最初使用 C++20 `std::stop_source` 和 `std::stop_token` 作为
`CancellationSource` 与 `CancellationToken` 的底层实现。本机 Apple Clang 21
以及 hosted GCC、MSVC 均可构建，但 hosted run `29693108274` 的 macOS 15
ARM64 runner 虽然启用了 C++20，其 Apple libc++ 仍未提供这两个类型，导致取消
测试在编译阶段失败。

启用 C++20 语言模式不代表所有受支持的标准库版本都完整提供每一项 C++20 库功能。
StreamView 当前只需要可复制、可轮询的 token，以及幂等且线程安全的取消请求；暂时
不需要 stop callback 或与 `std::jthread` 集成。

## 决策

- 保持 `CancellationSource` 和 `CancellationToken` 的公开 API 与可观察行为不变。
- source 创建的 token 使用带引用计数的共享状态，状态中保存原子取消标志。
- 取消请求继续保持线程安全、无异常且幂等。
- 任意 source 或 token 仍引用状态时，该状态必须继续有效。
- 规范性分析模型不把具体存储机制列为 API 契约。
- 只有当所有受支持工具链都提供标准 stop-token 功能，且运行时确实需要 callback 或
  `std::jthread` 集成时，才重新评估标准库实现。

## 影响

取消 API 无需降低项目 C++20 基线，即可在固定的 Windows、macOS 和 Ubuntu CI
工具链上一致编译。StreamView 需要自行维护少量同步代码，并为每个取消状态进行一次
共享分配。取消仍采用显式轮询，因此后续 VM 仍须按文档规定的指令间隔检查 token。

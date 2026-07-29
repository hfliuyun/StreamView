# 缓存稳定 Analysis Result，而不是 Live State

状态：已接受
日期：2026-07-29

## 背景

ADR-0029 提供有界 SQLite page，ADR-0032 绑定 durable identity 与 integrity envelope，
ADR-0033 则把 cache page 留在 `.svsession` 之外。`AnalysisCacheNamespace` 已绑定两种 owner
payload version，但 cache owner 要保存有用 analysis result，仍需要具体 body representation。

此时序列化 live H.264 analyzer 会让 persistence 依赖 scanner look-behind、pending start-code
state、source buffer、mapper progress、deferred queue、cancellation object、context payload 与
identifier allocation。这些状态的 invariant 不同，也不都在 page commit boundary 上稳定。遗漏
任一项的 partial checkpoint 可能重放或跳过 record/node，却仍显得合法。

## 决策

定义两种彼此独立的 version 1 big-endian body：

- H.264 progressive-index page：保存稳定 start-code record、首个全局 record index、
  indexed-through offset 与 end-of-source flag；
- materialized-result page：保存完整稳定 node record、parent identity、闭合 scalar value、
  location、metadata、specification 与 diagnostic。

规范 layout 与 bound 维护在 [Analysis Cache Owner Payloads](../analysis-cache-payloads.md)。

两种 body 都重复 `streamId` 与 `pageIndex`。owner decode 必须接收完整 expected page key，
并在 envelope validation 后拒绝 mismatch。只有 arithmetic、framing、string、flag、enumeration、
record relationship、location 与 node topology 全部验证通过，才返回 page。已知瞬态
materialization state（`Indexing`、`WaitingDependency`）不得保存。支持的 `QVariant` 闭合为
absent、Boolean、unsigned 64-bit、signed 64-bit 与 `QString`。

这些是 result representation，不是 analyzer checkpoint。progressive body 有意排除 pending
scanner state；materialized body 有意排除 mutable tree link 与 allocator state。两者都不包含
mapper、queue、cancellation、context-directory payload 或 thread ownership。decoder 不构造也不
恢复 live analyzer。

`FieldLocation` 新增经过验证的 standalone construction path，使 decoded location 无需伪造
完整 `SourceMapping` 也能证明自身 range/span invariant。

## 后果

- stable index/result page 现在具有确定且跨平台的 byte，可由现有 namespace 与 envelope 保护；
- 即使 envelope 只绑定 namespace 与 page kind，把合法 body 复制到其他 stream/page key 也会
  被发现；
- unsupported value 与 incompatible version 显式失败；malformed 或语义非法 page 整页丢弃，
  并可重建；
- future background owner 可以保存 completed page，而不序列化 transient analyzer internal；
- persistent analyzer recovery 仍不可用。它需要明确 owner lifecycle；如果确实需要恢复执行，
  还要为每个被省略的 live-state component 建立独立契约。

## 考虑过的方案

- 序列化完整 analyzer object graph：transient state 尚无稳定 commit boundary 或契约；
- 只保存 scanner cursor 与 tree node：cursor 不能表示 pending prefix 或 queued work，可能静默
  重复或跳过输出；
- 只在 SQLite 中保存 page key：同 kind body 被复制到错误 key 后仍可通过 envelope validation；
- 使用 `QDataStream` 或 native struct byte：隐式 Qt/platform version、padding 与 endianness 会使
  format 不稳定；
- 接受任意 `QVariant`：metatype registration 与 conversion behavior 不是闭合 durable data
  contract。

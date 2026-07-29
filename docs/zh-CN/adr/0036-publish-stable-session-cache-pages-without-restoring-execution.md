# 发布稳定 Session Cache Page，但不恢复执行

状态：已接受
日期：2026-07-29

## 背景

ADR-0035 提供有界 typed background owner，但 production session 尚未向它提交 page。H.264
analyzer 当前只返回已发布 node ID，不暴露 batch boundary 上产生的稳定 scanner record；
`AnalysisTree` 也没有 cache-page export operation。

cache reuse 必须从属于 analysis correctness。现有测试会直接构造 virtual source 与 temporary file
session；若在所有 `AnalysisSession` constructor 中静默启用用户 persistent cache，就会产生 filesystem
side effect，也容易把类似 path 的 virtual source identity 错当成 durable namespace。

version 1 materialized-result page 仍没有 manifest 或 final-page marker。本次变更可以发布 caller
已知完整的 atomic batch，但不能用当前 read API 在之后发现或证明该 batch 完整。

## 决策

除非 caller 提供带 database path 的 `AnalysisSessionCacheOptions`，application session API 默认不
启用 caching。production executable 设置 StreamView application/organization identity 后获取
`QStandardPaths::CacheLocation`，并使用其下的 `analysis-cache.sqlite`。测试与 embedder 必须用
显式 path opt in；任意 `RandomAccessSource` session 保持 cache-disabled。
cache activation 必须在首个 analyzer batch 之前完成；之后再启用会遗漏已经发布的 progressive
record，因此必须拒绝，而不能把它报告为 active 的完整 write path。

cache-enabled local-file session 从 analysis 使用的同一个已打开 `FileSource` handle 计算 version 1
`SourceFingerprint`。session restore 复用已经计算并与保存文档比对过的 fingerprint。session 将它
和 analyzer 的 exact `RuleEntryPointIdentity` 组合成 `AnalysisCacheNamespace`，再启动一个
`AnalysisCacheOwner`。fingerprint、namespace、path、driver、lock 或 cache open 的任何失败都只
禁用 cache acceleration，不会使 source open、rule resolution、analyzer creation 或 atomic session
replacement 失败。

`H264AnnexBAnalysisBatch` 新增 optional stable progressive-index update。每个实际 scanner batch
最多产生一个 update，包含：

- 首个 returned stable record 的 zero-based global index；
- 该 scanner batch 返回的 stable record；
- complete returned record 覆盖到的 exclusive byte frontier；
- 仅在 scanner `Complete` 时为真的 end-of-source flag。

scanner cursor 仅仅检查了 unresolved pending start code 之后的 byte 时，stable frontier 不会因此
推进。它推进到 returned record 的最大 end，并只在 scanner `Complete` 时推进到 source size。
只有 `Complete` 需要发布 terminal frontier 时才产生 empty update。deferred mapper/analyzer work 不会
重复该 update。

session 使用 progressive-index stream ID 0 与从 0 开始单调增加的 page index 写这些 update。
analyzer 默认 256-record batch 能放入 version 1 page。只为新 update 提交 write；queue acceptance
成功后才推进 page index。

analyzer terminal 后，session 以 materialized-result stream ID 0 导出完整 stable tree。node 按 stable
ID 顺序读取，转换时不改变 parent ID、value、location、metadata 或 diagnostic，并分成 version 1
body codec 接受的最大确定性连续 prefix。transient node、单 node oversized、非法 topology/value、
page-index overflow 或超过 256 page 都使 export 失败。所有结果 page 由一个 owner request 提交，
因此位于一个 SQLite transaction；不会写 empty page。

session 不会在 `analyzeBatch` 内等待 cache write，只用 nonblocking 方式 poll 之前 accepted 的
future；terminal batch 之后，application event loop 使用同一个 nonblocking poll。preflight
failure、`QueueFull`、shutdown 或已完成的 storage error 会禁用该 session 后续 write 并记录
cache error，但原 analyzer batch 原样返回。session 销毁时，owner 的 draining shutdown 会完成
已经 accepted 的 work。

本变更只写不读：不 enumerate page、不把 missing page 当作 end marker、不重建 `AnalysisTree`、不
发布 cached presentation snapshot、不填充 scanner queue，也不恢复 live analyzer。cache page 仍在
`.svsession` 之外。

## 后果

- production local-file analysis 现在会在完整 source/rule/version namespace 下发布 durable page，
  且不引入 path-only identity；
- direct local-file test 与 embedder 除非显式 opt in，否则没有 cache side effect；任意
  virtual-source session 保持 cache-disabled；
- progressive record 作为 stable batch output 暴露，但不暴露 scanner pending state；
- terminal materialized-result export 在 SQLite batch 层 all-or-nothing；无法放入 version 1
  有界表示的 tree 只是不使用该 cache acceleration；
- cache availability/health 不参与 analysis session 是否有效的判断；
- persistent cached presentation 与 live execution recovery 仍是未来另行 version 的工作。

## 考虑过的方案

- 在所有 session constructor 内启用 default cache：测试期间写用户状态，且 virtual source 没有合法
  fingerprint contract；
- 从 source path 或 `identity()` 派生 namespace：stale/caller-defined identity 可能静默绑定无关
  byte；
- 用 scanner cursor 作为 stable frontier：它可能越过尚未 emitted record 的 pending start code；
- 每个 materialized page 使用独立 request：后续 page 失败时可能暴露 partial result；
- 在 `analyzeBatch` 内等待每个 future：使 optional SQLite latency 进入 analysis publication path；
- 一直读取 page 到 missing 再附加到 UI：version 1 没有 complete set discovery 或 terminal-page
  proof。

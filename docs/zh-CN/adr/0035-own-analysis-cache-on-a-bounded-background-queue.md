# 在有界后台队列上拥有 Analysis Cache

状态：已接受
日期：2026-07-29

## 背景

ADR-0029 要求每个 `PagedCache` instance 及其 Qt SQL connection 在同一 thread 创建、使用和
销毁。ADR-0032 与 ADR-0034 已提供 durable analysis namespace，以及按完整 key 校验的
progressive-index/materialized-result payload，但 caller 仍只能在自己的 thread 同步执行 storage
work。

desktop analysis loop 当前在 GUI thread 上发布有界 batch。若在这里打开 SQLite、编码大型稳定
result、等待 write 或销毁 cache connection，就会把 cache acceleration 放进交互 latency path。
无界 async queue 只会把问题转移成 retained memory 与 shutdown behavior。

现有 version 1 materialized-result body 也没有 total-page count 或 final-page marker，而
`PagedCache` 有意不提供 page enumeration API。一直读取到 missing page，不能证明此前 page 构成
完整 snapshot。background owner 不得把这种歧义写成 persistent analysis recovery。

## 决策

rules 为一个 database path 和一个经过验证的 `AnalysisCacheNamespace` 暴露一个
`AnalysisCacheOwner`。database path 由 caller 提供；application cache-location policy 仍留在
rules/core module 之外。

启动 owner 时创建 dedicated worker thread，在该 thread 调用 `PagedCache::open`，并且只等待
open result。worker 独占返回的 `PagedCache`。每次 exact-key read、atomic batch commit，以及最终
cache destruction 都在同一个 worker thread 上执行。open 失败会 join worker，且不暴露 owner。

public API 接受 typed progressive-index 或 materialized-result page。write request 进入 queue 前，
submission thread 会：

1. encode 并语义校验每个 owner body；
2. 用 namespace-bound envelope 包装每个 body；
3. 拒绝 duplicate key，以及超出既有 1 到 256 page limit 的 batch；
4. 只为 worker commit 保留有界的 encoded page byte。

该 preflight 保留 body/envelope/full-key stack，同时避免 malformed object graph 或 oversized string
占据 background queue。worker 只以已验证的 key 和完整 envelope 调用 opaque atomic
`commitBatch`。

read 以完整 exact page key 进入 queue。worker 读取该 key，验证 namespace 与 page-kind envelope，
再以同一个完整 key decode body，最后才返回 typed page。outcome 区分 found、正常 missing、invalid
request、corrupt/incompatible cache byte 与 storage failure。不会返回 partially decoded page。

queue 同时按 outstanding request count 与 retained encoded write byte 设限，并计入正在执行的
request。默认上限为 64 个 request 和 16 MiB。任何会超过其中一个 bound 的 submission 都立即
返回 `QueueFull`，不阻塞也不保留该 request。shutdown 开始后，新 submission 返回
`ShuttingDown`。

每个 accepted operation 都有 future 表示 terminal result。`flush` 等待调用前已经 accepted 的
全部 request，本身不接纳新 work。显式 shutdown 幂等：停止 admission、drain 所有 accepted
request、在 worker 上销毁 `PagedCache`、join thread，然后返回。owner destructor 执行同样的
draining shutdown。cache storage/decode failure 只完成对应 request，不会杀死 worker 或丢弃后续
accepted work。

cache data 仍是 optional acceleration。caller 可以报告 cache failure、为当前 analysis 禁用后续
caching，再从 immutable source 与 exact rule identity rebuild。cache miss、corrupt page、full
queue 或 storage failure 不得使原本有效的 analysis session 失效，也不得替换其 live tree。

owner 只暴露 exact-page operation。version 1 progressive page 可以作为稳定 scanner output 保存和
检查，但不能恢复 scanner pending state 或 live analyzer。version 1 materialized page 可以在完整
key set 已知时 atomic 保存，但本 ADR 不定义 cross-process discovery 或 complete-snapshot
reconstruction。在从 cache 发布 presentation snapshot 前，必须另行定义 versioned manifest 或
final-page contract。

## 后果

- Qt SQL ownership 与 `PagedCache` destruction 保持在一个 dedicated thread；caller 可从任意
  thread 提交有界 value request；
- preflight 使每个 accepted write 有限，并把 invalid payload 留在 queue 外；SQLite 仍保证 batch
  all-or-nothing publication；
- queue pressure 与 shutdown 成为显式、可测试的 outcome，不是隐藏 blocking 或 detached work；
- exact-key read 会执行完整 envelope/body validation stack，并拒绝被复制到其他 stream/page
  coordinate 的 page；
- application 可接入 cache write，而无需让 cache health 参与 analysis correctness；
- persistent live analyzer recovery 仍不可用；在能表达完整性前，materialized snapshot read 也仍
  不可用。

## 考虑过的方案

- 把已打开的 `PagedCache` 移到另一 thread：owner 在 open 时捕获，wrong-thread destruction 会
  fatal；
- 在 caller thread 上使用 synchronous facade：正确但把 SQLite 与 shutdown latency 留在 GUI
  path；
- 使用 unbounded task queue：使 retained payload memory 与 shutdown time 随 producer speed 和
  source size 增长；
- 任意 typed object 入队后才 encode：被拒绝的 string、diagnostic 与 node vector 会在 page-byte
  bound 之外占用 memory；
- 把第一个 missing materialized page 当成 snapshot 结束：version 1 无法区分完整 result 与中断、
  eviction 或未知 page sequence；
- 从 progressive page 恢复 live analyzer：payload 有意省略 pending scanner、mapper、queue、
  cancellation、context 与 allocator state。

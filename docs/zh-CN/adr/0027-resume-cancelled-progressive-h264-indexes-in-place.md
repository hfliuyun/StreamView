# 原地恢复已取消的 H.264 渐进索引

状态：已接受
日期：2026-07-28

## 背景

最小 DSL 已经接受一种 progressive declaration：
`@index(progressive) sequence<Element> name = scan(h264_start_code);`。H.264
Annex B analyzer 使用 record count、inspected position 和 mapped byte 三种有界 batch
执行该声明，并在调用之间保留单调 scanner cursor、queued record、pending mapper、稳定
tree node，以及下一个 NAL 和 logical-view identifier。

这些状态目前只能支持普通的 `in-progress` continuation。当前 profile 会把 entry sequence
直接投影到 analysis root，不创建独立 sequence node。取消会把该 root 标为 `cancelled`，
终结 analyzer，之后每次调用都只重放 `cancelled`。这不满足 ADR-0004 对 resumable bounded
work 的要求，也没有落实 analysis model 明确允许的 `cancelled -> indexing` transition。

取消可能在不同边界被观察到。scanner cancellation 不会让 scanner failed 或 finished，
并会保留 cursor 与 pending start code；而在 record decode 或 mapping 中发生的取消，会把
该 record 发布为 terminal partial NAL。特别是 ADR-0025 有意把 cancelled EBSP-to-RBSP
mapper 设计为 terminal，并发布其 committed prefix。原地重试该 mapper 将需要可变 node
location、删除或重排已经发布的 child，以及不同的 mapper 契约。

跨进程恢复同样为时过早。持久 checkpoint 不能只凭当前类似 path 的 source identity 安全
绑定；它还需要后续的 source fingerprint、精确 rule-package identity 与 content hash、cache
namespace、schema version 和持久 tree/index storage。这些契约属于 M5 与后续 SQLite cache
切片。

## 决策

当前接受的恢复切片会在同一个 analyzer 对象内恢复已取消的 H.264 progressive index。
它不新增 DSL 语法，也不创建可序列化 checkpoint。保留 analyzer 对象本身就是 checkpoint：
其中继续持有同一个 source object、已经编译的 typed program、analysis tree、scanner state、
queued record、deferred scanner result、单调 identifier，以及尚未 terminalize 的 pending work。

analyzer 将新增显式 `resumeAfterCancellation` 操作，只在以下条件全部满足时接受：

- analyzer 以 `Cancelled` status 终结；
- 承载 entry sequence 的 analysis root 当前状态为 `MaterializationState::Cancelled`；
- 提供的 replacement cancellation token 尚未被请求取消。

replacement token 可以为空，使后续工作在下一次恢复提供 token 之前不可取消。已经 requested
的单向 token 不能复用。被拒绝的恢复不会改变 tree state、scanner state、cursor、queue、
diagnostic、identifier 或 terminal result。`Complete`、`SourceError`、`ResourceLimit` 和
`InvalidRule` 不能通过该操作恢复。

成功恢复不会读取 source、创建 node 或推进 scanner，只执行以下状态变更：

1. 删除直接挂在 analysis root 上且 code 为 `Cancelled` 的全部 diagnostic，并把 root 从
   `cancelled` transition 为 `indexing`。
2. 替换 analyzer 与 scanner 的 cancellation token。
3. 清除 analyzer 缓存的 cancelled terminal result。
4. 如果 deferred scanner result 本身是 `Cancelled`，消费该结果，让下一 batch 继续同一个
   scanner；其他 deferred scanner result 与全部 queued record 都原样保留。

core analysis tree 将为第 1 步提供受限 resume 操作。它只接受 cancelled node，只删除直接挂在
该 node 上且 code 为 `Cancelled` 的 diagnostic，并把它 transition 为 `indexing`；不会修改
descendant state、descendant diagnostic 或任何非 cancellation diagnostic。这样恢复后的 root
不会继续展示已经过时的 root-level pause，同时其下真实的 partial-result 证据仍会保留。

start-code scanner 允许所属 analyzer 替换 cancellation token，但不会重置 scan state。cursor、
累计 inspected count、pending start-code offset/length、trailing-zero run、buffer、finished flag
与 failed flag 都不改变。因此下一 scan batch 不会重放已经返回的 record，也不会跳过尚未检查
的 start-code candidate。

恢复发生在 sequence 层，不会重试已经发布的 element：

- 如果 cancellation 由 scanner 观察到，不会产生 terminal scanner failure；恢复从保存的
  cursor 与 pending boundary 继续。
- 如果 cancellation 发生在 NAL header decode 或 RBSP mapping 中，既有 analyzer 契约会先把
  该 NAL、已经完成的 child 和 committed RBSP prefix 发布为 cancelled partial result。这个
  record 随即视为已提交，index recovery 不会再次 decode 或 map；恢复从下一个 queued 或
  unscanned record 继续。
- 后续 batch 不会重放此前已经返回的 node identifier。新 record 只追加一次，并继续使用保存的
  `nextNalUnitIndex` 与 `nextViewId`。

恢复后的扫描正常到达 source 末尾时，analysis root 进入 `materialized`。已经作为
`cancelled` 提交的 descendant NAL 或 RBSP region 仍保持 cancelled，因此完成的 index 继续
报告 partial results，也不会被视为 fully materialized。这明确区分“index 已扫描到 source
末尾”和“每个 indexed element 都完整”。同一规则支持多次 cancel/resume cycle。

现有 per-batch budget 与 cancellation interval 均不改变。恢复本身不消耗 record、inspected
position、mapped byte、instruction 或 node budget；恢复后的第一轮 analysis batch 与普通
batch 使用完全相同的边界。

持久恢复仍是独立功能。未来 durable checkpoint 必须先验证 source fingerprint 与精确 rule
identity，才能恢复 scanner 或 tree state；其表示也不必等同于当前 private in-memory field。
本决策不承诺 checkpoint serialization format，也不会仅为恢复而复制 analysis tree。

## 后果

用户与 host 可以停止耗时的 Annex B index，保留全部已发布结果，并在之后继续到同一 source
末尾，而无需重新扫描已完成前缀或重新编号 node。scanner boundary 上的取消可以完整继续；
单个 NAL 内的取消会让该 NAL 保持清晰可见的 partial 状态，同时允许继续索引之后的 NAL。

append-only tree 与 terminal mapper 契约保持不变。恢复不需要删除 node、重排 child、修改
source location 或维护无界 replay/deduplication table。

这个原地边界有意小于 crash recovery。失去 analyzer 对象仍会失去可恢复状态，直到 source/rule
identity 与 durable cache 契约完成。

## 考虑过的方案

- 立即序列化 scanner、mapper、tree 与 queue state：没有 source fingerprint、精确 rule
  identity、cache versioning 与 durable storage 时并不安全，也会把 M5 和 SQLite 工作提前
  拉入本切片。
- 从 byte zero 重新扫描并去重 record：会重复 source work，需要第二套稳定 identity index，
  并可能在 partial committed record 附近产生重复。
- 恢复 cancelled mapper 并重写已经发布的 RBSP node：与 ADR-0025 terminal replay 以及
  append-only analysis-tree 契约冲突。
- 观察到取消后仍让 root 保持 `indexing`：会隐藏 analysis model 要求的明确 cancelled state
  与 diagnostic。
- 恢复后保留 root cancellation diagnostic：会让当前 indexing 或 materialized root 继续把
  已经过时的暂停显示为错误。

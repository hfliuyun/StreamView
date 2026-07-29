# 核心 Source 与 Bit 模型

状态：阶段 1 实现基线。英文 [Core Source And Bit Model](../core-model.md) 是规范性
文档，本文件提供对应中文说明；如有歧义，以英文为准。

## Bit 坐标

Source bit 地址是未修改媒体源中的从零开始绝对 bit offset。一个字节内 bit offset
0 表示最高有效位，bit offset 7 表示最低有效位。例如 `byte=2, bit=3` 对应绝对
bit offset 19。

Source span 和 logical range 都使用半开区间 `[start, start + length)`。若计算排他
结束位置时发生 64 位溢出，该区间无效。空区间可以表示边界，但 source mapping
中不能包含空 span。

Logical 地址只有与 logical view identity 一起使用才有意义；一个 view 的 range
不能通过另一个 view 的 mapping 解析。

## Source Mapping

Source mapping 使用一组有序的绝对 source span 表示一个完整 logical view，并维持：

- 每个 source span 都非空；
- span 按 source 顺序排列且不重叠；
- 相邻 span 自动合并；
- logical view 长度等于所有 source span 长度经过溢出检查后的总和。

解析 logical range 时会返回包含原 logical range 与精确 source span 的字段位置。
因此跨越被排除 source byte 的 logical range 会解析为多个 span。例如，一个 view
由 source bits `[0, 16)` 与 `[24, 40)` 组成时，logical bits `[8, 24)` 会映射到
source bits `[8, 16)` 与 `[24, 32)`。

发生溢出、span 重叠或乱序、view identity 不一致、range 超出 logical view 时，
mapping 会拒绝请求。没有精确 mapping 的值后续必须表示为 computed field，不能
伪装成有 source 位置的字段。

## 位置感知上下文目录

`ContextDirectory` 记录供后续语法引用、已经完成且有 source backing 的 definition。
封闭的 definition kind 包含 H.264 SPS/PPS、AAC AudioSpecificConfig 和 ISO BMFF
sample description。key 由 kind、host 分配的数字 scope 和由 kind 解释的 value 组成。
scope 零是独立 elementary stream 的自然 scope；container 为 context ID 可能冲突的
不同 track 使用不同稳定 scope。

每个 definition 包含非空绝对 source span、analysis-node ID、单调 definition ID 和精确
dependency-generation ID。给定 key 和查询 source position，目录选择 exclusive
source-span end 不晚于查询位置、且结束位置最大的 definition。查询仍位于
definition 内部时它不可见；到达排他结束位置时恰好可见，也永远不会影响
更早位置。

同一 key 的 definition 不能重叠，但注册不必遵循 source 顺序，因此 lazy 与
known-offset 分析仍能保持确定的位置查询。ID 按追加顺序分配、永不复用；
lookup 返回的 snapshot 在后续注册之后仍有效。

dependency 必须是在 dependent definition 开始前选中的精确 generation。lookup 会在
consumer 位置重新检查每个绑定 dependency 是否仍是当前 generation。发生重定义
时，stale dependent context 会报告 `dependency-unavailable`，不会回退或猜测。
resolution 最多访问 64 个 definition；后续交叉重定义形成的 dependency cycle 与
超过该上限的 chain 都报告 unavailable。

malformed definition 不会注册，也不会遮蔽此前的有效 generation。被拒绝的注册
不改变可见目录状态。目录不保存格式专用 payload、不读取 source，并沿用
analysis worker 单写者模型。首个消费该目录的正式格式规则会把一个目录实例
接入自己的 analysis session。持久化和 SQLite paging 属于后续独立契约；详见
[ADR-0028](../adr/0028-resolve-context-generations-by-source-position.md)。

## 严格只读 Source

随机访问 source 接口只暴露大小、身份和 `readAt`。本地文件实现只使用 Qt 只读
模式打开媒体源，不提供写入或调整大小操作。并发随机读取只在 seek/read 周围串行
执行，不会把完整文件载入内存。

读取结果分为：

- `complete`：目标 buffer 已完整填充；
- `end-of-source`：读取到达或开始于当前 source 末尾之后；
- `error`：source 无法完成读取。

`end-of-source` 可以包含有效的部分字节数，调用方不得解释该长度之后的内容。

## 持久文件 Fingerprint

`FileSource::fingerprint()` 在同一个已经打开的只读 file handle 上计算 version 1 identity；
类似 path 的 source identity 永远不能充当 durable identity。小于等于 3 MiB 的文件使用
全文 SHA-256 与 size；更大的文件使用 size、Unix epoch 纳秒 modification time，以及首、
中、尾三个 1 MiB window 拼接后的 SHA-256。中间 window 从
`floor((size - 1 MiB) / 2)` 开始。全文 hash 的读取量因此不超过采样模式，两个 mode 都只
保留 64 KiB 工作 buffer。

实现会在 hash 前后从同一个 OS handle 比较 size 与纳秒 modification time。文件 size 与
打开 source 时不一致，或计算期间 snapshot 变化时，会显式诊断 changed，不产生 fingerprint。
小文件 modification time 只用于该一致性检查，不写入 durable value，因此只 touch 未改变的
content 不会造成 mismatch。不支持的 metadata 与 I/O error 都是显式结果。virtual source
不会回退使用 path identity。详见
[ADR-0031](adr/0031-versioned-file-source-fingerprints.md)。

## 持久 Session Document

`SessionDocument` 是一个 local file 与一个完整 rule entry-point identity 的紧凑、versioned
user-state 记录。Version 1 使用闭合 UTF-8 JSON schema，包含 source path 与描述性 identity、
完整 `SourceFingerprint`、package ID/version/content hash 与 entry-point ID、bookmark、
annotation、expanded analysis path，以及 raw/selection view state。所有 64-bit quantity 使用
canonical decimal string，避免 JSON binary64 丢失 source-coordinate 精度。parser 限制 document
size、nesting、text 与 collection count，并拒绝 duplicate、missing、unknown、mistyped、
unsupported、malformed 或 source 外 value。

保存使用关闭 direct-write fallback 的 `QSaveFile`。恢复会验证完整 document，以只读方式打开
local file，比较从同一 handle 新计算的 fingerprint，执行精确且兼容的 catalog lookup，从该
resolved entry 构造 analyzer，最后才绑定 user state。失败不会返回 replacement session，也不会
应用 saved coordinate。bundled H.264 analyzer 同样保留完整 catalog-resolved identity。
path-like identity 不能授权恢复，virtual source 也不能借此 fallback 持久化。cache page 与 live
analyzer state 不属于 `.svsession`；参见[会话格式](session-format.md)与
[ADR-0033](adr/0033-save-exact-analysis-sessions-as-atomic-json.md)。

## 分页 Source 访问

`SourcePager` 在 `RandomAccessSource` 之上提供有界分页视图。每页最多 64 KiB，
通过经过检查的 page index 定位；加载一页不会读取相邻页，也不会保留历史页缓存。
当 source 大小不是 64 KiB 的整数倍时，最后一页只包含声明的剩余字节。到达声明的
source 末尾的页会标记为 `end-of-source`，但仍属于成功的页结果。

如果 source 在 `sizeBytes()` 声明的字节返回完之前报告 `end-of-source`，该页会被
视为错误，而不是合法的短页。source 错误只保留 source 明确报告的字节，不会暴露页
buffer 中未写入的部分。超出范围的 page 是空的 end-of-source 结果；发生 page 坐标
溢出则是错误。

## 分页分析缓存

`PagedCache` 使用 SQLite WAL 把可重建的 progressive-index 与 materialized-result
数据保存为不透明 page；它不会缓存媒体 source byte，也不解释 payload 编码。调用方
以数据库路径和不透明 namespace 打开一个 thread-affine cache instance，然后按精确
page key 读取，或一次提交完整 batch。任意 namespace 仍只负责分区，不授权跨 session
复用。production reuse 使用 `AnalysisCacheNamespace`：对经过验证的 source
fingerprint、完整 package ID/version/content hash 与 entry-point ID、SQLite schema、
namespace-format version 和两种 payload-format version 进行 domain-separated SHA-256。
payload-envelope format version 也包含在内；类似 path 的 source identity 不是输入。

owner data 存储前会包装在 version 1 `AnalysisCachePayloadEnvelope` 中。固定 header
绑定 namespace digest、封闭 page kind、该 kind 的 payload version 与 payload length，并用
SHA-256 验证 body。envelope 加 body 仍受 64 KiB page 上限约束。owner 解释 body 前，decode
会拒绝错误 namespace/kind、未知 version、非法 framing、截断、trailing byte 和 digest
mismatch。`PagedCache` 自身仍把完整 envelope 当作 opaque byte。详见
[ADR-0032](adr/0032-bind-analysis-cache-namespaces-and-payload-envelopes.md)。

version 1 owner body 现在为 completed progressive-index record 与 stable materialized-result
node 提供确定的 big-endian 表示。两者都会重复完整 stream/page coordinate，并在 envelope
decode 后与 requested page key 比较。index record 校验 checked byte relationship 与顺序；
result node 使用闭合 scalar value set，并保留 stable ID、parent ID、state、multi-span
location、metadata、specification 与 diagnostic。瞬态 indexing/waiting state 不得保存。详见
[owner payload 规范](analysis-cache-payloads.md)与
[ADR-0034](adr/0034-cache-stable-analysis-results-not-live-state.md)。

每个 payload 为 1 至 64 KiB。一次原子提交包含 1 至 256 个互异 page key，因此最多
16 MiB；成功时所有替换一起可见，失败时一个也不可见。page 缺失是正常 cache miss。
wrong-thread 访问和非法 key 会在接触 storage 前被拒绝。一个 live instance 独占其
database path；在 owner 关闭或 stale process lock 被回收之前，第二次打开会失败。

打开 cache 时会检查 QSQLITE runtime driver、WAL 配置、StreamView application ID、
schema version、必需 schema 和 SQLite 完整性。SQLite 回滚中断的 transaction；
persistent marker 让下次打开可以显式识别并清理由 process crash 遗留的 batch，清理
失败则中止打开。在 `synchronous=NORMAL` 下，这不承诺断电后仍持久。cache 只保存
已经提交的可重建 page，不恢复 live analyzer、scanner、mapper、analysis tree 或
context directory。body serializer 有意省略 scanner pending state、mapper state、queue、
context payload、identifier allocation 与 thread ownership。

`AnalysisCacheOwner` 现在会在 dedicated worker thread 上打开、使用并销毁一个
`PagedCache`。typed write 在入队前完成 body/envelope validation；queue 默认同时受 64 个
outstanding request 与 16 MiB retained encoded write byte 约束。exact-key read 在 worker 上反向
执行完整 storage stack，并区分 missing、corrupt、invalid 与 storage outcome。`flush` 等待此前
accepted 的 request；draining shutdown 拒绝新 work、完成 accepted work、在 owner thread 销毁
cache 并 join。cache failure 仍只属于 optional-acceleration failure。详见
[ADR-0035](adr/0035-own-analysis-cache-on-a-bounded-background-queue.md)。

该 owner 不会使 persistent analyzer recovery 可用。version 1 materialized page 没有 manifest、
total-page count 或 final-page marker，因此尚不能被发现并发布成可证明完整的 cached presentation
snapshot。live recovery 还需要为每个被省略的 analyzer-state component 建立明确契约。

## Bit Reader

有界 bit reader 每次按最高有效位优先顺序读取 1 至 64 bit，位置相对于声明的
source span。读取会先检查声明边界，再只请求组成该值所需的 source 字节。

成功读取后位置按请求宽度前进；非法宽度、超出声明范围、source 意外截断和读取
错误都不会推进位置。解析器因此可以保留此前的完整字段，并在未解析字段边界附加
精确诊断。

合法示例：从 `0b10110010` 先读取 3 bit 得到 `0b101`，再读取 5 bit 得到
`0b10010`。非法示例包括请求 0 或 65 bit，以及从 8-bit 有界范围请求 12-bit 值。

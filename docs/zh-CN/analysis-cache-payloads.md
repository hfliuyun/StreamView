# Analysis Cache Owner Payloads

状态：规范
版本：1

本文规定放在 version 1 `AnalysisCachePayloadEnvelope` 内的 owner-defined body：一种用于
H.264 progressive-index record，另一种用于稳定的 materialized-result node。两者都是可重建
cache 表示，都不是 live analyzer checkpoint。

## 通用编码

除明确标为 signed 的 field 外，所有整数都是 unsigned big-endian。progressive-index 的 size
与 offset 以 byte 为单位，field location 以 bit 为单位。reserved value 必须为零；flag bit 与
enumeration code 都是闭合集合，decoder 必须拒绝 unknown value。

每个 body 必须放入 `AnalysisCachePayloadEnvelope::maximumPayloadBytes()`，即 65,440 byte，
使 96-byte envelope 加 body 不超过一个 64 KiB `PagedCache` page。body 会重复 page key 的
`streamId` 与 `pageIndex`。decoder 必须接收完整 expected `PagedCachePageKey`，要求正确 page
kind，并比较两个重复值。把合法 body 移到另一个 key 下只能视为 cache miss 或损坏，不能授权
复用。两个 key coordinate 都必须位于 `PagedCache` 与 SQLite 接受的非负 signed 64-bit
范围。

字符串使用 32-bit byte length 加 strict UTF-8；单个字符串最多 32 KiB，且必须可以无 replacement
character 地往返。encoder 拒绝不表示 Unicode scalar text 的 `QString`。

写入顺序固定为：

1. 编码并完成 owner body 的语义验证；
2. 使用 expected namespace 与 page kind 包装 `AnalysisCachePayloadEnvelope`；
3. 以同一个完整 page key 提交整个 envelope。

读取按相反顺序进行：读取精确 page key、验证 envelope，再用同一完整 key 解码 body。仅验证
envelope 不会绑定 `streamId` 或 `pageIndex`，因此不构成完整 cache read。

## Progressive-Index Body

version 1 progressive-index header 为 56 byte：

| Offset | 大小 | Field |
| ---: | ---: | --- |
| 0 | 8 | magic `SVPIDX\0\0` |
| 8 | 4 | format version `1` |
| 12 | 4 | flags；bit 0 为 `endOfSource` |
| 16 | 8 | stream ID |
| 24 | 8 | page index |
| 32 | 8 | 首个全局 record index |
| 40 | 8 | indexed-through byte offset |
| 48 | 4 | record count |
| 52 | 4 | reserved zero |

每个 record 为 48 byte：

| 相对 offset | 大小 | Field |
| ---: | ---: | --- |
| 0 | 8 | start-code byte offset |
| 8 | 4 | start-code length，只能为 3 或 4 |
| 12 | 4 | reserved zero |
| 16 | 8 | NAL-unit byte offset |
| 24 | 8 | NAL-unit byte length |
| 32 | 8 | trailing-zero byte offset |
| 40 | 8 | trailing-zero byte length |

record count 可以为零，并受剩余 page 容量限制。first record index 加 record count 不得发生
64-bit overflow。每个 record 都必须用 checked arithmetic 证明：

- `nalUnitOffset == startCodeOffset + startCodeLength`；
- `trailingZeroOffset == nalUnitOffset + nalUnitLength`；
- 完整 record 在 `indexedThroughByteOffset` 处或之前结束；
- 所有 record 有序且不重叠。

decode 会从 byte offset/length 重建 source span。零长度 NAL unit 或 trailing-zero run 没有 span；
非零 range 转为 bit 时不得 overflow，且必须精确匹配重建 span。该编码不包含 scanner pending
prefix、buffered source byte、pending start code、cancellation token、mapper state 或排队中的
analyzer work。

## Materialized-Result Body

version 1 materialized-result header 为 40 byte：

| Offset | 大小 | Field |
| ---: | ---: | --- |
| 0 | 8 | magic `SVMATR\0\0` |
| 8 | 4 | format version `1` |
| 12 | 4 | reserved zero |
| 16 | 8 | stream ID |
| 24 | 8 | page index |
| 32 | 4 | node count |
| 36 | 4 | reserved zero |

一个 body 包含 1 至 1,024 个 node。每个 node 先写固定 header，再按以下顺序写 variable field：

| 大小 | Field |
| ---: | --- |
| 8 | stable node ID |
| 8 | parent ID，无 parent 时为零 |
| 4 | node-kind code |
| 4 | materialization-state code |
| 4 | value-kind code |
| 4 | flags：bit 0 location，bit 1 specification |
| variable | name、type name 与 description string |
| variable | flag bit 1 存在时的 standard 与 clause string |
| variable | encoded value |
| variable | flag bit 0 存在时的 field location |
| 4 | diagnostic count |
| variable | diagnostics |

node-kind code 为：`1` root、`2` structure、`3` syntax field、`4` computed field、`5`
compressed payload、`6` region。state code 是 core enumeration 加一：稳定值为 `1` lazy、`4`
cancelled、`5` unsupported、`6` invalid、`7` materialized。已知瞬态值 `2` indexing 与 `3`
waiting-dependency 在 cache body 中非法；其他 code 属于 unsupported。

value-kind code 为：`0` absent、`1` Boolean、`2` unsigned 64-bit、`3` signed 64-bit、`4`
string。Boolean、unsigned 与 signed value 占八 byte；Boolean 只能为零或一。signed value 保存
two's-complement bit pattern。禁止隐式 `QVariant` conversion。

### Location

location layout 为：

| 大小 | Field |
| ---: | --- |
| 8 | logical view ID |
| 8 | logical start bit offset |
| 8 | logical bit length |
| 4 | source-span count |
| 4 | reserved zero |
| 每项 16 | source start bit offset 与 bit length |

最多允许 1,024 个 source span。logical range arithmetic 不得 overflow；每个 source span 必须
非空、有序、不重叠，并与前一个 span 规范地分离，adjacent span 必须先合并再编码。所有 span
总长度必须等于 logical length。零长度 logical range 没有 span。syntax field 必须有 location，
computed field 禁止 location。

### Diagnostic

每个 node 最多有 256 个 diagnostic。每项先包含四个 32-bit value（diagnostic code、severity
code、flags、reserved zero），再写 message 与 field path string；flag bit 0 存在时追加 location。
diagnostic code 是 core enumeration 加一的闭合范围 1 至 7；severity 是 core enumeration 加一的
闭合范围 1 至 3。message 不得为空。

### Node Topology

node ID 非零且在一页内连续。root 只能是 node 1，且没有 parent。每个 non-root 都有非零且
小于自身 ID 的 parent ID。后续页可以引用前一页的 parent，因此完整 cross-page reachability
要由 owner 在加载全部所需 page 后验证。name 不得为空；specification 存在时 standard 与 clause
都不得为空。

body 只保存稳定 result record，不序列化 child vector、mutable `AnalysisTree`、next-node
allocator、scanner/mapper state、deferred result queue、cancellation state、context-directory
payload 或 cache-thread ownership。consumer 可以从经过验证的 page 重建 presentation snapshot，
但前提是另行取得并验证了完整 key set；不能据此恢复 live analyzer。

## Background Owner Stack

`AnalysisCacheOwner` 为一个经过验证的 namespace 接受 typed page。write submission 在保留
request 前完成 body encoding、envelope encoding、full-key check、duplicate check 与 256-page
batch bound。dedicated worker 只提交这些有界 encoded page。read submission 接受一个 exact full
key；worker 先读取它、按该 namespace/kind 验证 envelope，再按同一个 key decode body。

storage 中不存在数据时 owner 返回 missing；任何 envelope/body failure 返回 corrupt。两种 outcome
都不会返回 partial typed page。queue pressure、shutdown 与 storage error 是彼此独立的显式
outcome；caller 应 rebuild 或禁用 optional cache，而不改变有效 live analysis。

该 exact-page stack 不定义 page discovery。特别是，missing version 1 materialized page 不是
final-page marker。consumer 必须先拥有另行验证过的完整 key set，才能执行 cross-page
reachability validation 或发布 presentation snapshot。

production H.264 session 可以通过该 stack 发布 version 1 page。每个实际 scanner batch 最多暴露
一个 stable progressive-index update；exclusive frontier 是 completed record 的最大末端，仅在 scan
complete 时推进到 source size。stream 与初始 page index 均为 0。terminal stable tree 按 node ID
顺序导出为 codec 接受的最大连续 prefix，不产生 empty page，并作为一个最多 256 page 的 batch
提交。该 producer 行为只写不读：不提供 complete-key manifest，也不恢复 cached snapshot 或 live
analyzer。详见
[ADR-0036](adr/0036-publish-stable-session-cache-pages-without-restoring-execution.md)。

## 失败处理

encode 会区分非法语义输入、不支持的闭合 value 与 page/text overflow。decode 会区分 malformed
body、重复 page key mismatch、不支持的 body version 与不支持的闭合 value。任一失败都丢弃
整页结果。调用方应从 immutable source 与 exact rule identity 重建，不能局部绑定已解码 node，
也不能静默重解释 byte。

详见 [ADR-0034](adr/0034-cache-stable-analysis-results-not-live-state.md)与
[ADR-0035](adr/0035-own-analysis-cache-on-a-bounded-background-queue.md)。

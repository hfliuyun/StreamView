# 绑定 Analysis Cache Namespace 与 Payload Envelope

状态：已接受
日期：2026-07-29

## 背景

ADR-0029 在 source/rule identity 尚未完成时，有意让 `PagedCache` 接受调用方给出的
opaque namespace 与 opaque page byte。ADR-0030 已提供完整 package identity，ADR-0031
也提供版本化 source fingerprint。persistent cache 复用仍需要唯一的构造路径，确保不会漏掉
entry point、SQLite schema 或 owner payload format。

只有 SQLite namespace 分区还不能让 page byte 自验证。被复制到其他 namespace、按错误 page
kind 解码、由不兼容 owner version 写入、发生截断或损坏的 page，都必须在 body 到达 scanner、
tree 或 materialized-result decoder 前失败。

## 决策

`RuleEntryPointIdentity` 是完整 rule execution identity：package ID、package version、
package content SHA-256 和 entry-point ID。构造时验证每个 component。catalog restore 仍必须
精确 resolve 该 tuple，并诊断 installed content 缺失、冲突或不兼容。

`AnalysisCacheNamespace` version 1 只能由以下值创建：

- 一个经过验证的 `SourceFingerprint`，包括 algorithm version 与 mode-specific field；
- 一个完整 `RuleEntryPointIdentity`；
- `PagedCache::schemaVersion()`；
- cache namespace-format version；
- payload-envelope format version；
- progressive-index 与 materialized-result payload-format version。

builder 进行 domain separation 与 length framing，发布
`sv-cache-v1-sha256:HEX`，其中 `HEX` 是完整 tuple 的 SHA-256。filesystem path
和 `RandomAccessSource::identity()` 都不是输入。不支持或为零的 version 会被拒绝；
文本满足现有 SQLite namespace 上限，并直接传给 `PagedCache::open`。

每个存储 page body 使用 `AnalysisCachePayloadEnvelope` version 1。固定 96-byte 的
big-endian header 包含：

- 八 byte magic 与 envelope version；
- 封闭的 page-kind code 与对应 owner payload version；
- 必须为零的 reserved byte；
- 32-byte analysis-cache namespace digest；
- payload length；
- payload SHA-256。

其余 byte 是 owner-defined payload；header 加 body 不得超过既有 64 KiB page 上限。decode
要求 expected namespace 和 page kind，并拒绝未知 version、namespace/kind mismatch、非零
reserved field、非法 length、截断、trailing byte 和 payload digest mismatch。`PagedCache`
仍不了解该格式，只把完整 envelope 当作 opaque byte 保存。

envelope 版本化的是 payload 边界。后续 ADR-0034 已定义 stable progressive-index 与
materialized-result body，但 scanner pending state、mapper state、mutable tree/allocator
state、context payload 与 background cache owner 仍是独立工作。

## 后果

production cache user 现在只有一个确定 namespace，它绑定全部现有 durable identity 与 storage
format version。source content 或 sampled metadata、package ID/version/hash、entry point、
SQLite schema、namespace framing 或任一 payload version 变化时，都会选择另一个 namespace。
envelope version 变化时也会选择另一个 namespace。

page owner 在解释自身数据前获得有界的 corruption 与 compatibility 检查。合法 envelope 只证明
framing、namespace association、kind、version、length 与 byte；body 语义仍由 owner 验证。

系统没有引入 path-only API。直接给 `PagedCache` 传 opaque namespace 仍不授权跨 session
复用。只有接入 background owner，并为 execution resume 所需的每项 live state 建立明确契约，
persistent analyzer recovery 才可用。

## 考虑过的方案

- 拼接可读 identity 字符串：tuple 演进时容易产生 delimiter 歧义或漏掉 field。
- 把 source/rule column 写入 SQLite：让通用 page store 依赖 rules，并使每个 identity field
  变化都需要 schema migration。
- 只信任 SQLite namespace：owner decode 前无法发现被复制、错标、陈旧或损坏的 page byte。
- 只版本化 envelope：index 与 result body 独立演进，需要各自 version。
- 在同一次修改中序列化 live H.264 analyzer：scanner、mapper、tree、queue、context 与
  identifier state 仍需要显式契约。

# 不复制数据地读取 source-mapped logical view

状态：已接受
日期：2026-07-28

## 背景

`SourceMapping` 已经把一个 logical view 表示为有序的绝对 source span，`FieldLocation` 也能
把 logical field 解析为一个或多个不连续 span。analysis tree、field inspector、raw-data
highlight 和 source-bit lookup 都会保留这种 multi-span location。

剩余执行路径仍只支持 direct mapping。`BitReader` 只能读取一个连续 `SourceSpan`，DSL
executor 则接收另一份 mapping，并拒绝 location 不是单个连续 span 的字段。把 transformed
view 复制进 decoded buffer 虽然便于读取，却会丢失只读 source 关系、复制可能很大的媒体
数据，并引入第二套坐标权威。

M4 必须先提供通用 mapped read，H.264 EBSP-to-RBSP helper 才能排除
emulation-prevention byte，同时让跨越该 gap 的字段仍具有精确 location。

## 决策

`BitReader` 除既有 contiguous-range 构造方式外，还将支持有序 source-mapped backing。
对外公开的 position、remaining length、seek offset 和 read bound 都是该 backing 内的
logical offset。mapped reader 只转发有序 source span 指定的 bit；source gap 永远不会被
读取，也不贡献 logical bit。

reader 会把 logical length 和规范化后的有序 backing span 作为只读 execution metadata
公开。`SourceMapping` 仍相对于当前 active analysis source，本切片不会给它增加单独的 source
identity。analysis-session owner 必须从同一个 `RandomAccessSource` 构造 reader 与 mapping；
source identity 和 fingerprint 验证仍由 session 负责。

mapped reader 只能从合法 `SourceMapping` 或通过该 mapping 解析出的合法 logical slice
创建。空 mapping 会创建零长度 reader。实现可以规范化 mapping segment 或为其建立索引，
但不会物化完整 logical view 的 decoded copy。既有 contiguous constructor 保持原行为，并
视为单 span logical backing。

`readBits` 保持现有 `1..64` 契约和 MSB-first 数值顺序；一次 read 可以在 source-span 边界
拆分。工作量和临时存储受请求 bit 数及这些 bit 跨越的 mapping segment 限制，而不受完整
view 长度影响。越过 logical bound 返回 `EndOfRange`；source 数据不可用返回 `EndOfSource`；
I/O error 或不一致的 successful source read 返回 `SourceError`。任何失败都不推进 reader
position，包括同一次 value 的前一 segment 已成功读取、后一 segment 才失败的情况。`seek`
只改变 logical position，并拒绝超过 logical length 的 offset。

DSL execution boundary 会在执行 bytecode 前验证：reader 的完整规范化 backing 必须精确等于
execution mapping 中从 `logicalStart` 开始、长度为 reader logical length 的 source
resolution。mapping slice 越界、span 重排、缺少或增加 span，以及 source length 不同，都
属于 invalid typed execution。在当前 active analysis source 内，这可以防止 caller
读取一个 range 却发布另一个 range 的 location，但不会替代 session 的 same-source ownership
检查。

field read 继续按 logical position 推进。成功读取后，VM 从 execution mapping 获取 field
location；location 可以包含一个或多个 source span，原来的 direct single-span 限制会被移除。
diagnostic 使用相同的 logical-to-source resolution，因此只包含可用 forwarded span，永远不
包含 excluded gap。

little-endian field 仍先由 reader 按 logical MSB-first 顺序组合数值，再反转完整 logical byte
的权重。声明宽度和 structure-relative start 与之前一样必须按字节对齐；执行时 field logical
start 和第一个 resolved source bit 也必须按字节对齐。后续 mapping-segment boundary 不要求
source-byte alignment：需要反转数值权重的单位由 logical byte 而非 source segmentation
决定。source gap 本身不会使字段非法，也不会改变其 location。

mapped read 不增加 bytecode opcode、analysis node、source mutation 或 decoded-view
allocation。固定宽度 read 仍是一条 VM instruction 和一个取消边界。Exp-Golomb read 保持
现有 127-bit 内部上限和取消行为。mapped-view depth limit 仍为嵌套 transformation execution
保留；在一个已经解析成绝对坐标的 mapping 上构造 reader，不额外消耗 view frame。

本切片暂不接受 format-language 草案中的 `view`、`forward`、`skip` 或 `slice` 语法，也不
定义 H.264 中哪些 byte sequence 会被排除，或 excluded span 如何出现在 analysis tree 中。
这些 codec 和 presentation 规则属于下一项单独记录的 M4 切片。

## 影响

syntax field 可以直接从跨 source gap 的 logical view 解码，同时保留一个 logical range 和
精确的不连续 source span。raw media 仍是唯一 byte store，所有既有 selection 与 diagnostic
路径继续把 `FieldLocation` 作为坐标权威。

executor 会在创建 structure node 或消费 source data 前拒绝 reader/mapping 不匹配。mapped
read 即使在后一 span 遇到失败也保持事务性，保留与 direct read 相同的 partial-result 边界。

backing representation 仍可能需要与 mapping segment 数量成比例的存储。因此
transformation builder 必须合并相邻 forwarded span，并实施自己的 input、segment 和 work
limit。本 ADR 不允许 unbounded transformation 立即扫描或索引完整的大型 source。

本切片不弃用任何已接受的 0.1 语法或 direct-read 行为。direct mapping 是同一 execution
contract 的单 span 情况。

## 考虑过的方案

- 把每个 logical view 复制到 byte buffer：reader 简单，但会复制大型输入、掩盖 source
  failure，并需要第二套坐标权威。
- 新增 H.264 专用 reader：初期改动小，但会把 codec escape rule 放进 core read API，也无法
  服务后续 container 或 codec mapped view。
- 保持 reader direct-only，由 VM 拼接字段：会在 rules runtime 重复 read、seek、error 和
  transaction semantics，而不是把这些行为留在 bounded core reader 中。

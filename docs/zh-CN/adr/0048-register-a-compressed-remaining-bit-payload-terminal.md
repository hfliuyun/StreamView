# 注册消费剩余 bit 的压缩载荷终结项

Status: Accepted
Date: 2026-08-04

## 背景

H.264 slice-header 切片在导入有效 PPS 与 SPS 值后，可以描述有界 header 语法。其后的
`slice_data()` 使用 CAVLC 或 CABAC 熵编码。解码它明确不在 v0.1 范围内，但直接省略会让
已派发 RBSP view 只消费一部分，违反 ADR-0037 的精确消费契约。未来 AAC 的
`raw_data_block()` 具有同样的边界：规则必须标识压缩范围，但不能假装已经解码其中的
Huffman payload。

既有 lazy byte region 无法表达该边界。它要求 byte count 与 byte-aligned start，而 H.264
slice data 从当前 bit position 开始并延伸到有界 logical view 末尾。analyzer 不得根据字段名
或格式知识推断这个范围，因此声明、node kind 与终结消费语义都属于 DSL。

## 决策

DSL 新增一个有名称的终结结构项：

```cpp
compressed_payload slice_data
    @description("熵编码 slice data。")
    @spec("ITU-T H.264", "7.3.2.10");
```

`compressed_payload` 是上下文标识符。item 可以在名称之后带 `@description` 与 `@spec`，
不接受其他 annotation、数组后缀或前置 annotation。它的名称进入结构统一字段命名空间。

该 item 在一个结构中至多出现一次，只能无条件位于结构顶层并作为最后一项。它不能位于
conditional、switch 或 repeat body 中，并且与 `rbsp_trailing_bits` 互斥；两者都会声明当前
view 的剩余部分，不能组合。只包含一个 compressed payload 的结构不为空。

typed IR 将它表示为声明顺序中的一个 `CompressedPayload` field。它没有 scalar value、
expression、condition、array、constraint、enum 或 context eligibility，因此不能作为
controller、expression dependency、context key、import key 或 exported value。它生成一条
`register-compressed-payload` instruction，消耗一条 instruction 与一个 analysis-node slot，
并形成一个 cancellation point。

VM 在执行读取 source 前校验 field kind、metadata、终结位置、opcode，以及不存在任何只属于
scalar 的属性。malformed typed IR 是 invalid definition，且不读取 source。

选中该 instruction 时，它取得当前有界 reader 的全部剩余 bit，通过执行
`SourceMapping` 解析 logical range 并保留所有 mapped source span，追加一个状态为
`Materialized` 的 `CompressedPayload` analysis node，然后在不读取或复制 payload data 的
情况下 seek 到 reader 末尾。range 可以不按 byte 对齐、跨越 excluded source span，也可以
为空。空声明仍会发布带有效空 logical range 的 node，从而保留规则显式写出的终结项。
mapping 失败是 invalid definition；node budget 耗尽是 resource limit。两种失败都发生在追加
node 或推进 reader 之前。

`Materialized` 表示承诺的 opaque representation 已经完整，并不表示压缩语法已经解码；
该 node 既不是 lazy，也不是 unsupported。instruction 推进到 reader exclusive end 后，派发
view 的精确消费自然成立。

本切片只新增语言 primitive。内置 H.264 规则会在后续 slice-header structure 与 dispatch
变更中采用它，因此这一步不改变 package version。该语法是有界且向后兼容的 v0.1 增量，
language compatibility string 仍为 `0.1`。

## 后果

规则可以在 bit 精度上明确保留压缩语法，而不把 H.264、AAC、CABAC、CAVLC 或 Huffman
知识移入 analyzer。node 保留完整 logical/source coordinates，即使 payload byte 从未读取，
也仍可被选择。

该终结项会刻意消费全部剩余 bit。若某格式的压缩范围有显式长度，或其后仍有普通语法，
就需要另一种有界声明；不能把本 item 当成结构中间未经检查的 skip。

## 考虑过的方案

- 复用 `@lazy(...) bytes`：它要求已知 byte count 与 byte alignment，而且会错误承诺后续
  expansion，而不是已经完成的 opaque representation。
- 让 analyzer 识别 `slice_data` 或 `raw_data_block`：这会把正式格式语义放进 C++，并让
  规则字段改名意外改变 runtime 行为。
- 保留 suffix 不消费：派发结构会无法通过精确消费，或迫使 runner 弱化该一致性保证。
- 将 node 标为 `Unsupported`：规则已经完整表达所需 opaque boundary；unsupported 应保留给
  active rule 无法提供的请求解码。
- 要求至少剩余一个 bit：显式空 terminal 具有确定性，也能保留声明，无需发明特殊缺席规则。

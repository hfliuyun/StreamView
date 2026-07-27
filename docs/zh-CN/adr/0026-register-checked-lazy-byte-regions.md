# 注册经过检查的惰性字节区域

状态：已接受
日期：2026-07-28

## 背景

ADR-0013 要求规则声明安全的 lazy boundary；analysis model 也已经用 `Lazy` 表示“边界已知、
内容尚未请求”，并把它与 unsupported、invalid、cancelled 和 materialized node 区分开。
但当前可执行 DSL 还没有任何声明会创建这种 node。规则只能立即消费全部声明字段，
或者把 payload byte 完全留在规则之外。

当前 VM 刻意保持线性。它可以计算有界 scalar expression 并保留 source mapping，但尚不支持
nested structure call、runtime call stack、lazy decode recipe 或 progressive index persistence。
首个 lazy 切片必须先建立有用且经过检查的 boundary，不能把这些后续功能一起引入，也不能
为了跳过大型 payload 而读取它。

## 决策

最小 DSL 新增专用的 lazy byte region item：

```cpp
struct Packet {
    bits<16> payload_size;

    @lazy(payload_size)
    bytes payload @description("Deferred packet payload");
}
```

`@lazy(byte_count_expression)` 必须紧邻 `bytes name` 之前；`@description` 与 `@spec` 可以写在
name 后、分号前。当前不接受脱离 `@lazy` 的 `bytes`、写在 `@lazy` 之前的普通 annotation、
数组后缀、`@equals` 或 `@enum`。byte count 描述当前 execution view 中的 logical byte，
不是绝对 source byte。

在 struct item 的开头位置，token sequence `@lazy(` 是保留 introducer，会先于普通 annotation
list 被识别。它不是携带 annotation-value argument 的普通 annotation；这一区分允许参数使用
完整的 bounded expression grammar。

byte-count expression 使用已经接受的有界 expression grammar，类型必须为 `u64`。它只能引用
此前声明、且在到达该声明的每条路径上都保证存在的 scalar unsigned syntax field 或 computed
field。pure call 按既有 expression depth、node 和 work limit 在编译期展开。lazy region 不是
scalar value，不能作为后续 expression dependency 或 controller；其名称仍参与整个结构的
field name 唯一性检查。

lazy region 可以出现在 conditional、switch 和 bounded-repeat body 中，继承外层 presence
guard 与 repeat index。每个投影后的 region 都计入既有 99,999-item structure limit。被选中的
region 消耗一条 VM instruction 和一个 analysis-node 名额；guard 为 false 时 instruction 仍
计费，但不会计算 byte count、消费输入或创建 node。

compiler 只接受 structure-relative 起点在静态上已知为 byte-aligned 的 lazy region。注册一个
runtime-sized region 后，后续的精确静态 offset 变为未知。因此，在未来 alignment analysis
能够给出证明之前，既有保守规则会继续拒绝之后的 little-endian field 或 lazy byte region。
本切片不放宽 branch 或 repeat 的 alignment check。

typed IR 把 lazy region 表示为 declaration-order、kind 为 `LazyBytes` 的 field。执行前必须验证
其 `u64` typed expression 与 presence guard。它生成一条 `register-lazy-bytes` instruction，
不增加 read、call、jump 或单独的 lazy-expression opcode。

instruction 被选中时，VM 按以下顺序执行：

1. 使用 checked `u64` arithmetic 计算 byte-count expression。
2. 如果乘以八会溢出 bit-coordinate domain，则拒绝。
3. 验证绝对 logical start byte-aligned，并确认完整 bit range 位于 reader 剩余的 enclosing
   range 内。
4. 通过 execution `SourceMapping` 解析 logical range，保留所有 forwarded source span，并
   排除每一个 source-coordinate gap。
5. 检查 node budget，追加一个以声明命名的 `Region`。
6. 不读取 payload data，直接把 reader seek 到经过检查的 exclusive end。

正长度 region 的初始状态为 `MaterializationState::Lazy`。零长度 region 没有需要延后的内容，
因此直接创建为 `Materialized`，并携带合法的空 logical range 与空 source-span 集合。
包含它的 structure 可以在正长度 child 仍为 lazy 时进入 materialized 状态；只有该 child
被解析或终态化后，整棵 analysis tree 才可能 fully materialized。

checked-expression failure 或 byte-to-bit overflow 在 lazy field path 上报告 `invalid-syntax`。
声明范围超过 enclosing reader 的剩余部分时报告 `truncated-source`，并在非空时携带可用的
mapped prefix。execution start 未对齐、缺失或不是 `u64` 的 typed expression、非法 field
reference、opcode/type 不匹配或 mapping resolution 失败，均属于 invalid typed definition。
超过 node limit 时报告 `resource-limit`。cancellation 在 instruction boundary 观察。所有失败
都发生在追加 lazy node 或推进 reader 之前，并保留更早已经完成的 field。

`register-lazy-bytes` 不执行 source read；即使注册范围跨多个 mapping span，或者 source 一旦
读取 payload 就会失败，这一点也保持可观察。生成 node 的 location 是 tree-to-raw selection
与后续工作的唯一 coordinate authority。既有 raw-bit reverse lookup 继续忽略非 materialized
node，因此正长度 lazy region 在 materialized 前不会被 raw bit 反选命中。

本切片只注册安全 boundary 与持久的 lazy node。typed nested-structure materialization、decode
recipe、用户触发的 expansion、已发布 GUI subtree 的 child 增量插入，以及可恢复 progressive
index 仍是独立决策。后续功能会消费已经检查的 node location，而不会重新猜测边界。

## 影响

规则可以在不读取或分配大型 payload 的情况下让它在 tree 中保持可见，并允许从 tree 选择。
该操作仍然确定、保留 source mapping、受 enclosing reader 限制、计入既有 sandbox budget，
并兼容 conditional 与 repeat projection。

初始语法只表示未解释的 logical byte，不创建通用 eager byte-array value，不接受任意 type
annotation，也不承诺 nested parse target。后续 on-demand parsing 可以增加 typed target，同时
保留该 boundary 与 source coordinate。

保守的静态 alignment 可能拒绝更强 solver 能够证明安全的声明。显式保留这个限制，好过在
runtime-sized region 之后猜测 alignment。

## 考虑过的方案

- 把 `@lazy` 当作普通 field annotation：现有 annotation grammar 不能携带完整 expression，
  unknown annotation 也会在 typed lowering 中被丢弃。
- 立即增加 lazy nested-structure call：以后会有价值，但会把 structure composition、call-depth
  semantics、decode recipes、tree insertion 和 cancellation/resume policy 合并到一个切片。
- 注册时复制或探测 payload：这会破坏 lazy work，使初始成本可能随 source size 线性增长。
- 只保存绝对 source span：这会丢失 logical coordinate，对跨 mapped view excluded byte 的
  region 也是错误的。
- 拒绝空 region：零是合法且经过检查的 boundary；直接 materialize 可以避免留下没有内容、
  却永久 pending 的 node。

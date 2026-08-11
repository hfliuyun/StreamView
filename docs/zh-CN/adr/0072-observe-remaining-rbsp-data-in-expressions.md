# 在表达式中观察剩余 RBSP 数据

状态：已接受
日期：2026-08-11

## 背景

即使引用的序列参数集使用 High profile，H.264 picture parameter set extension 仍是可选的。
因此 clause 7.3.2.2 用 `more_rbsp_data()` 守卫 extension；只看 profile 无法区分合法的
base-only High-profile PPS 与实际携带 extension syntax 的 PPS。

DSL 已有终结项 `rbsp_trailing_bits;`，但它只能作为唯一、无条件、顶层的最后一项出现。
语言没有表达式能够观察当前 RBSP cursor 是否已经到达该终结模式。加入 remaining-bit count
或通用 source lookahead 会暴露超出本次格式决策所需的 source state，并扩大语言控制面。

## 决策

新增保留的零参数表达式 leaf `more_rbsp_data()`。它返回 `bool`，只允许用于 structure
执行期表达式，包括 computed-field initializer 与 assertion condition。pure function body
会拒绝它，因为结果依赖当前 reader cursor；pure function declaration 也不能使用该保留名。

求值不推进当前 reader。没有剩余 logical bit 时返回 false。剩余 bit 多于八个时返回 true，
因为 H.264 `rbsp_trailing_bits()` 只包含一个 stop bit 与最多七个 alignment zero bit。剩余
一至八个 bit 时，VM 探测 reader 的副本；只有完整 remainder 恰好是 `1` 后全为零时返回
false，其他模式都返回 true。探测读取失败时保留原 cursor，并沿用现有 truncated-source 或
source-error 状态传播。

parser 在执行前检查零参数与 Boolean 类型。compiler 把 leaf 降低为零 operand 的
`MoreRbspData` typed-expression node。VM 校验 descriptor，并在外层 expression instruction
内部求值。本能力不新增 opcode；除 rule 显式声明的 computed field 外，也不新增 presentation
node。

## 影响

规则可以区分可选 RBSP syntax 与 `rbsp_trailing_bits`，无需猜测 profile、执行无界扫描或做
消费式 peek。探测最多只读取一次、至多八个 bit，并继续受外层 expression 已有 instruction、
node、depth 与 cancellation 合同约束。

High-profile PPS extension 是第一个消费者，但仍作为独立的规则增量交付，拥有自己的 accepted
syntax boundary 与 fixture。

## 非目标

本决策不新增 EOF predicate、remaining-bit count、任意 lookahead、通用 alignment expression、
依赖 source state 的 pure function 或 repeat termination form；也不移动、复制
`rbsp_trailing_bits;` 终结项，或让它变成 conditional。

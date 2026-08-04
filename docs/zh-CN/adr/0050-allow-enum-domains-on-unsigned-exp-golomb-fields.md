# 允许 unsigned Exp-Golomb 字段使用 enum 值域

状态：已接受
日期：2026-08-04

## 背景

有界 H.264 IDR slice header 当前接受 `slice_type == 2`，但 H.264 还定义了等价的
all-I 值 7。等值约束无法表达闭集 `{2, 7}`；借助算术失败编码该集合会隐藏 rule 意图，
并产生误导性的 diagnostic。

DSL 已经通过 `@enum(Type)` 为定宽 `bits` 字段提供命名闭值域。unsigned Exp-Golomb
字段解码到相同的 `u64` value model，拥有完整 source location，也能作为 condition、switch、
repeat 与 sentinel controller。唯一新增边界是受支持的 `ue` domain 截止于 `2^64 - 2`；
`2^64 - 1` 没有可表示的 Exp-Golomb 码字。

## 决策

允许 `ue` 字段带一个 `@enum(Type)` annotation。enum 必须已经声明，按照既有 enum 规则
不得为空，且每个 member 都必须位于 `0..2^64 - 2`。值为 `2^64 - 1` 的 member 是静态
`enum-value-out-of-range` definition error。`se @enum(Type)` 仍然非法。

typed IR 保持字段的 `UnsignedExpGolomb` kind，并同时保存 enum index。这有意区别于 kind
保持为 `Enum` 的定宽 enum。bytecode 因而继续使用 `ReadUnsignedExpGolomb`，不增加 opcode
或 encoding。两种表示的 metadata 都使用 enum type name。

VM 在读取 source 前验证 enum reference 及所有 member value，然后解码并物化完整的
Exp-Golomb 码字。若值不属于声明的 enum，则在该字段完整 mapped codeword location 上产生
致命 `invalid-syntax`，与定宽 enum 行为一致。字段 node 与解码值作为 partial analysis 保留，
但结构内后续字段不再执行。

一个 `ue` 字段可以同时带 `@enum`、`@equals` 与 `@range`。它们以确定的 lowering 顺序独立
检查同一个已物化值：先 enum membership，再 equality，最后检查 range 两端。enum 与 equality
违规是致命错误；range 违规继续采用 ADR-0040 的非致命 warning 语义。前面的致命检查会阻止
后续 instruction 执行。附加 enum 不改变 controller 资格，condition、switch、bounded repeat
或 sentinel 继续使用其解码后的 `u64`。

这是 DSL `0.1` 的加法兼容变更；既有 source 与 typed program 行为保持不变。

## 影响

rule 可以直接声明变长无符号语法的闭值域，并获得可读名称与精确 diagnostic。H.264 rule
可以同时接受两个 all-I slice type，而不放宽对 P、B、SP 或 SI slice header 的拒绝。

malformed typed program 无法在读取 source 前绕过越界 enum index、空 enum、`2^64 - 1`、
signed Exp-Golomb enum 或 opcode/type 不匹配检查。

## 非目标

本决策不把 enum 扩展到 `se`、computed field、lazy region 或 compressed payload；不增加
enum-member expression、bit flag、runtime enum alias、通用 set constraint，或致命 enum
membership 失败后的恢复；也不在本身扩展其余 H.264 slice-header branch。

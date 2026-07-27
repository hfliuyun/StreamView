# 将纯标量函数内联到 computed field

状态：已接受
日期：2026-07-27

## 背景

格式作者需要派生 flag 和有界算术值，同时不能消费 source bit，也不能让派生值伪装成
具有 source location 的字段。已接受的语言已有按声明顺序排列的值、带 guard 的字段投影、
有界 repeat 和线性 VM，但没有通用表达式模型、运行时调用栈或可变状态。引入不受限制的
函数会显著扩大语言与 sandbox。

## 决策

首个 computed-value 切片接受顶层、表达式 body 的纯标量函数，以及结构内的 computed
field：

```cpp
pure bool between(u64 value, u64 low, u64 high) {
    return value >= low && value <= high;
}

struct NalUnitHeader {
    bits<5> nal_unit_type;
    computed<bool> is_vcl = between(nal_unit_type, 1, 5);
}
```

公开的标量类型为 `bool` 和 `u64`。纯函数具有带类型的返回值、至多 16 个名称互异的
带类型参数，以及唯一一条 `return` 表达式。函数只能引用参数和此前声明的纯函数。函数名
与顶层声明共用命名空间；不接受 overload、前向调用或 annotation，声明顺序也使直接或
间接递归无法成立。每个函数 body 都会执行类型检查，而且即使函数未被使用，也必须独立
满足表达式大小与深度上限。

computed field 具有声明的标量类型和一条表达式。表达式只能引用此前的 scalar 无符号
`bits`、enum、`ue` 或 computed field，并且这些值必须在到达声明的每条路径上都保证已
物化。数组、`se`、未来或未知声明，以及当前路径不可用的 branch-local 值都会被拒绝。
computed 与 syntax field 的名称在整个结构中仍必须唯一。computed field 可以携带
`@description` 和 `@spec` metadata，但不接受 `@equals`、`@enum` 或数组后缀。

表达式接受无符号整数与 Boolean 字面量、identifier、纯函数调用、括号、unary `!`、
带检查的 `*`、`/`、`%`、`+` 和 `-`、同类型 `==` 与 `!=`、无符号 `<`、`<=`、`>` 与
`>=`，以及短路 Boolean `&&` 和 `||`，并使用常规 C precedence。不进行隐式转换：算术
和顺序比较使用 `u64`，逻辑运算使用 `bool`，等值运算两侧类型必须相同。无符号 overflow、
underflow、除零和模零会在运行时于 computed field path 报告 `invalid-syntax`。source enum
在表达式中提供解码后的无符号 `u64` 值；本切片不把 enum member 名作为表达式值。

compiler 把纯函数调用展开到每条 computed 表达式中。展开后的表达式最多包含 256 个节点，
深度最多为 64。纯函数因此不会引入 runtime call opcode、递归、动态派发，也不能访问 source、
analysis tree、host、时间、随机值或可变状态。固定上限让一次 computed 求值与单条内部有界的
Exp-Golomb read 一样保持有界。

computed 声明加入现有按声明顺序排列的 typed-field 流。它们继承外层条件、switch 和 repeat
guard，计入单结构 99,999 字段投影上限，并在物化名称上追加 repeat index。repeat-local
computed 值只对同一次迭代内的后续 item 可见。它不消费 source bit，也不改变静态对齐。

controller 验证会扩展，使此前的 `computed<u64>` 可以作为现有等值条件、switch 或有界
repeat 的 controller；条件与 switch 字面量使用完整 `u64` 范围。此前的 `computed<bool>`
可以作为新的受限 `if (flag) { ... } else { ... }` controller；与既有条件形式相同，`else`
仍可省略。该形式降低为与 `true` 的等值判断，但 bool 不能控制 switch 或 repeat。computed
controller 不改变 repeat 的字面量 maximum 契约；正常的带 guard repeat assertion 仍会在
消费 body 输入前拒绝执行路径抵达且
超过 maximum 的值。该失败附加到 computed controller field path，且没有 `FieldLocation`；
source-backed controller 继续按 ADR-0022 保留精确 source location。

每个 computed field 生成一条 `evaluate-computed` instruction。它的 typed expression 只能
引用此前 typed-field index，并在执行前接受验证。presence guard 为 false 时跳过求值和节点
创建，但该 instruction 仍计入 instruction budget 并保留取消检查点。求值成功时创建一个
`AnalysisNodeKind::ComputedField`，保存 `bool` 或无符号 64 位值与 metadata，不携带
`FieldLocation`，并消耗一个物化节点名额。求值失败时不创建 computed node，保留此前节点，
把结构标记为 invalid，并在 computed field path 上附加无 location 诊断。非法 expression
index、类型、依赖、guard 或 opcode 位置属于 invalid typed definition。subexpression 求值
作为这一条 instruction 的内部工作计费，不增加 instruction 边界之外的取消检查点；256 节点
与 64 深度上限约束了这部分内部工作。

malformed `pure`、parameter、`return`、call、expression 或 `computed<...>` 语法会产生带
source range 的诊断。statement recovery 停在下一个分号或外层右花括号；expression recovery
还会识别当前 call 的逗号或右括号。本切片被视为稳定前，format-language reference 会完整
列出合法与非法形式。

## 影响

格式定义可以表达常见派生 flag 和计数调整，同时保留 source traceability、线性 bytecode、
selected-only 物化，以及现有 partial-result 和取消模型。纯函数通过静态内联、而不是运行时
调用栈提供复用。

即使特定运行路径可能短路，大型 helper 展开仍会被拒绝。computed field 虽然不消费 source
bit，仍占用静态投影、instruction 和节点容量。运行时算术诊断具有 field path，但没有 source
location，因为为其分配位置会违反 computed-field 模型。

本切片不接受有符号标量表达式、引用 `se`、cast、bitwise 或 shift operator、ternary、
computed array、局部变量、单条 return 表达式以外的函数 statement、递归、前向调用、
runtime function value、source-location introspection 或任意 native/host helper。直接出现在
array length、case label、repeat maximum 和 repeat count 中的通用表达式仍是独立决策；
computed `u64` identifier 是本切片连接既有控制形式的唯一新方式。

本切片不弃用任何已接受的 0.1 语法。既有 syntax-field read、数组、等值条件、switch 和
有界 repeat 的行为保持不变；唯一例外是此前 computed scalar identifier 只在上文明确列出的
形式中成为合法 controller。

## 考虑过的方案

- 带 runtime call 的通用 expression VM：扩展性强，但会在格式语言真正需要之前引入 call
  frame、递归验证、更多取消核算和更大的 malformed-bytecode surface。
- 只提供 built-in helper：实现较小，但普通的格式专用 predicate 会依赖不断扩大的 engine
  catalog。
- 只提供 computed field、不提供可复用函数：足以表达一次性派生值，但会复制常见规范
  predicate，也无法完成计划中的 pure-helper 功能。

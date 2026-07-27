# 将等值 switch 降低为 guard 合取

状态：已接受
日期：2026-07-27

## 背景

等值条件已经把结构化分支降低为携带 presence guard、按声明顺序排列的线性 typed field。
下一项 DSL 功能需要提供多路选择，同时不能引入 jump bytecode 或表达式求值器，并且必须保持
现有 field index、验证和预算不变量。

## 决策

首个 switch 切片接受可嵌套的 C 风格
`switch (previous_field) { case integer: { ... } default: { ... } }` block。
controller 与等值条件遵守相同限制：它必须是此前声明的 scalar `bits` 或 enum 字段，并且在
到达 switch 的每条路径上都保证存在。switch 至少包含一个 `case`；每个 case 只接受一个不
重复、能适配 controller 宽度的无符号整数字面量。`default` 可以省略，最多出现一次，并且
必须是最后一个 arm。首个切片不支持 fallthrough、`break`、同一 body 的多个 label、范围、
enum member 名和一般表达式。

每个 case body 降低为由对应等值比较守卫的字段。default body 降低为携带全部 case 等值
比较之否定合取的字段。已有外层 guard 继续保留，因此嵌套 switch 和等值条件无需新 opcode
即可组合。所有 arm 都执行静态验证，字段名在完整结构中仍必须唯一，所有可能 arm field 都
计入结构展开上限。

静态对齐从同一个入口 offset 开始，分别分析每个 arm。只有所有路径都在相同的已知 offset
结束时，switch 出口才保留已知 offset；省略 `default` 时，未匹配的空路径也参与合并。

## 后果

switch 继续使用确定性的线性 bytecode，并复用 VM 仅对选中字段读取 source 和创建节点的
语义。跳过 arm 的 instruction 仍计入 instruction budget，跳过字段不消耗 source bit 或
node budget。case 较多时，default 字段会携带每个 case 对应的否定 guard，typed IR 因此
增大，但语义保持显式并且可以局部验证。

同一 body 使用多个 label 暂缓支持，因为当前字段 guard 列表表示合取，无法在不新增条件
形式或控制流 bytecode 的情况下表达所需的析取。

## 考虑过的方案

- jump table 或 branch opcode：大型 switch 更紧凑，但会在表达式语言和通用 statement
  runtime 成形前扩大 bytecode 控制流验证面。
- 把 switch 改写成嵌套 `if`/`else`：case arm 可以表达，但多 case 的 default 仍需要多个
  否定比较的合取，而且 source AST 会失去 switch 专属的重复 case/default 诊断。
- 同一 body 支持多个 case label：更接近 C，但需要 OR guard；暂缓可以避免把 label 错误地
  解释成 AND 语义。

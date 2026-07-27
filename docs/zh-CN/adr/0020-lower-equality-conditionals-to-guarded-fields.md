# 将等值条件降低为 guarded field

状态：已接受
日期：2026-07-27

## 背景

按声明顺序执行的 VM 具有确定的 field index，尚未引入通用控制流验证。首个条件功能需要在
不削弱这些不变量、也不提前定义完整表达式语言的前提下提供分支语义。

## 决策

首个条件切片接受可嵌套的 C 风格
`if (previous_field == integer) { ... } else { ... }` block，`else` 可以省略。
控制字段必须解析到此前声明的 scalar `bits` 或 enum 字段，并且该字段在到达条件的每条路径上
都必定已经物化。数组、`ue`/`se`、一般表达式，以及当前路径不能保证存在的 branch-local 值
都会被拒绝。字段名在整个结构的全部分支中仍必须唯一，两个分支也都会执行静态检查。

compiler 把条件 block 降低为按声明顺序排列、携带已解析 presence guard 的 typed field，
不新增通用 jump bytecode。VM 在读取字段前验证并计算 guard：不存在的字段不消耗 source bit、
不创建 analysis node，也不执行 enum 或 equality 数值检查；它的 read 和可选 assertion
instruction 仍计入 instruction budget，并保留取消检查点。这样可以继续维持现有单调 field
index 和 malformed-bytecode 检查，同时保证未选择分支不能访问输入。

静态对齐按分支分别分析。只有 then 和 else 路径都以相同的已知 offset 结束时，条件出口才
保留已知 offset；省略 `else` 时把它视为空路径。所有可能出现的 branch field 都计入结构
展开上限，只有实际选择的字段消耗物化节点预算。

## 后果

规则可以表达嵌套的等值选择布局，同时 bytecode 仍保持线性和确定性。所有可能字段都会增加
typed IR 大小和静态字段计数，即使一次执行只选择很小的分支。跳过的 read 和 assertion
instruction 仍消耗 instruction budget，因此大型 alternative 不能绕过取消或工作量记账。

## 考虑过的方案

- 通用 jump opcode：跳过大 block 更紧凑，但会破坏当前单遍 field-index 不变量，并在表达式
  语言成形前显著扩大控制流验证面。
- 字段级 `@if` 注解：parser 改动较小，但不是 C 风格 block 语法，也难以自然表达嵌套分支、
  数组和未来 statement。
- 先实现完整 expression VM：以后有价值，但会无谓扩大首个条件切片的类型、溢出和 sandbox
  契约。

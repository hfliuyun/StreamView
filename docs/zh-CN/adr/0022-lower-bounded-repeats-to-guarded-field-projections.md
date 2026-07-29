# 将有界 repeat 降低为带 guard 的字段投影

状态：已接受
日期：2026-07-27

## 背景

固定长度数组覆盖计数在编译期已知的重复标量声明。媒体语法还包含由前置字段给出
次数的多字段重复条目。当前按声明顺序执行的 VM 没有回边、通用表达式求值器或可变
循环索引；字段索引与工作量核算仍须保持静态有界，并且可以局部验证。

## 决策

首个有界循环切片接受可嵌套的 `repeat (count_field, maximum) { ... }`。
`count_field` 必须解析到此前且在抵达 repeat 的每条路径上都保证已物化的无符号
标量 `bits`、enum 或 `ue` 字段；数组与有符号 `se` 字段被拒绝。`maximum` 是正的
无符号整数字面量；使用定宽 controller 时它必须能被该字段表示，并且它静态约束
完整 body 投影。repeat body 必须至少包含一个字段，并可包含字段、等值条件、等值
switch 和嵌套 repeat。

[ADR-0023](0023-inline-pure-scalar-functions-into-computed-fields.md) 后续允许此前且路径上保证存在的
`computed<u64>` controller 和 computed body item；
[ADR-0026](0026-register-checked-lazy-byte-regions.md) 后续允许 lazy byte region 作为 body item。
现行规范规则见[英文 format-language 规范](../../format-language/README.md)，中文伴随说明见
[格式语言说明](../format-language/README.md)。

编译器把 body 按 `maximum` 次投影到现有线性 typed-field 流。第 `i` 次迭代中的
字段除继承外层 guard 外，还带有正向 `count_field > i` presence guard。物化名称先
追加各层 repeat 索引，再追加可选的固定数组索引：标量命名为 `value[i]`，数组元素
命名为 `value[i][j]`，嵌套 repeat 继续按同一顺序追加后缀。源字段声明名在整个结构
中仍保持唯一。body 声明可被同一次迭代内的后续 item 引用，但 repeat 局部声明不能
被其他迭代或 repeat 之后的语句引用。

每个投影后的 repeat 还在语句位置生成一条带外层 guard 的边界断言。执行路径抵达
repeat 且解码计数超过 `maximum` 时，在消费 body 输入之前于计数字段报告
`invalid-syntax`；运行时绝不截断计数。该断言以及所有投影出的 read 或等值断言都
计入指令预算并保留取消检查点，包括缺席迭代的指令。只有 guard 成功的迭代字段才
消费源 bit 或物化节点预算；所有投影字段都计入现有的单结构 99,999 字段上限。

这种 lowering 新增 greater-than presence 比较和 repeat 边界断言，但不新增 jump、
回边或可变循环状态。typed repeat 元数据、controller、外层 guard 与 bytecode 位置
和既有 typed field/断言元数据一样，在执行前或执行期间接受验证。无符号 Exp-Golomb
值会保留下来供 repeat 作为 controller 使用，但不会因此成为等值条件或 switch 的
合法 controller。

编译器在投影每个可能迭代时继续检查静态 offset，因此 body 内的固定对齐错误仍可
诊断。运行时计数可选择零到 `maximum` 次迭代，所以 repeat 之后的 offset 静态未知；
在未来的常量或表达式分析能够证明对齐之前，repeat 后的小端字段会被拒绝。

## 影响

规则可以表达有界的计数前缀条目列表，同时保留线性 bytecode、确定性字段索引、只对
选中字段读取源数据，以及现有取消和部分结果模型。typed program 的大小随声明的
maximum 而非解码计数增长，因此保守的大上限即使面对短输入，也会占用静态字段与
指令容量。

本切片不暴露循环索引，也不接受 `count_minus_one + 1` 一类算术计数、sentinel/EOF
终止、`break`、可变状态、结构组合或分页/lazy 列表物化。这些形式分别需要 computed
field、通用表达式、语句控制流或 lazy runtime 工作，继续作为独立语言决策处理。

## 考虑过的方案

- 仅支持字面量 `repeat (3)`：很容易展开，但与已接受的固定数组切片大幅重叠，也
  无法表达媒体语法中的计数前缀结构。
- 新增运行时 loop/jump opcode：对大上限更紧凑，但会在通用语句 runtime 出现之前
  引入 program counter、回边验证、可变迭代状态与新的取消核算。
- 用 `count != 0 && ... && count != i` 表示 `count > i`：现有等值 guard 可以表达，
  但会产生二次方级别的 guard 存储与求值，而这些工作目前不计入指令预算。

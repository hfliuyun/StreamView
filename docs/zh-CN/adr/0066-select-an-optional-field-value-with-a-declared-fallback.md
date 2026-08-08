# 以声明的回退值选择可选字段值

Status: Accepted
Date: 2026-08-08

## 背景

ADR-0065 放开了 computed initializer 中的保留 external leaf，并记录了
`pred_weight_table()` 仍然无法落地。剩下的阻塞点是 count 选择。

clause 7.3.3.2 迭代 `num_ref_idx_l0_active_minus1 + 1` 次。clause 7.4.3 规定该值在
`num_ref_idx_active_override_flag` 置位时取 slice 级 override，否则取 PPS 默认值
`num_ref_idx_l0_default_active_minus1`。于是 override 字段声明在
`if (num_ref_idx_active_override_flag == 1)` 内部，而后者又嵌在
`if (uses_reference_lists)` 内部；默认值则通过已声明的 `h264-pps` import 抵达。

一次探测把阻塞点归约到最小形态：

```
bits<1> override_flag;
if (override_flag == 1) {
    bits<4> override_count_minus1;
}
computed<u64> effective_count = override_count_minus1 + 1;
```

编译器以「Computed field dependency is not guaranteed on the current branch」拒绝该计算
字段。这条规则本身是正确且承重的：普通 field reference 绝不能读取当前路径未曾物化的值。
语言缺少的是一种表达方式——「此字段在这里可能缺席，缺席时用这个值」。

复制方案同样做了探测，它可以编译通过：

```
if (override_flag == 1) {
    bits<4> override_count_minus1;
    computed<u64> effective_count_override = override_count_minus1 + 1;
    repeat (effective_count_override, 16) { bits<1> weight_flag_override; }
}
if (override_flag == 0) {
    computed<u64> effective_count_default = 3;
    repeat (effective_count_default, 16) { bits<1> weight_flag_default; }
}
```

由于扁平命名空间禁止在互斥分支间重复同名，复制体内每个字段都需要不同名字。对
`pred_weight_table()` 而言，这意味着 list 0 的表体要在 override 与默认两个分支下各出现
一次，list 1 的表体又要在 `if (is_b_slice)` 内部各出现一次，而 clause 7.3.3.2 自己的名字
——`luma_weight_l0`、`chroma_offset_l0`——在规则里根本不会出现。代价是四份近似相同的
表体，且字段名编码了一个无关 flag；后续每一张具有同样默认化形态的表都会重复这份代价。

既有 runtime 的三个事实让另一条路径变得便宜，且每一条都在源码中确认过，而非假设。

字段 presence 本来就被精确建模。解释器持有 `std::vector<std::optional<quint64>>` 字段值，
初始为空；guard 是逐字段的 condition chain 而不是 jump——guard 为假时该字段被跳过，槽位
保持 `nullopt`。已取分支的值在该 structure 执行的剩余过程中一直保留，唯二的 `.reset()`
调用属于 lazy 与 compressed region，它们不携带 scalar 值。

字段缺席目前必然是硬失败。对槽位为 `nullopt` 的 `FieldReference` 求值会报
「Computed expression dependency is unavailable」；今天这条路径不可达，恰恰因为静态规则
保证它不会发生。

repeat body 是带 scope 的。每次迭代结束都会把已声明字段列表 resize 回进入时的长度，因此
repeat body 字段在 repeat 之后已不在 scope，仍会被「must be declared earlier」拒绝。
conditional body 没有这个机制，这正是 override 字段可被命名却被拒绝的原因。

所以该限制纯属静态。runtime 已经知道字段是否被物化。

## 决策

新增第三种保留 leaf 形式 `optional_value(field_identifier, fallback_expression)`，
类型为 `u64`。

第一个实参必须是 identifier，命名同一 structure 中此前声明的字段。它豁免
branch-guarantee 规则，且**仅**豁免这一条。其余每项 dependency 限制继续适用：字段必须
更早声明、必须带 typed index、必须是 scalar 无符号 `bits`、enum、`ue` 或 `computed<u64>`，
数组与 `se` 字段一律拒绝。已离开 scope 的 repeat body 字段仍按未声明拒绝，因此逐迭代
投影出的 index 绝不会被别名化。

第二个实参是完整的 `u64` expression，受包括 branch-guarantee 在内的每一条规则约束，因此
回退值本身只能读取当前路径保证存在的值。嵌套可以组合：回退值可以是另一个
`optional_value(...)`。

该形式在所有已经求值 field-dependent expression 的位置被接受——computed field
initializer、assertion condition、dynamic `bits` width 与 lazy byte count。它在
pure-function body 中被拒绝（那里解析的是 parameter 而不是 field），在所有固定形状位置也
一律拒绝：imported equality conditional、switch 与 repeat controller、array length、
case label 与 annotation。

typed IR 新增一个 expression kind `OptionalFieldReference`，携带解析出的 field index，并以
其唯一 operand 承载 lower 后的回退值。不需要新的 struct 成员，也不需要新 opcode。runtime
在槽位有值时取字段值，无值时对回退值求值，复用上述 presence 模型。普通 `FieldReference`
指向空槽位仍是硬失败；只有这一个 kind 把缺席视为有意义。

该 leaf 计为一个 expression node 加上它的回退子树，并照旧受既有 node、depth 与共享
expansion-work 上限约束。

## 影响

`pred_weight_table()` 由此可以用 clause 7.3.3.2 自己的字段名表达：

```
computed<u64> effective_l0_count =
    optional_value(num_ref_idx_l0_active_minus1,
                   context_value(pic_parameter_set_id,
                                 h264_pps,
                                 num_ref_idx_l0_default_active_minus1)) + 1;
```

它读起来与 clause 一致，并且是语言中第一个能表达 syntax element 推断默认值的构造。后续
若干 H.264 元素共享同一形态，因此这份代价只付一次。

编译器仍然能抓住本形式所要处理的错误。对 branch-local 字段写普通引用依旧以
branch-guarantee 诊断失败，因此缺席必须被显式承认，而不能靠遗漏蒙过去。

第一个实参不要求必须是 branch-local。若强制要求，有效性就会依赖 structure 中别处无关的
guard：给某个既有字段加上一层 conditional，会追溯性地让远处一个原本正确的
`optional_value` 失效。命名一个总是存在的字段的冗余 `optional_value` 会被接受，只是永远
不会取用其回退值。

本增量不改动任何 bundled 规则。能力连同自身测试与文档一起落地，沿用 ADR-0063 与
ADR-0065 的先例；消费它的有界 `pred_weight_table()` 作为下一个增量，携带自己的 analyzer
覆盖。

## 非目标

Boolean 形式不属于本决策。两个既有保留 leaf 都是 `u64`，H.264 的需求也是 `u64`，而窄的
第一版日后放宽比收紧更容易。`optional_value` 命名 `computed<bool>` 属类型错误。

不引入通用 conditional 或 ternary expression。那条路要达到同样效果，需要 flow-sensitive
分析来针对 condition 所隐含的 guard 证明每个 arm 的 dependency，还要新增 grammar 与优先级。
本形式不需要任何证明，因为它显式声明了缺席处理方式，而且在任意嵌套深度都可用，无需 guard
的 controlling field 处于 scope 内。

presence 不作为 Boolean 暴露。不存在 `has_value(field)`；观察缺席的唯一方式就是提供替代它
的值。

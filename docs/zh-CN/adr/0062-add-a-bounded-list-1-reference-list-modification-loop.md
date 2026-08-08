# 新增有界 list 1 reference-list modification loop

状态：已接受
日期：2026-08-08

## 背景

ADR-0061 加入了有界 non-reference B-slice header，但止步于 list 1 reference-picture
modification。它读取 `ref_pic_list_modification_flag_l1` 并用 `@equals(0)` 约束，因为
list 0 loop 投射出的字段名已经占满 structure 的扁平命名空间，而非零 flag 会让之后每个
字段错位。

clause 7.3.3.1 的 `ref_pic_list_modification()` 包含两个结构完全相同的 loop。第一个在
slice 既非 I 也非 SI 时执行，第二个在 slice 为 B 时执行。两个 loop 都读取
`modification_of_pic_nums_idc`，随后按 operation code 0/1 读取
`abs_diff_pic_num_minus1`、按 operation code 2 读取 `long_term_pic_num`，并在
operation code 3 处终止。规范在两个 loop 中复用同一批 syntax element 名字，因为每个
loop 自带作用域。

rule language 没有这种作用域。一个 structure 只有一个扁平字段命名空间，而 sentinel
repeat 会把 body 投射进该命名空间，因此第二个 loop 无法复用第一个 loop 的名字。该语言
还把 sentinel repeat 的 maximum 限制在 1..64，因此迭代上界由语言决定，而不是由 profile
选择。

## 决策

用真正的 loop 取代 `@equals(0)` 约束，镜像 list 0 的形状，并用 `_l1` 后缀区分投射名：

```cpp
if (is_b_slice) {
    bits<1> ref_pic_list_modification_flag_l1;
    if (ref_pic_list_modification_flag_l1 == 1) {
        repeat (64) {
            ue modification_of_pic_nums_idc_l1 @enum(ModificationOfPicNumsIdc);
            computed<bool> uses_abs_diff_pic_num_l1 =
                modification_of_pic_nums_idc_l1 == 0 ||
                modification_of_pic_nums_idc_l1 == 1;
            if (uses_abs_diff_pic_num_l1) {
                ue abs_diff_pic_num_minus1_l1;
            }
            if (modification_of_pic_nums_idc_l1 == 2) {
                ue long_term_pic_num_l1;
            }
        } until (modification_of_pic_nums_idc_l1 == 3);
    }
}
```

`_l1` 后缀只是呈现层的消歧手段，不是另一个 syntax element。clause 7.3.3.1 对两个 loop
使用完全相同的名字，因此每个 list 1 字段都保留该 clause 引用，并在 description 中说明它
修改的是哪个 list。将来若语言引入 scope 构造，可以在不改变解码 bit 布局的前提下去掉该
后缀。

该 loop 复用既有的闭集 `ModificationOfPicNumsIdc` enum，因此保留值仍在完整 Exp-Golomb
码字处致命失败，终止值 3 也仍保留在树中。flag 为零时完全不发布 loop 字段。64 次是
sentinel repeat 的语言上界；未终止的列表会在最后一个投射 operation 处失败，并保留有界
前缀。

现在两个 loop 各自独立受界，因此一个 B slice 最多可投射 128 个 operation。list 0 loop
保持不变；P 与 all-I slice 中 list 1 依旧缺席，因为外层 `is_b_slice` guard 为 false 时
不消费任何 bit。

package 版本 `0.1.18` 声明这次的追加字段面。coverage depth 保持 `i-p-b-slice-header`，
因为本增量完成的是既有 B-slice 形状的一个可选分支，而不是新增 slice family。

回归 fixture 覆盖：flag 为零且无 loop 字段；首轮即终止；单个 list 1 loop 中出现
operation 0/1/2/3 全集；同一 slice 中 list 0 与 list 1 两个独立 loop；保留 operation
code；截断的 operation 与截断的 operand；始终不终止的列表；精确的 child 顺序与 source
span；以及后续 NAL unit 继续被扫描。

## 影响

bundled rule 现在能为其支持的每种 slice type 解码完整的 `ref_pic_list_modification()`
语法，分析者可以看到两个 reference list 的重排操作及其精确 source mapping。

代价是可见的命名偏差。读者拿树对照 clause 7.3.3.1 时，会在规范写
`modification_of_pic_nums_idc` 的位置看到 `modification_of_pic_nums_idc_l1[0]`。该后缀
已在两份语言参考以及每个字段的 description 中说明，并且它是扁平命名空间的直接后果，而
不是建模选择。

两个 list 都被大量重排的 B slice 现在会投射出比以前多得多的字段。这仍远在 structure
字段上限之内，但确实增加了该 structure 的编译后 bytecode 体积。

## 非目标

本决策不新增 `pred_weight_table()` 或 explicit weighted biprediction，不解码 CABAC 或
CAVLC `slice_data`，不新增可让两个 loop 共享 clause 名的语言 scope 构造，不提高 sentinel
repeat 上界，不新增 SP 或 SI slice type，不接受 nonzero-reference type-1 header，不解析
reference-picture marking 或 adaptive memory-management operation，不支持 field picture
或 MBAFF，不新增 POC type 1 或 2，不新增 slice group 或 partitioned data，也不改变
opaque payload 语义。它不扩展 DSL、context model、compiler、VM，也不改变 `@range`、
`@equals` 与 enum constraint 的 diagnostic severity。

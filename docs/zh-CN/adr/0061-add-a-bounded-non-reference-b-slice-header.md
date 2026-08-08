# 新增有界 non-reference B-slice header

状态：已接受
日期：2026-08-08

## 背景

ADR-0057 至 ADR-0060 完成了有界 progressive non-reference P-slice header：
reference-index override、list 0 modification loop 以及条件字段
`cabac_init_idc`。`NonIdrSliceType` 目前只接受 I 值 2 和 7 与 P 值 0 和 5，因此任何
B slice 都会在 `slice_type` codeword 处失败，后续字段一个都读不到。

clause 7.3.3 把三个 B-only syntax element 放在现有有界结构内部。
`direct_spatial_mv_pred_flag` 位于 reference-count override 之前。
`num_ref_idx_l1_active_minus1` 紧随 override 分支内的
`num_ref_idx_l0_active_minus1`。`ref_pic_list_modification()` 在 list 0 loop 之后读取
`ref_pic_list_modification_flag_l1`。P 与 B 都会读取
`num_ref_idx_active_override_flag`、`ref_pic_list_modification_flag_l0`、list 0 loop
和 `cabac_init_idc`；只有 I 与 SI 省略它们。

现有 rule language 的两项性质决定了可达边界。每个 structure 的 field name 共享一个
扁平命名空间，因此两个互斥分支不能同时声明
`num_ref_idx_active_override_flag`。`if` condition 只接受 `computed<bool>` 标识符、
field equality 或 imported `context_value` equality，所以共享分支无法内联写出
`is_p_slice || is_b_slice`。

## 决策

为 `NonIdrSliceType` 增加 `b = 1` 与 `all_b = 6`，并把现有 P-slice 分支拓宽为共享的
reference-list 分支，而不是新增平行的 B-only 副本：

```cpp
computed<bool> is_p_slice = slice_type == 0 || slice_type == 5;
computed<bool> is_b_slice = slice_type == 1 || slice_type == 6;
computed<bool> uses_reference_lists = is_p_slice || is_b_slice;

if (is_b_slice) {
    bits<1> direct_spatial_mv_pred_flag;
}
if (uses_reference_lists) {
    bits<1> num_ref_idx_active_override_flag;
    if (num_ref_idx_active_override_flag == 1) {
        ue num_ref_idx_l0_active_minus1 @range(0, 31);
        if (is_b_slice) {
            ue num_ref_idx_l1_active_minus1 @range(0, 31);
        }
    }
    bits<1> ref_pic_list_modification_flag_l0;
    if (ref_pic_list_modification_flag_l0 == 1) {
        repeat (64) { ... } until (modification_of_pic_nums_idc == 3);
    }
    if (is_b_slice) {
        bits<1> ref_pic_list_modification_flag_l1 @equals(0);
    }
}
```

共享同一个分支是必需的，不只是为了精简：独立的 B-only 分支会重复声明
`num_ref_idx_active_override_flag` 与 `ref_pic_list_modification_flag_l0`，会被判为
duplicate field name 而拒绝。`uses_reference_lists` 出于同一原因存在，因为 `if`
condition 无法内联组合两个 type predicate。

list 1 modification 由 `@equals(0)` 约束，而不是第二个 loop。list 0 loop 投射出的字段
名——`modification_of_pic_nums_idc`、`uses_abs_diff_pic_num`、
`abs_diff_pic_num_minus1` 与 `long_term_pic_num`——已经占用扁平命名空间，因此 list 1
loop 需要另一套不同名字和第二个 64 次迭代投射。该约束是 layout-critical：非零 flag
会引入本 profile 无法解析的 modification operation，之后每个字段都会错位。因此值一在
该 bit 处 fatal，并保留已解码前缀。

weighted prediction 新增第二条 source-anchored prerequisite。clause 7.3.3 在
`weighted_bipred_idc` 等于一时为 B slice 调用 `pred_weight_table()`，因此 explicit
biprediction 必须失败，而 default（0）与 implicit（2）biprediction 保持支持：

```cpp
assert(!is_b_slice ||
       context_value(pic_parameter_set_id, h264_pps, weighted_bipred_idc) != 1)
    at pic_parameter_set_id;
```

现有 P-slice 的 `weighted_pred_flag == 0` assertion 保持不变，两者都仍是无条件的
top-level item，因为该语言禁止在分支内声明 assertion，也禁止从 assertion 引用被
guard 的字段。

`cabac_init_idc` 从 `is_p_slice` guard 移到 `uses_reference_lists`，与 clause 中
“slice 既非 I 也非 SI”的条件一致。type-1 direct-header assertion 仍要求
`nal_ref_idc == 0`，因此 `dec_ref_pic_marking()` 依旧缺席，这些是 non-reference
B slice。

package 版本 `0.1.17` 声明这次的追加字段面，coverage depth 变为
`i-p-b-slice-header`。

本次增量改变 type-1 的呈现形状。structure 现在发布三个 top-level computed Boolean
而不是一个，因此 I 与 P slice 中 `slice_type` 之后的所有 child index 都后移两位。两个
新节点没有 source location，也不消耗 bit。

回归 fixture 覆盖：B 值 1 与 6；`direct_spatial_mv_pred_flag` 的两种状态；list 0 与
list 1 override count 同时出现，以及 P slice 时只有 list 0 count；B slice 复用 list 0
modification loop；非零 list 1 modification flag 在其 bit 处失败；explicit
biprediction 在 `pic_parameter_set_id` 处失败而 implicit biprediction 成功；
entropy-coded B slice 出现 `cabac_init_idc`；精确的 child 顺序与 source span；以及
后续 NAL unit 继续被扫描。

## 影响

bundled rule 覆盖了有界 non-reference progressive 流可能包含的三类 coded slice，B
slice 现在能以精确 source mapping 暴露其 direct-prediction mode、两个 active
reference count 与 CABAC initialization table。

共享一个 reference-list 分支让 P 与 B 的公共语法只有一处定义，后续增量只需扩展一条
路径而不是两条。代价是扁平命名空间限制了 list 1 modification 的落点：支持它需要为第
二个 loop 取不同的投射名。

依赖位置索引访问 slice child 的现有 type-1 测试需要后移两位。这是可见的呈现变化而非
语法变化，任何字段的 source span 与取值都不受影响。

## 非目标

本决策不新增 `pred_weight_table()` 或 explicit weighted biprediction，不解码 CABAC
或 CAVLC `slice_data`，不新增 list 1 modification loop，不新增 SP 或 SI slice type，
不接受 nonzero-reference type-1 header，不解析 reference-picture marking 或
adaptive memory-management operation，不支持 field picture 或 MBAFF，不新增 POC
type 1 或 2，不新增 slice group 或 partitioned data，也不改变 opaque payload 语义。
它不扩展 DSL、context model、compiler、VM，也不改变 `@range`、`@equals` 与 enum
constraint 的 diagnostic severity。

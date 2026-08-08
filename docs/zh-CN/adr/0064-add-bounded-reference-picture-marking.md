# 新增有界 reference-picture marking

状态：已接受
日期：2026-08-08

## 背景

到目前为止，每一个有界 non-IDR slice-header 增量都要求 type-1 NAL unit 满足
`nal_ref_idc == 0`。ADR-0054 引入该 source-anchored assertion 正是因为 payload dispatch
只按 `nal_unit_type` 选择结构；没有它，规则会把 reference-picture marking 的 bit 静默当作
下一个 slice-header 字段来解释。其后果是 bundled profile 只覆盖 non-reference slice，而这
恰好排除了最常见的情况：P 与 B slice 自身就是 reference picture 的码流。

clause 7.3.3 规定只要 `nal_ref_idc` 非零就读取 `dec_ref_pic_marking()`。对 non-IDR slice，
clause 7.3.3.3 读取 `adaptive_ref_pic_marking_mode_flag`：值零选择 sliding-window marking
且不再读取任何内容；值一进入一个 memory-management control operation 循环，并由 operation
零终止。每个 operation 读取各自的 operand 组合：operation 1 与 3 读取
`difference_of_pic_nums_minus1`，operation 2 读取 `long_term_pic_num`，operation 3 与 6
读取 `long_term_frame_idx`，operation 4 读取 `max_long_term_frame_idx_plus1`，operation 0
与 5 不读取 operand。

ADR-0063 正是为这一依赖新增了 `header_value(element_field)` leaf，因此该 presence 条件现在
无需改动语言即可表达。

## 决策

移除 `assert(nal_unit_type != 1 || nal_ref_idc == 0)` 前置约束，并在 sequence-element guard
下解码 `dec_ref_pic_marking()`：

```cpp
if (header_value(nal_ref_idc) == 0) {
} else {
    bits<1> adaptive_ref_pic_marking_mode_flag;
    if (adaptive_ref_pic_marking_mode_flag == 1) {
        repeat (64) {
            ue memory_management_control_operation
                @enum(MemoryManagementControlOperation);
            computed<bool> marking_uses_pic_num_difference =
                memory_management_control_operation == 1 ||
                memory_management_control_operation == 3;
            if (marking_uses_pic_num_difference) {
                ue difference_of_pic_nums_minus1;
            }
            if (memory_management_control_operation == 2) {
                ue long_term_pic_num_mmco;
            }
            computed<bool> marking_uses_long_term_frame_idx =
                memory_management_control_operation == 3 ||
                memory_management_control_operation == 6;
            if (marking_uses_long_term_frame_idx) {
                ue long_term_frame_idx;
            }
            if (memory_management_control_operation == 4) {
                ue max_long_term_frame_idx_plus1;
            }
        } until (memory_management_control_operation == 0);
    }
}
```

空的 `then` 分支是有意为之。`if` condition 只接受 equality，因此 non-reference 情况无法写成
`header_value(nal_ref_idc) != 0`。反转分支让 guard 保持在该语言唯一接受的形式上；空路径不
消费 bit，也不创建 node。

`MemoryManagementControlOperation` 是覆盖 `0..6` 的闭集 enum。保留值不对应任何 operand
组合，因此它是 layout-critical：之后每个字段都会错位。所以它在完整 Exp-Golomb 码字处致命
失败，与 `ModificationOfPicNumsIdc` 遵循同一条 framing-versus-conformance 边界。operation
零是 sentinel，保留在树中。

只有一个名字与扁平字段命名空间冲突：clause 7.3.3.3 的 `long_term_pic_num` 已经被 list 0
modification loop 投射。该字段改名为 `long_term_pic_num_mmco`。与 ADR-0062 不同——那里有
四个名字冲突，统一加 `_l1` 更清晰——本循环其余四个 operation 字段都是独有的，因此保留
clause 名。该后缀标记的是呈现层冲突，而不是另一个 syntax element。

两个 computed Boolean 之所以存在，是因为 `if` condition 无法内联表达 disjunction。它们与
既有 modification loop 中的 `uses_abs_diff_pic_num` 同构，且没有 source location。

该 loop 以 64 个 operation 受界，这是 sentinel repeat 的语言上界。这是资源边界，不是宣称的
conformance limit。

type-5 assertion 仍要求 `nal_ref_idc != 0`，这是正确的：IDR picture 一定是 reference
picture，而 `IdrSliceLayerWithoutPartitioningRbsp` 已经把 clause 7.3.3.3 的 `IdrPicFlag`
分支声明为 `no_output_of_prior_pics_flag` 与 `long_term_reference_flag`。

package 版本 `0.1.19` 声明这次的追加字段面。coverage depth 变为
`i-p-b-reference-slice-header`，因为本增量拓宽的是 profile 所接受的 slice 范围，而不是补完
某个已被接受形状的可选字段。

回归 fixture 覆盖：不含 marking 字段的 non-reference slice；sliding-window marking；每个
operation 各自的 operand 组合；多 operation 列表；保留在树中的 terminator；保留 operation
在其码字处失败；截断的 operation 与截断的 operand；精确的 child 顺序与 source span；以及
后续 NAL unit 继续被扫描。

## 影响

bundled profile 现在接受 reference P 与 B slice，也就是真实码流实际包含的内容。结合此前
若干增量，type-1 的 slice-header projection 覆盖了 reference 与 non-reference 两种形态下的
I、P、B slice。

移除该前置约束改变的是既有行为，而不只是新增行为。`nal_ref_idc` 非零的 type-1 NAL 此前会在
`nal_ref_idc` 处、payload mapping 之前失败，现在则可以解码。因此固定旧拒绝行为的那条回归被
替换而不是修补——它断言的正是本增量所移除的东西。

空 `then` 分支是 bundled rule 第一次为满足 equality-only 的 condition 形式而反转 guard。若
该模式反复出现，把 `header_value` 允许进入 `computed<bool>` 就能让规则写成
`computed<bool> is_reference = header_value(nal_ref_idc) != 0;`，读起来更直接。此处选择推迟
而非采纳，因为那会拓宽该 leaf 被允许的位置。

## 非目标

本决策不校验 marking 语义，不跟踪 decoded-picture buffer，不强制 clause 7.4.3.3 中 operation
之间的关系，不检测同一列表内矛盾或重复的 operation，不新增 `pred_weight_table()` 或
explicit weighted biprediction，不解码 CABAC 或 CAVLC `slice_data`，不新增 SP 或 SI slice
type，不支持 field picture 或 MBAFF，不新增 POC type 1 或 2，不新增 slice group 或
partitioned data，也不改变 opaque payload 语义。它不扩展 DSL、context model、compiler、VM，
也不改变 `@range`、`@equals` 与 enum constraint 的 diagnostic severity。

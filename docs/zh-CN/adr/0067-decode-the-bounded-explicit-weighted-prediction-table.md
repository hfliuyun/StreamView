# 解码有界的显式加权预测表

Status: Accepted
Date: 2026-08-09

## 背景

迄今为止每一个有界 non-IDR slice-header 增量都带着两条 source-anchored assertion：

```
assert(!is_p_slice ||
       context_value(pic_parameter_set_id, h264_pps, weighted_pred_flag) == 0)
    at pic_parameter_set_id;
assert(!is_b_slice ||
       context_value(pic_parameter_set_id, h264_pps, weighted_bipred_idc) != 1)
    at pic_parameter_set_id;
```

它们存在的原因是：clause 7.3.3 规定，当 P 或 SP slice 使用
`weighted_pred_flag == 1` 的 PPS，或者 B slice 使用 `weighted_bipred_idc == 1`
的 PPS 时，就要读取 `pred_weight_table()`。没有这两条 assertion，规则会把表的
bit 当成 slice header 的下一个字段来读。其后果是 bundled profile 拒绝一切使用
显式加权的码流，而普通编码器输出确实会用到它。

Clause 7.3.3.2 先读 `luma_log2_weight_denom` 与 `chroma_log2_weight_denom`，
然后循环 `num_ref_idx_l0_active_minus1 + 1` 次，每次先读 `luma_weight_l0_flag`
（guard 一对 weight/offset），再读 `chroma_weight_l0_flag`（guard 两个 Cb 与
两个 Cr 值）。B slice 对 list 1 重复整个循环。

**表体从来不是障碍，次数才是。** `num_ref_idx_lX_active_minus1` 只在
`num_ref_idx_active_override_flag == 1` 时存在，否则次数来自 PPS 默认值。从
controller 位置命名那个被覆盖的字段会失败，报
`Computed field dependency is not guaranteed on the current branch`；而能绕开这个
选择的 narrowing assertion **没有合法位置**——写在顶层时报同一条 branch-guarantee
错误，写进该 flag 可用的 `if` 内部则报
`Assertions must be unconditional top-level items`。

ADR-0066 正是为这条 dependency 新增了
`optional_value(field_identifier, fallback_expression)`，因此现在无需再改语言就能
表达这个次数。

## 决策

移除两条 assertion，在一个 computed 存在性 guard 下解码 `pred_weight_table()`：

```
computed<bool> uses_explicit_weighting =
    (is_p_slice &&
     context_value(pic_parameter_set_id, h264_pps, weighted_pred_flag) == 1) ||
    (is_b_slice &&
     context_value(pic_parameter_set_id, h264_pps, weighted_bipred_idc) == 1);
if (uses_explicit_weighting) {
    ue luma_log2_weight_denom @range(0, 7);
    ue chroma_log2_weight_denom @range(0, 7);
    computed<u64> effective_l0_count =
        optional_value(num_ref_idx_l0_active_minus1,
                       context_value(pic_parameter_set_id, h264_pps,
                                     num_ref_idx_l0_default_active_minus1)) + 1;
    repeat (effective_l0_count, 32) {
        bits<1> luma_weight_l0_flag;
        if (luma_weight_l0_flag == 1) {
            se luma_weight_l0;
            se luma_offset_l0;
        }
        bits<1> chroma_weight_l0_flag;
        if (chroma_weight_l0_flag == 1) {
            se chroma_weight_l0_cb;
            se chroma_offset_l0_cb;
            se chroma_weight_l0_cr;
            se chroma_offset_l0_cr;
        }
    }
    if (is_b_slice) {
        computed<u64> effective_l1_count = /* list 1 同样形状 */;
        repeat (effective_l1_count, 32) { /* 同样形状，改用 _l1 命名 */ }
    }
}
```

存在性条件由**单个 `computed<bool>`** 承载，而不是嵌套 conditional。这是探测的
结论而非偏好：用 `if (is_p_slice)` 包住一个 imported equality、再对 `is_b_slice`
做同样的事，会被 `Duplicate field name` 拒绝——因为一个 structure 只有**一个扁平
字段命名空间**，互斥分支不能重名。嵌套因此会迫使整张表以不同名字复制两份，而这
正是 ADR-0066 要避免的代价。

**色度权重字段无条件存在。** `chroma_format_idc` 只在 `profile_idc == 100` 下声明
且在那里被 `@equals(1)` 钉死，其余受支持 profile 根本不声明它，所以受支持子集内
ChromaArrayType 恒为 1。不需要新增 SPS export，也不需要改动 context 契约。

**`_cb` 与 `_cr` 后缀是必需的，不是风格选择。** Clause 7.3.3.2 对单个 chroma
元素循环两次，但单整数形式 `repeat (n)` **永远是 sentinel 形式**，因此固定 2 次的
循环不可表达，只能把四个字段展开写。`_l1` 后缀沿用 list 1 modification loop 的既有
先例：每个 structure 一个扁平命名空间，所以后缀只用于呈现层消歧，并不表示另一个
语法元素。

两个循环都以 32 为上界，与 count 字段已经携带的 `@range(0, 31)` 一致。

**`weighted_bipred_idc == 3` 不需要新 assertion。** `@enum` 提供 fatal validation，
而 `WeightedBipredIdc` 只声明 0、1、2，因此保留值在 PPS 处已经是
`invalid-syntax`——那正是正确的锚点，因为该值就是在那里读取的。

## 影响

这是四个增量以来**第一个真正改变解码输出**的 bundled 规则变更，因此 `rule.toml`
从 `0.1.19` 升版本。前三个是 capability-only，故意不升。

24 个 analyzer 测试失败。这是把改动打进规则跑完整套件**实测**得出的，不是估算：

- **22 个是纯索引平移。** child count 一律 12 → 13，因为**计算字段总会物化为可见
  树节点**。于是每个 non-IDR slice 都多一个节点，与它是否真的带表无关。这是既有
  「计算字段总是物化、没有隐藏机制」设计的直接后果，不是可以绕开的实现细节。
- **2 个是真实语义变更，已重写而不是改索引。**
  `rejectsWeightedNonIdrPSliceAtPictureParameterSetAndContinues`（10 → 19
  children）与
  `rejectsExplicitWeightedBipredictionAtPictureParameterSetAndContinues`
  断言的正是本增量要解除的限制：它们期望被拒绝的码流现在会被解码。

`acceptsImplicitWeightedBipredictionInNonIdrBSlice` 保持有效且无需改动；
`weighted_bipred_idc == 2` 不进入该 guard。

这 2 个重写扩成 5 个测试，覆盖两个 `optional_value()` 叶子各自的两种状态：

- `decodesWeightedPredictionTableWithImportedDefaultCountInNonIdrPSlice`——P
  slice，list 0 次数回落到 imported 的 PPS 默认值。
- `decodesWeightedPredictionTableWithOverriddenCountInNonIdrPSlice`——P slice，
  list 0 次数取自 slice header 中声明的 override。
- `decodesExplicitWeightedBipredictionTableForBothReferenceLists`——B slice，
  两个次数都回落到各自 imported 的 PPS 默认值。
- `decodesExplicitWeightedBipredictionTableWithOverriddenCounts`——B slice，
  两个次数都被声明，两个 list 因此跑到不同长度。
- `reportsTruncatedExplicitWeightedPredictionTable`——表被码流末尾截断。

每个测试断言的是完整有序的 child 名字列表，而不是位置索引；这样后续 slice-header
增量插入字段时会以名字不匹配的形式失败，而不是变成一次被静默平移的比较。截断用例
把码流终止在 `luma_weight_l0[0]` 与 `luma_offset_l0[0]` 之间，确认部分前缀仍然物化、
诊断锚定在未读到的字段上。这些码流由生成脚本装配，其解码结果经 `svtool analyze`
回读，因此每个被断言的值都是实测而非手算。

分析器测试套件最终 97 passing。

## 非目标

隐式加权双向预测（`weighted_bipred_idc == 2`）不读表，保持不变。4:2:0 之外的色度
格式仍在受支持子集之外。解码出的权重的**语义**——解码器如何把它们施加到预测
样本上——不属于分析器范围；本增量解码并呈现语法。field-coded 与 MBAFF 码流仍在
范围之外，因此不涉及依赖 `field_pic_flag` 的次数调整。

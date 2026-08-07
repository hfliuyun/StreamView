# 新增有界 P-slice reference-index override

状态：已接受
日期：2026-08-07

## 背景

ADR-0057 为编码 slice type 0 和 5 新增了有界 progressive non-reference P-slice header。
规则会读取 `num_ref_idx_active_override_flag`，但目前要求它为零，因此总是从所选 PPS 继承
list 0 active-reference count。

clause 7.3.3 会在 P 与 SP slice 的 override flag 为一时，紧随其后放置 unsigned Exp-Golomb
`num_ref_idx_l0_active_minus1`。B slice 还会携带 list 1 count，但这些 slice type 仍不属于
bundled profile。该 count 不会改变后续是否存在 `ref_pic_list_modification_flag_l0` bit。

既有 DSL 已支持本地 scalar condition、guarded `ue` field 与非致命 unsigned range；该语法
分支不需要扩展 engine 或 context。

## 决策

保留 `num_ref_idx_active_override_flag` 作为 P-slice 必需语法，但移除它的 `@equals(0)`
constraint。值为一时按 clause 顺序读取 list 0 override：

```cpp
bits<1> num_ref_idx_active_override_flag;
if (num_ref_idx_active_override_flag == 1) {
    ue num_ref_idx_l0_active_minus1 @range(0, 31);
}
bits<1> ref_pic_list_modification_flag_l0 @equals(0);
```

override count 表示 slice 使用 `num_ref_idx_l0_active_minus1 + 1` 个 active list 0 entry，
替代 PPS default。对于受支持的 progressive frame 路径，`0..31` 是 conformant value。值大于
31 时保留完整码字，产生既有带 source 位置的 `invalid-syntax` warning，并继续解码，因为
Exp-Golomb 长度及所有后续字段位置仍然已知。

flag 为零时 override field 不出现，既有 P-slice child order 与 bit boundary 保持不变。规则不发布
computed effective-reference count，因为当前已解码的下游语法不会消费该值。

reference-list modification flag 仍然是必需字段并被约束为零。weighted-prediction 与 CABAC
PPS assertion 继续位于该 flag 之后；type-1 direct-header assertion 仍要求
`nal_ref_idc == 0`，因此继续排除 `dec_ref_pic_marking()`。

package version `0.1.14` 发布这一 additive rule surface。coverage depth 保持
`i-p-slice-header`，因为该增量扩展既有 P-header 分支，没有新增 slice family。

回归 fixture 覆盖编码 P 值 0/5 的非默认 override、zero-flag absence 路径、不会错移 payload
的越界 count warning、截断 count、override 后仍不支持的 list modification、精确 source span，
以及后续 NAL 继续扫描。

## 影响

有界 P-slice header 现在可以声明自身的 list 0 active-reference count，同时继续显式隔离所有
后续不支持的布局分支。新的 source-backed field 只在 controller flag 为一时出现，位置在该
flag 与 `ref_pic_list_modification_flag_l0` 之间。

非致命 range diagnostic 会区分 conformance value 问题与布局不确定：值 32 会产生 warning，
但不会移动或隐藏 QP 与 opaque slice-data boundary。

## 非目标

本决策不新增 list 1 override count、B/SP/SI slice type、field picture 或 MBAFF 专属的
`0..15` 上限、computed effective-reference count、针对 decoded picture buffer 的校验、
reference-list modification loop、weighted prediction 或 `pred_weight_table()`、CABAC 或
`cabac_init_idc`、非零 reference 的 type-1 header、reference-picture marking、adaptive
memory-management operation、POC type 1/2、slice group、partitioned data 或 CAVLC/CABAC
slice-data 解码；不新增 context mechanism，也不改变 opaque NAL 语义。

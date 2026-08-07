# 新增有界 progressive non-IDR P-slice header

状态：已接受
日期：2026-08-07

## 背景

内置 H.264 规则已经可以解码有界 progressive all-I IDR 与 non-IDR slice header。
ADR-0055 把 NAL unit type 1 限定为 non-reference picture，因此该路径不需要
`dec_ref_pic_marking()`。ADR-0056 又允许 source-anchored assertion 把本地语法与精确 imported
PPS value 组合起来。

type 1 的下一个有用增量是 non-reference P-slice。它会在 `slice_qp_delta` 前增加
reference-list 语法；所选 PPS 也可能启用 `pred_weight_table()` 或 `cabac_init_idc`。把其中任意
bit 当作 opaque suffix 会移动 slice-data boundary，而静默省略它们则会把语法错误标成
`slice_qp_delta`。

因此首个增量必须实际读取两个必需的 P-slice control flag，并拒绝所有尚未建模的可变分支；
同一个 type-1 dispatch 中已有的 all-I 路径仍须保持有效。

## 决策

把 type-1 enum 与 structure 重命名为能反映更宽 package-visible surface 的名称：

- `NonIdrAllISliceType` 改为 `NonIdrSliceType`；
- `NonIdrAllISliceLayerWithoutPartitioningRbsp` 改为
  `NonIdrSliceLayerWithoutPartitioningRbsp`。

`NonIdrSliceType` 声明所支持的 H.264 编码值：

```cpp
enum NonIdrSliceType {
    p = 0;
    i = 2;
    all_p = 5;
    all_i = 7;
}
```

紧跟 `slice_type` 发布 presentation-visible computed field：

```cpp
computed<bool> is_p_slice = slice_type == 0 || slice_type == 5;
```

对于 P 值 0 和 5，在既有 picture-order 与 redundant-picture 分支之后、
`slice_qp_delta` 之前读取 clause 7.3.3 字段：

```cpp
bits<1> num_ref_idx_active_override_flag @equals(0);
bits<1> ref_pic_list_modification_flag_l0 @equals(0);
```

即使值为零，这两个 bit 仍是 P-slice 的必需语法。值为一时在完整 controller bit 上失败，
不让尚未支持的 reference count 或 modification loop 改变布局。

在这两个 flag 之后，要求精确选中的 PPS 禁用两个被省略的分支：

```cpp
assert(!is_p_slice ||
       context_value(pic_parameter_set_id, h264_pps, weighted_pred_flag) == 0)
    at pic_parameter_set_id;
assert(!is_p_slice ||
       context_value(pic_parameter_set_id, h264_pps, entropy_coding_mode_flag) == 0)
    at pic_parameter_set_id;
```

Boolean short-circuit 会保持 all-I 值的既有行为。对于 P 值，违反 PPS prerequisite 会得到致命
`invalid-syntax`，并锚定到完整 `pic_parameter_set_id` 码字；missing、future 或 stale generation
仍沿用 `dependency-unavailable`。type-1 direct-header assertion 继续要求
`nal_ref_idc == 0`，因此不会出现 reference-picture marking 语法。

共同的 picture-order、redundant-picture、QP、deblocking-control 与 opaque `slice_data` 字段
保持既有顺序和语义。package version `0.1.13` 用 coverage depth `i-p-slice-header` 发布这一
additive rule 与 presentation-name 变化。

回归 fixture 覆盖 P slice type 0 和 5、保持不变的 all-I 路径、两个非零 control flag、两个
imported PPS prerequisite、精确 field/payload span，以及后续 NAL 继续扫描。

## 影响

内置规则可以公开首个有界 P-slice header，同时不声称解析可变 reference-list、weighted-
prediction、reference-picture marking 或 CABAC 分支。受支持的 P header 会把 opaque
slice-data boundary 放在两个必需的零 flag 与 `slice_qp_delta` 之后。

通用 enum 与 structure 名会替换此前的 all-I presentation 名。固定使用 package `0.1.12` 的
consumer 保留旧 surface；使用 `0.1.13` 的 consumer 会看到新名称及新增的可见
`is_p_slice` node。

## 非目标

本决策不新增 B/SP/SI slice type、非零 reference 的 type-1 header、field picture、POC type
1/2、reference-index override 语法、reference-list modification loop、weighted prediction 或
`pred_weight_table()`、CABAC 或 `cabac_init_idc`、reference-picture marking、adaptive
memory-management operation、slice group、partitioned data 或 CAVLC/CABAC slice-data 解码；
不新增 context mechanism，也不改变 opaque NAL type 的语义。

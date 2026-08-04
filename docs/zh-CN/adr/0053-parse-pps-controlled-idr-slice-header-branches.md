# 解析由 PPS 控制的 IDR slice-header 分支

状态：已接受
日期：2026-08-06

## 背景

bundled H.264 package `0.1.9` 已经能解码有界 progressive、POC type 0、all-I IDR
slice header。它目前通过 checked dynamic-width division 拒绝所选 PPS 可以启用的三组可选
语法：bottom-field picture order、redundant-picture count 与 deblocking-filter control。
ADR-0052 已允许 rule 使用 imported PPS 导出的精确值保护字段，因此这些布局决定不再需要
拒绝占位符。

完整 slice header 的范围仍然大得多。non-IDR picture、P/B slice、field picture、其他 POC
type、reference-list modification、weighted prediction、adaptive memory management 与 slice
group 都需要各自独立的有界 rule 增量和 fixture。

## 决策

从 `IdrSliceLayerWithoutPartitioningRbsp` 移除三个 PPS presence divisor，并按字段顺序声明
对应的 clause 7.3.3 语法：

- 在 `pic_order_cnt_lsb` 后，仅当
  `bottom_field_pic_order_in_frame_present_flag == 1` 时读取 signed
  `delta_pic_order_cnt_bottom`；
- 在 IDR reference-picture-marking flag 前，仅当
  `redundant_pic_cnt_present_flag == 1` 时读取 `redundant_pic_cnt`，并用非致命
  `@range` warning 表达 clause 7.4.3 的 `0..127` value domain；
- 在 `slice_qp_delta` 后，仅当
  `deblocking_filter_control_present_flag == 1` 时读取 deblocking-filter control。

把 `disable_deblocking_filter_idc` 声明为 unsigned Exp-Golomb syntax，并使用命名 enum
值域 `enabled = 0`、`disabled = 1` 与 `enabled_within_slice = 2`。reserved 值会控制后续字段
是否存在，因此在完整码字处致命失败。值 1 省略两个 signed offset；值 0 和 2 读取
`slice_alpha_c0_offset_div2` 与 `slice_beta_offset_div2`。DSL 没有 `!=` condition，所以 rule
用空的 `== 1` branch 和在 `else` 中声明的两个 offset 表达该关系。当前 DSL 无法表达的
signed semantic bound 只记录在文档中，暂不强制。

每个 presence decision 都使用 ADR-0052 的 exact imported-context equality 形式。false guard
不消费 source bit，也不创建 field node。因此剩余的 `compressed_payload slice_data` 会紧接在
最后一个被选中的可选字段之后，并继续包含所有剩余 RBSP bit，其中也包括 slice trailing bit。

package version `0.1.10` 发布这一 additive rule 变化，coverage token 保持
`idr-slice-header`。回归 fixture 覆盖三个 presence flag 全部启用、包含与省略 offset 的
deblocking 值、false-guard 字段缺席、精确 opaque-payload boundary，以及 reserved deblocking
值失败后后续 NAL 仍然 materialized。

## 影响

官方 rule 现在可以为既有 progressive all-I IDR 形状解码三组由 PPS 控制的可选语法，而无需
把 H.264 专用决定移入 analyzer。imported guard 会与本地 deblocking enum guard 组合，并且只有
选中的字段影响 source-mapped header boundary。

阶段 3 的 Baseline/Main/High slice-header 项仍未完成。该增量移除了三个刻意设置的前提，但
不会扩展 NAL、slice-type、frame-picture 或 POC-type 值域。

## 非目标

本决策不支持 non-IDR NAL、P/B/SP/SI 语法、field picture、POC type 1/2、data
partitioning、reference-list modification、weighted prediction、adaptive memory-management
operation、slice group 或 CAVLC/CABAC 解码；也不向 DSL 新增一般 imported expression 或
signed range constraint。

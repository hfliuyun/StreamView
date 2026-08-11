# 新增有界 IDR all-I slice header

Status: Accepted
Date: 2026-08-04

## 背景

内置 H.264 rule 已经能够发布精确 SPS/PPS generation、在后续 consumer 中导入所选 PPS、
从该 PPS 及其绑定 SPS 求值 dynamic field width，并把全部剩余 bit 保存为 compressed
payload。因此第一条规则增量可以把这些语言切片连接成真实 VCL NAL，而无需把 H.264 lookup
或布局决策移入 Annex B analyzer。

完整 clause 7.3.3 slice header 包含许多会改变布局的分支。P/B slice 会新增 reference-list
modification、weighted prediction 和不同的 reference-picture marking。field picture、POC type
1/2、bottom-field POC、redundant picture、deblocking control、slice group、SP/SI 语法与 data
partitioning 也需要额外声明分支。一步声称全部支持会超出现有 rule 与回归 fixture 能证明的范围。

## 决策

新增 `IdrSliceLayerWithoutPartitioningRbsp`，并把 NAL unit type 5 派发给它。首个有界
structure 只接受 `slice_type == 2`，并导入 `pic_parameter_set_id` 指定的 PPS。它解码：

- `first_mb_in_slice`、`slice_type` 与 `pic_parameter_set_id`；
- 由 SPS 决定宽度的 `frame_num` 与 `pic_order_cnt_lsb`；
- `idr_pic_id`；
- IDR reference-picture-marking flag；
- `slice_qp_delta`。

dynamic-width expression 还会在受影响字段读取 source 前强制布局前提：
`frame_mbs_only_flag == 1`、`pic_order_cnt_type == 0`、
`bottom_field_pic_order_in_frame_present_flag == 0`、
`redundant_pic_cnt_present_flag == 0` 与
`deblocking_filter_control_present_flag == 0`。checked division 以必须为一的除数表达这些条件；
前提不成立时，在字段消费 bit 前返回 `invalid-syntax`。PPS 已要求只有一个 slice group，因此
不存在 slice-group-change 字段；I slice 不携带 CABAC-init、reference-list modification 或
prediction-weight 语法。

后续：ADR-0071 与 package `0.1.24` 已移除 POC-type-0 dynamic-width 前提，改为显式声明
type-0、type-1 与 type-2 分支。

最终的 `compressed_payload slice_data;` 把全部剩余 RBSP bit 映射为 materialized
`CompressedPayload` node。字段名沿用 H.264 语法，但 remaining-bit terminal 表示完整 opaque
slice-layer suffix，其中包括 entropy-coder alignment 与 slice trailing bits；它不声称已经解码
CAVLC 或 CABAC。该精确终结项使 dispatched structure 满足 runner 的完整消费合同。

missing、future 或 stale PPS/SPS generation 仍在 source-backed `pic_parameter_set_id` 上报告
`dependency-unavailable`，且不会选择更旧 generation。失败只影响当前 VCL NAL，扫描继续。
package version `0.1.8` 发布该规则变化，entry-point coverage token 更新为
`idr-slice-header`。

## 后果

常见 progressive IDR I slice 现在能通过与 SPS/PPS 相同的 rule execution session 暴露
source-mapped header 与 bit-precise opaque payload。Annex B analyzer 保持 format-neutral：rule
负责声明 context selection、dynamic width、fatal layout prerequisite、字段与 payload boundary。

支持形状刻意窄于阶段 3 的 slice-header 目标，因此该目标仍未完成。被拒绝的分支保留已经解码
的前缀与 source-located diagnostic，不猜测后续布局。

## 非目标

本决策不支持 non-IDR type 1 slice、`slice_type == 7`、P/B/SP/SI slice、field picture、POC
type 1/2、bottom-field POC、redundant picture、deblocking control、slice group、data
partitioning、reference-list modification、weighted prediction、adaptive memory-management
operation 或 CAVLC/CABAC 解码；也不声称完整 Baseline/Main/High slice-header coverage 或完整
H.264 conformance。

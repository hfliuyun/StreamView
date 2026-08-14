# 解码有界的 High-profile PPS extension

状态：已接受
日期：2026-08-11

## 背景

内置 H.264 规则目前接受 clause 7.3.2.2 的 picture parameter set base 语法，并要求
`redundant_pic_cnt_present_flag` 后立即出现 `rbsp_trailing_bits`。High-profile 码流还可以
携带可选的 PPS extension，其中包含 transform、scaling-matrix 和第二个 chroma QP offset
语法。只有 `more_rbsp_data()` 为 true 时 extension 才存在；仅凭被引用 SPS 的 profile
无法区分 extension 与合法的 base-only High-profile PPS。

ADR-0072 已加入这个 presence test 所需的非消费 source-state expression。稳定 DSL 的其余
能力可以导入被引用 SPS 的 profile，把致命 assertion 锚定到带 source 的 SPS identifier，
并解码有界 extension 字段。Scaling matrix 仍属于 layout-critical 分支：presence flag 为
一时，最终 signed offset 之前会出现一个或多个 scaling list，其数量依赖本增量范围之外的
SPS 状态。

## 决策

导出无条件 SPS 字段 `profile_idc`。PPS 保留对被引用 SPS generation 的 dependency，并在
解码 PPS 时同时导入该 generation。Dependency 保留后续 PPS consumer 的失效语义；import
提供 extension gate 所需的 profile 值。

在已有 base PPS 字段之后发布
`computed<bool> has_pps_extension = more_rbsp_data()`。带 source 锚点的 assertion 要求不存在
extension，或者被引用 `profile_idc` 为 100。Assertion 锚定 `seq_parameter_set_id`，因此携带
额外 PPS 语法的 Baseline、Main 或 Extended 码流会在读取任何 extension 字段之前失败。
Base-only PPS 会短路 imported lookup，并保持既有行为。

当 `has_pps_extension` 为 true 时，按 bitstream 顺序解码：

- `transform_8x8_mode_flag`；
- `pic_scaling_matrix_present_flag @equals(0)`；
- signed Exp-Golomb `second_chroma_qp_index_offset`。

Scaling-matrix flag 为一时，在该 flag 的单个 source bit 上返回致命 `invalid-syntax`。规则不会
跨过未支持的 scaling-list 布局错位读取 offset。`rbsp_trailing_bits;` 仍是最终、无条件的
顶层 item，负责消费 base-only 或 extended 两种 terminal pattern。

Context lookup 保持既有的 source-order generation 策略。Extension 不能导入在码流更晚位置
才发布的 SPS；失败的 SPS 重定义不会遮蔽之前最近的合法 generation；后续合法 SPS
generation 替换旧 generation 时，绑定旧 generation 的 PPS 会变为 stale。

## 影响

Package `0.1.25` 增加有界 High-profile PPS extension，同时保持 coverage depth
`picture-order-count-slice-header`，因为最深的 slice-header 支持面没有改变。既有 base-only
PPS 输出增加一个 computed presence node；接受的 extension 再增加三个带 source 的字段。

回归 fixture 覆盖 base-only 与 extended High-profile PPS、transform flag 的两个值、带精确
source span 的正负第二 chroma offset、scaling-matrix 拒绝、字段截断、非 High profile 拒绝、
missing/future/stale SPS generation、失败重定义恢复，以及失败 NAL 后继续扫描。

## 非目标

本决策不解码 PPS scaling list、flexible macroblock ordering，或其他未支持的 SPS profile 与
chroma format。它不校验 signed QP-offset 数值域，不推导 transform 或 quantization 语义，
也不增加 picture-order、decoded-picture-buffer 或 output-order 状态。

## 后续

ADR-0077 与 package `0.1.29` 随后为 `second_chroma_qp_index_offset` 应用非致命 signed
`@range(-12, 12)` 约束。

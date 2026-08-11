# 增加有界的 H.264 PPS 核心切片

状态：已接受
日期：2026-08-02

## 背景

内置 H.264 规则已经能解码有界的 sequence parameter set 核心，并在不丢失后续字段的
情况下报告已声明语义范围。下一个 M6 增量是 NAL unit type 8 携带的 picture parameter set。

base PPS 语法无需查找它引用的 SPS 即可解码，但两个可选区域不能这样处理。Flexible
macroblock ordering 会引入 slice-group 分支，其中包含稳定 DSL 无法表达的动态宽度
`slice_group_id` 字段。PPS extension 又包含 scaling-list 语法，其字段数量依赖被引用 SPS 的
`chroma_format_idc`。在结构内把任一区域当作 opaque 都会失去精确 RBSP 消费。

## 决策

在内置 Annex B 规则中增加 type-8 `PictureParameterSetRbsp` payload。首个接受的结构覆盖
clause 7.3.2.2 base 字段，要求 `num_slice_groups_minus1 == 0` 且不存在 PPS extension：

- PPS 与被引用 SPS 的 identifier；
- entropy coding 与 bottom-field picture-order flag；
- 默认 reference-index count 与 weighted-prediction control；
- 初始 QP/QS 与 chroma QP offset；
- deblocking、constrained-intra 与 redundant-picture control；
- base 语法之后立即出现的 `rbsp_trailing_bits;`。

为 `pic_parameter_set_id`（`0..255`）、`seq_parameter_set_id`（`0..31`）以及两个
`num_ref_idx_l*_default_active_minus1` 字段（`0..31`）添加非致命 `@range` constraint。用 enum
声明 `weighted_bipred_idc` 接受的三个值，避免静默接受 reserved 值 3。要求
`num_slice_groups_minus1 @equals(0)`；非零值会改变后续布局，属于 layout-critical，因此会保留
已解码前缀，并让该 PPS 以 `invalid-syntax` 停止。

已声明结构必须消费完整 RBSP。额外 PPS extension bit 会触发现有 trailing-bits 或精确消费
检查，不会被误认为已支持字段。package 版本 `0.1.4` 用来发布新增规则资产。

后续：ADR-0073 与 package `0.1.25` 只取代上述无 extension 边界。它们使用
`more_rbsp_data()` 接受有界 High-profile PPS extension，同时继续把 scaling-list 语法作为
layout-critical unsupported。

本切片中的 PPS materialized 只表示已声明 base 结构被精确消费。`seq_parameter_set_id` 仍是
带 source 的 identifier；analyzer 尚不查找 SPS generation、不注册 PPS generation，也不证明
后续 slice header 可以使用该 parameter set。

## 影响

type-8 NAL 通过与 SPS、AUD 相同的规则 payload dispatch 公开带 source mapping 的 PPS base
结构。未支持 slice group、reserved weighted biprediction、PPS extension、截断和错误 trailing
bit 都产生明确诊断；ID/count 的语义范围违规则以 warning 保留完整结构。

这个 base 结构为后续 context 注册和 slice-header 工作提供稳定字段表面，不向 Annex B
analyzer 加入 H.264 特判。

## 非目标

本决策不增加 flexible macroblock ordering、PPS extension 或 scaling-list 语法、SPS lookup、
PPS 注册、dependency generation 或 slice-header dispatch。它不增加 signed range constraint；
包括依赖 SPS bit depth 的 `pic_init_qp_minus26` 在内，signed QP 字段的语义范围暂不检查。
本切片不声称完整 H.264 PPS conformance。

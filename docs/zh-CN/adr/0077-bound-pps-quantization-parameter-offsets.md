# 约束 PPS 量化参数偏移

状态：已接受
日期：2026-08-14

## 背景

ADR-0041 引入了基础 `PictureParameterSetRbsp` payload，ADR-0073 增加了 High
profile extension。两项决策均将 `pic_init_qs_minus26`、`chroma_qp_index_offset` 与
`second_chroma_qp_index_offset` 解码为有符号 Exp-Golomb 字段（`se`），但未对其取值
范围做校验，接受该编码可表示的所有值。

ADR-0076 将非致命 `@range(minimum, maximum)` 约束扩展到 `se` 字段，并约束了 slice
deblocking offset。

ITU-T H.264 clause 7.4.2.2 规定了 PPS QP offset 的一致性语义值域：

- `pic_init_qs_minus26` 须在 `-26..25` 范围内（闭区间）；
- `chroma_qp_index_offset` 须在 `-12..12` 范围内（闭区间）；
- `second_chroma_qp_index_offset`（存在 PPS extension 时）须在 `-12..12` 范围内（闭区间）。

这些均为语义值域而非布局选择器。完整的有符号 Exp-Golomb 码字仍决定后续字段（如
`deblocking_filter_control_present_flag` 或 `rbsp_trailing_bits`）的起始偏移与
opaque payload 边界。越界值不符合规范，但若拒绝整个 PPS 或终止解析，会丢弃后续字段
偏移与后续 NAL 单元依然明确的结构。

相比之下，`pic_init_qp_minus26` 的符合规范值域为
`-(26 + QpBdOffsetY)..(25 + QpBdOffsetY)`，动态依赖于所引用的 SPS
`bit_depth_luma_minus8`（`QpBdOffsetY = 6 * bit_depth_luma_minus8`）。由于 `@range`
仅接受整数字面量常量，无法表达关系型或上下文依赖型 bound，因此 `pic_init_qp_minus26`
在该层保持未约束并延期。

## 决策

在 `PictureParameterSetRbsp` 中为字面量值域的 PPS QP offset 应用静态非致命 `@range`
注解与 clause 7.4.2.2 引用：

- `se pic_init_qs_minus26 @range(-26, 25)`
- `se chroma_qp_index_offset @range(-12, 12)`
- `se second_chroma_qp_index_offset @range(-12, 12)`（位于 PPS extension 分支内）。

复用 ADR-0076 建立的有符号 Exp-Golomb `@range` 合同：值在范围内时不产生诊断；越界时
在字段上附加 severity 为 warning、带 source location 的 `invalid-syntax` diagnostic，
保留完整码字 span，使 PPS 结构保持 materialized，并继续解码后续字段与后续 NAL。

将 package 版本提升至 `0.1.29`，保持 coverage depth
`picture-order-count-slice-header`。

## 影响

内置 H.264 profile 会报告越界的 PPS QP offset，而不会中断分析或导致下游 bit offset
漂移。回归覆盖验证：

- 合法极值（QS 为 `-26` 与 `25`，chroma offset 为 `-12` 与 `12`）产生 0 条诊断；
- 首个非法值（QS 为 `-27` 与 `26`，chroma offset 为 `-13` 与 `13`）恰好产生 1 条
  `invalid-syntax` warning diagnostic，且带有正确的 fieldPath 与 message；
- 配对使用相同长度的 Exp-Golomb 码字证明合法与非法探针之间的后续字段起始位置
  （`deblocking_filter_control_present_flag`、`rbsp_trailing_bits`）与后续 NAL 单元
  完全未发生漂移；
- `PictureParameterSetRbsp` 中的兄弟字段不受影响。

## 非目标

本决策不约束 `pic_init_qp_minus26` 与 `slice_qp_delta`（其值域依赖 SPS
`QpBdOffsetY`）。它不增加表达式 bound 或关系型 range 约束。它不校验 slice-group
参数、PPS scaling matrix、POC 派生、DPB 管理或 output order。

## 后续

- ADR-0041：增加有界 H.264 PPS 核心切片
- ADR-0073：解码有界 High Profile PPS 扩展
- ADR-0076：将非致命 range 扩展到有符号字段

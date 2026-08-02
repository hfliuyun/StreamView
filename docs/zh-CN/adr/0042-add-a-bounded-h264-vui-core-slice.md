# 增加有界的 H.264 VUI 核心切片

状态：已接受
日期：2026-08-02

## 背景

内置 H.264 SPS 当前要求 `vui_parameters_present_flag == 0`。在有界 SPS 与 PPS base 结构完成
之后，下一个 M6 增量是描述 sample aspect ratio、video signal、timing 与 decoder restriction
的 VUI 语法。

大部分 VUI 分支由固定字段或有界 Exp-Golomb 值组成，可以由稳定 DSL 表达。HRD parameters
不同：它会引入一个带 count 的 schedule，随后是 delay-length 与 bitrate 数组，并被后续
buffering-period 和 picture-timing SEI 使用。它需要独立契约与 fixture，不应在本切片中做不完整
解释。

## 决策

把 SPS 的 `vui_parameters_present_flag @equals(0)` 边界替换为 Annex E.1.1 的可选 inline VUI
core。flag 为一时解码：

- aspect-ratio information，包括 Extended SAR width 与 height；
- overscan information；
- video format、full-range、colour primaries、transfer characteristics 与 matrix coefficients；
- top-field 与 bottom-field chroma sample location；
- timing information；
- NAL 与 VCL HRD presence 边界；
- `pic_struct_present_flag`；
- 完整 bitstream-restriction 分支。

要求两个 HRD presence flag 都等于零。HRD 存在时会改变后续布局，因此保留已解码前缀，并让
SPS 以 `invalid-syntax` 停止。两个 HRD 分支都不存在时没有 `low_delay_hrd_flag`，
`pic_struct_present_flag` 紧随其后。

为两个 chroma sample-location type 添加非致命 `@range(0, 5)` constraint。为
`max_bytes_per_pic_denom`、`max_bits_per_mb_denom` 与两个 `log2_max_mv_length_*` 字段添加
`@range(0, 16)`。其他 VUI 值仍带 source，但当稳定 DSL 缺少所需 fixed-width 或 relational
constraint 时，不声称其语义 conformant。

既有 SPS `rbsp_trailing_bits;` 继续作为可选 VUI 字段之后的精确终结项。package `0.1.5`
发布新增语法，`parameter-sets` coverage token 不变。

## 影响

常见 VUI metadata 现在会以精确 source span 展示，无需向 Annex B analyzer 添加 H.264 特判。
未支持 HRD 语法会在 presence flag 处明确失败，一个 SPS 失败也不妨碍扫描后续 NAL。

后续可在 inline 字段表面增加 HRD parameters，并由 SEI 工作复用。VUI core materialized 表示
精确消费了已声明分支，不表示完整 Annex E 语义 conformance。

## 非目标

本决策不解析 NAL/VCL HRD parameters、buffering-period 或 picture-timing SEI，也不解析 SPS
extension。它不检查 reserved fixed-width table 值、非零 SAR/timing 值、timing ratio，或
`max_num_reorder_frames`、`max_dec_frame_buffering` 与 SPS-derived decoder limit 的关系。
它不增加 SPS context 注册，也不改变有界 PPS 切片。

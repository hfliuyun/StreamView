# 增加有界的 H.264 SPS 核心切片

状态：已接受
日期：2026-08-02

## 背景

有界 RBSP trailing-bits 终结项已经能够精确消费变长的 H.264 sequence parameter set。
当前内置规则仍只派发 access-unit delimiter，而实施计划要求在 PPS、VUI/HRD 和 slice-header
之前先建立 SPS 结构。

H.264 SPS 包含依 profile 变化的 scaling list、多个 Exp-Golomb 分支以及可选 VUI/HRD 语法。
稳定 DSL 能表达核心字段和有界 repeat 投影，但首个切片不应假装已经解析了尚无完整规则表示的
可选语法。

## 决策

在内置 Annex B 规则中增加 type-7 `SequenceParameterSetRbsp` payload。首个接受的结构覆盖常见
8-bit Baseline/Main/Extended 核心，以及 4:2:0、luma/chroma 均为 eight-bit、关闭 transform bypass、
不带 scaling matrix 的 High 子集：

- NAL header 之后的 profile/level identity、constraint flags 和 SPS ID；
- frame-number 与 picture-order-count type 0；
- reference-frame、picture-size、frame-structure 与 cropping 字段；
- `vui_parameters_present_flag` 边界，随后是 `rbsp_trailing_bits;`。

规则通过 enum 接受 profile_idc 66、77、88 和 100。其他 profile 值会在 profile 字段处 invalid。
本切片尚未实现的 VUI flag、超出受限 8-bit 4:2:0 子集的 High profile 值以及其他 picture-order
分支不能被静默消费；在后续切片加入
对应字段前，它们会使 RBSP 保持 invalid。

后续：ADR-0071 与 package `0.1.24` 已加入 type-1 与 type-2 SPS picture-order 分支；本决策
提到的其他未支持语法仍会被拒绝。

首个结构切片尚不强制 clause 7.4.2.1.1 对 `log2_max_frame_num_minus4` 与
`log2_max_pic_order_cnt_lsb_minus4` 的 `0..12` 语义范围。它会发布带 source 的字段值，完整
range conformance 留给下一条 SPS constraint 切片。因此 structure 的 materialized 表示精确
消费了已声明 SPS 子集，不表示已经完成全部 H.264 语义一致性检查。

无符号 Exp-Golomb 字段可以带一个 `@equals` constraint，并继续作为 repeat controller；这不会
让它成为 equality 或 switch controller。compiler 将 constraint 降低到现有 `assert-equals`
instruction；VM 使用已物化的 Exp-Golomb range 报告带 source location 的 invalid-syntax
diagnostic。

## 影响

现在 type-7 NAL 会为支持的核心生成带 source mapping 的 SPS structure，并拒绝未实现的可选语法，
而不向 analyzer 增加 H.264 特判。后续可以在每个语法切片拥有独立有界契约和测试之后，继续在此
结构中增加 scaling list、VUI/HRD、PPS 与按位置注册。

## 非目标

本决策不增加 context-directory 注册、PPS 引用、VUI/HRD 解析、scaling-list 值或运行时大小数组；
也不把 `@equals` 扩展到 signed Exp-Golomb 字段，且尚不增加通用 `ue` range constraint。

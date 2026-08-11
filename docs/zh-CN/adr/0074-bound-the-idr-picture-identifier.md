# 约束 IDR picture identifier

状态：已接受
日期：2026-08-11

## 背景

内置 H.264 规则会在所有已支持的 type-5 slice header 中把 `idr_pic_id` 解码为 unsigned
Exp-Golomb 字段，但尚未表示 clause 7.4.3 要求该值处于闭区间 `0..65535`。

这是语义值域，而不是布局选择器。即使解码值越界，完整 Exp-Golomb 码字仍然决定后续
picture-order 语法的起点。拒绝整个 slice 会丢弃一个其余字段边界仍然已知的 header。

## 决策

为既有 `ue idr_pic_id` 声明增加 `@range(0, 65535)`。复用稳定的 unsigned Exp-Golomb
range contract：区间内的值不产生 diagnostic；更大值会在带 source 的字段上附加非致命
`invalid-syntax` warning，并保持外层 slice materialized。

Warning 覆盖完整码字。解码从码字实际结束处继续，因此 picture-order 字段、IDR marking
flag、`slice_qp_delta`、可选 deblocking 字段与 opaque `slice_data` 边界均保持不变。

## 影响

Package `0.1.26` 保持 coverage depth `picture-order-count-slice-header`，并增加已声明的 IDR
identifier 值域。回归 fixture 覆盖零、上界 `65535` 与首个非法值 `65536`；非法用例验证
33-bit warning span、materialized slice、未移动的后续字段与 payload 位置，以及继续扫描下一个
NAL。

## 非目标

本决策不约束 `first_mb_in_slice`，不派生 picture size，不要求 IDR `frame_num` 为零，不校验
signed QP 或 deblocking 值域，也不增加 picture-order、decoded-picture-buffer、MMCO 或
output-order 语义。

## 后续

ADR-0075 随后把非致命 `@range` 校验扩展到无符号 bit 字段，并用该能力要求 IDR
`frame_num` 为零，同时不停止 slice header 的剩余部分。

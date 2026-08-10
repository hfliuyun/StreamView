# 新增有界的二次幂表达式

状态：已接受
日期：2026-08-10

## 背景

H.264 clause 7.4.3.3 用 `MaxPicNum` 约束 `difference_of_pic_nums_minus1`。在当前支持的
SPS 子集中，帧图像的 `MaxPicNum` 是 `2^(log2_max_frame_num_minus4 + 4)`，场图像再乘二。
DSL 已有 checked unsigned arithmetic 与 imported context leaf，但没有有界幂运算或 shift
操作。手写 lookup 会变成格式专用实现，也不能推广到其他由宽度推导的关系。

## 决策

新增保留表达式 leaf `power_of_two(unsigned_expression)`。它恰好接受一个 `u64` 实参并返回
`u64`，在完整 bounded expression grammar 接受的所有位置可用，包括 pure function、computed
field、dynamic width、lazy byte count 与 assertion。exponent 沿用现有 checked expression 语义；
`0..63` 返回 `1 << exponent`，`64` 或更大值在求值时以致命 `invalid-syntax` 失败。

parser/compiler 在读取 source 前检查 arity 与 unsigned operand。typed IR 保存一个
`PowerOfTwo` expression node，VM 在求值前验证 operand。该运算不增加 source read、presentation
node，也不在外层 expression 已有 instruction 之外增加 bytecode instruction。

H.264 规则在 operation 1/3 的 `difference_of_pic_nums_minus1` repeat-local assertion 中使用
该 leaf，并乘以 `optional_value(field_pic_flag, 0) + 1`，覆盖当前 slice shape 中 frame 与 field
的 `MaxPicNum`。assertion 仍然只作用于当前 marking iteration。

## 影响

宽度推导的幂值可以表达，不需要通用 shift operator 或格式专用 helper。该运算保持在 `u64`
范围内，并受既有 expression node/depth/work 限制约束。DPB state、picture-number wrap 与
operation 顺序仍不在本增量范围内。

## 非目标

本决策不新增 bitwise operator、通用 exponentiation、可变 state、array indexing 或完整
decoded-picture-buffer conformance。

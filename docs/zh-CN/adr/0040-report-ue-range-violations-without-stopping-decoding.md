# 报告 ue range 违规但不中断解码

状态：已接受
日期：2026-08-02

## 背景

有界 H.264 SPS 核心切片发布了 `log2_max_frame_num_minus4` 与
`log2_max_pic_order_cnt_lsb_minus4`，但没有强制 clause 7.4.2.1.1 的 `0..12` 范围。
ADR-0039 已把该范围推迟到本切片。

DSL 目前的所有受检约束都与 layout 相关。`forbidden_zero_bit` 或 `reserved_zero_2bits`
上失败的 `@equals(0)`，以及未声明的 enum member，都说明规则对 bit 位置的假设已经出错，
因此结构中后续字段都不可信。这类约束会把结构标记为 invalid 并停止解码。

语义 range 约束不同。当 `log2_max_frame_num_minus4` 解码为 13 时，Exp-Golomb 码字本身
读取正确，后续每个字段仍精确位于规则预测的 offset。该值不符合规范，但并非无法解析。
此时停止结构会丢弃一个完全可恢复的 SPS，并隐藏分析者理解码流所需的 picture size、
cropping 和 frame structure。

## 决策

为无符号 Exp-Golomb 字段增加 `@range(minimum, maximum)` 字段注解，并以非致命方式报告违规。

该注解是静态的：每个字段最多出现一次，只能用于 `ue` 字段，且必须带两个整数参数，满足
`minimum <= maximum <= 2^64 - 2`。parser 与 compiler 都会拒绝违反这些规则的写法，因此
malformed constraint 是定义错误，而不是运行时意外。

违规时保留已物化的字段节点，为该字段附加带 source location、severity 为 `warning` 的
`invalid-syntax` diagnostic，并继续执行结构。结构自身状态不变：只有 range 违规的结构仍会
到达 `materialized`，所属 NAL 单元也不会被标记 invalid。执行状态保持成功，因此 analyzer
继续像此前一样派发后续 payload。

这与 `@equals` 有意不同。`@equals` 回答"我的 framing 是否正确"，`@range` 回答"这个值是否
符合规范"；只有前者构成停止解码的理由。

compiler 把一个 constraint 降低为两条 instruction：`assert-range-minimum` 与
`assert-range-maximum`，各自以对应边界作为 immediate。两条 instruction 让每个 immediate
都是朴素的边界值，并使 diagnostic 能指明实际被违反的那个边界。无论字段是否被选中，两条
instruction 都计入结构预算，与既有 guarded-field 记账方式一致。

内置规则对 `log2_max_frame_num_minus4` 与 `log2_max_pic_order_cnt_lsb_minus4` 应用
`@range(0, 12)`，package 版本升到 `0.1.3`。

## 影响

分析者会同时看到完整 SPS 和该字段上明确、带 source location 的一致性警告，而不是被截断的
结构。H.264 SPS、PPS、VUI 和 slice-header 中后续的语义范围可以复用同一注解和同一套非致命
报告契约。

diagnostic severity 现在承载了消费方必须尊重的含义：字段上存在 diagnostic 不再意味着其所属
结构已失败。field inspector 与 diagnostics 面板已从 core model 渲染 severity，因此 warning
无需改动展示层即可呈现。

## 非目标

本决策不把 `@range` 扩展到 `bits`、`se`、computed 或 lazy 字段，也不会让带 range 约束的字段
失去 repeat controller 资格。它不引入通用表达式约束、可由规则选择的 severity，或把 range
违规提升为致命错误的方式。它不重新讨论 `@equals` 语义，也不增加 ADR-0039 推迟的其余 SPS
可选语法。

## 后续

ADR-0075 随后把同一非致命合同扩展到无符号固定宽度与动态宽度 `bits` 字段。上述最初的
`ue` 范围记录的是首个切片的边界，而不是语言能力的最终范围。

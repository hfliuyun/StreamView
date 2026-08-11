# 将非致命 range 扩展到无符号 bit 字段

状态：已接受
日期：2026-08-11

## 背景

ADR-0040 为 `ue` 字段引入了非致命 `@range(minimum, maximum)` 校验。语义值违规与布局
失败之间的同一区分也适用于固定宽度 syntax field。

H.264 clause 7.4.3 要求 IDR picture 的 `frame_num` 等于零。内置规则以
`log2_max_frame_num_minus4 + 4` 的动态宽度读取该字段，但目前接受该宽度可表示的所有值。
非零值不符合规范，却不会改变字段宽度，也不会移动 `field_pic_flag`、`idr_pic_id`、
picture-order syntax、IDR marking、QP 或 opaque slice payload。

既有带 source anchor 的 `assert` statement 无法表示该合同：它失败时会致命停止结构。为此
增加一次性的 warning assertion 又会复制已经稳定的 range diagnostic 与 bytecode 行为。

## 决策

把 `@range(minimum, maximum)` 从 `ue` 字段扩展到无符号固定宽度和动态宽度 `bits` 字段。
带 enum 的固定宽度 `bits` 字段也可以使用同一注解；致命 enum membership 与 `@equals`
检查仍先于非致命 range 检查执行。

Parser 与 compiler 保留既有的参数数量、唯一性和边界顺序规则。对固定宽度 `bits<N>` 字段，
maximum 必须能由 `N` bit 表示。动态宽度字段的运行期宽度无法静态得知，因此接受完整 unsigned
64-bit 注解值域；既有运行期宽度合同仍要求 `1..64`，解码值也必然能由所选宽度表示。既有
`ue` maximum 仍为 `2^64 - 2`。

本决策不增加 typed-IR descriptor 或 opcode。两种无符号 bit encoding 复用
`DslTypedUnsignedRange`、`assert-range-minimum` 与 `assert-range-maximum`。VM 校验每条
instruction 都紧随匹配的已物化字段与 bound。违规时在字段上附加 severity 为 warning、带
source location 的 `invalid-syntax` diagnostic，保留完整字段 span，使外层结构保持
materialized，并继续后续字段。

为 `IdrSliceLayerWithoutPartitioningRbsp.frame_num` 增加 `@range(0, 0)`，并引用 ITU-T
H.264 clause 7.3.3 与 7.4.3。Package `0.1.27` 保持 coverage depth
`picture-order-count-slice-header`。

## 影响

内置 profile 会报告非零 IDR frame number，但不会隐藏 header 的剩余部分。回归覆盖固定与
动态 bit 字段、malformed typed IR、合法值零、首个非法值一、完整动态宽度 warning span、
未移动的后续字段与 payload 位置，以及继续扫描下一个 NAL。

无符号固定宽度 semantic domain 可以复用同一注解，无需发明 format-specific warning
statement。DSL reference 必须改为说明按类型区分的静态 maximum，而不能再把 `@range`
描述成 `ue` 专用注解。

## 非目标

本决策不把 `@range` 扩展到 `se`、computed、lazy、compressed payload 或生成的 trailing-bit
字段。它不增加 expression-valued 或 signed bound、通用 warning assertion、由规则选择的
severity 或自定义 diagnostic message。它不校验 `first_mb_in_slice`、signed QP 或 deblocking
值域，也不增加派生 picture order、decoded-picture-buffer state、MMCO-5 或 output order。

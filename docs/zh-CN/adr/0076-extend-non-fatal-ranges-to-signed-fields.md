# 将非致命 range 扩展到有符号字段

状态：已接受
日期：2026-08-14

## 背景

ADR-0040 为 `ue` 字段引入了非致命 `@range(minimum, maximum)` 校验，ADR-0075 又把它扩展到
无符号固定宽度和动态宽度 `bits` 字段。这两个决策都明确排除了 `se` 字段与 signed bound。

H.264 clause 7.4.3 要求 `slice_alpha_c0_offset_div2` 与 `slice_beta_offset_div2` 位于
`-6..6`，因为两者各自加倍后构成 `FilterOffsetA` 与 `FilterOffsetB` 这两个 deblocking
offset。内置规则以 `se` 读取它们，目前接受 signed Exp-Golomb 映射可表示的所有值。越界
offset 不符合规范，但完整码字仍然决定后续字段与 opaque `slice_data` payload 的起点。

注解语法无法表达这种边界。注解参数只接受裸整数字面量，因此前导 `-` 是 parse error，而不是
负的 bound。既有带 source anchor 的 `assert` statement 是致命的，会停止结构；为此增加一次性
的 warning assertion 又会复制已经稳定的 range diagnostic 与 bytecode 行为。

## 决策

把 `@range(minimum, maximum)` 从无符号 encoding 扩展到 `se` 字段。

注解参数接受整数字面量前的可选前导 `-`。注解值保存无符号 magnitude 与独立的 negative 标记，
而不是有符号整数，因为无符号 `@range` 的 bound 仍必须覆盖完整的 `ue` 值域直到 `2^64 - 2`，
而任何有符号 64-bit 类型都无法容纳该值。负的 bound 在 `bits` 与 `ue` 字段上仍然非法。

signed bound 的静态值域为 `-(2^63 - 1)` 到 `2^63 - 1`。该区间对称，因为 signed Exp-Golomb
映射由至多 `2^64 - 2` 的 code number 经 `magnitude = (codeNumber + 1) / 2` 导出，任一符号都
无法取到 `-2^63`。Parser 与 compiler 保留既有的参数数量、唯一性和边界顺序规则，并在有符号
字段上以有符号语义比较 bound。

本决策不增加 opcode。有符号字段携带 `DslTypedSignedRange` descriptor，并复用
`assert-range-minimum` 与 `assert-range-maximum`，其 immediate 保存 bound 的补码位模式。VM
依据字段 encoding 选择有符号或无符号比较，其 bytecode preflight 要求 descriptor 与该 encoding
匹配：有符号字段上的无符号 constraint、无符号字段上的有符号 constraint，或同时存在两种
constraint，都属于 malformed typed IR，不消耗 bit、不移动 reader，也不创建节点。违规时在字段
上附加 severity 为 warning、带 source location 的 `invalid-syntax` diagnostic，保留完整字段
span，使外层结构保持 materialized，并继续后续字段。

在 IDR 与非 IDR slice header 中，为 `slice_alpha_c0_offset_div2` 与 `slice_beta_offset_div2`
增加 `@range(-6, 6)`，并引用 ITU-T H.264 clause 7.3.3 与 7.4.3。Package `0.1.28` 保持
coverage depth `picture-order-count-slice-header`。

## 影响

内置 profile 会报告越界 deblocking offset，但不会隐藏 header 的剩余部分或 opaque payload。
回归覆盖负注解字面量、合法端点 `-6` 与 `6`、首个非法值 `-7` 与 `7`、两个 offset 各自的违规、
未移动的后续字段与 payload 位置、每一种被拒绝 descriptor 配对的 malformed typed IR，以及继续
扫描下一个 NAL。

有符号 semantic domain 现在可以复用同一注解，无需发明 format-specific warning statement。DSL
reference 必须把 `@range` 描述为同时覆盖有符号与无符号 encoding，并在既有按类型区分的无符号
maximum 之外说明有符号静态值域。

## 非目标

本决策不把 `@range` 扩展到 computed、lazy、compressed payload 或生成的 trailing-bit 字段。它
不增加 expression-valued bound、通用 warning assertion、由规则选择的 severity 或自定义
diagnostic message。它不约束 `slice_qp_delta`——其合规值域取决于生效的 SPS 与 PPS——也不校验
`first_mb_in_slice`、派生 picture order、decoded-picture-buffer state、MMCO-5 或 output order。

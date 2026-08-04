# 使用 imported context value 保护字段

状态：已接受
日期：2026-08-04

## 背景

ADR-0046 允许 dynamic bit width 从 exact imported context closure 读取一个 scalar。剩余的
有界 H.264 IDR slice-header 语法还依赖所选 PPS 导出的 scalar flag：只有 PPS 启用时才存在
bottom-field picture-order count；只有 PPS 声明该能力时才存在 redundant-picture 语法。要把这些
决定留在 rule 中，就必须在读取相关字段前取得 imported value。

既有 conditional language 会把两个 branch 投影到 guarded linear field stream。只给该模型增加
一个静态描述的 imported scalar，不需要一般 imported expression、object model 或 control-flow
jump。

## 决策

新增一种受支持的条件形式：

```cpp
if (context_value(pic_parameter_set_id,
                  h264_pps,
                  bottom_field_pic_order_in_frame_present_flag) == 1) {
    se delta_pic_order_cnt_bottom;
}
```

左侧必须精确为 `context_value(import_key, context_kind, exported_field)`，operator 必须是
`==`，右侧必须是一个无符号整数字面量。imported value 类型为 `u64`，因此接受完整 `u64`
literal 范围。拒绝 arithmetic、Boolean combination、negation、`!=`、ordering、包围 imported
value 的 call，以及 imported Boolean shorthand。既有 field equality 和 `computed<bool>` 简写
condition 保持原行为。

import key、可达 target kind、唯一 publishing structure 与有序 export 使用 ADR-0046 的同一
静态合同解析。compiler 把左侧 lower 为 typed imported-context descriptor，并把它、expected
literal 以及 positive/negated sense 附加到每个 projected branch field。descriptor identity 包含
import ordinal、target kind、publishing structure 与 export ordinal，因此引用不同 export 的
nested guard 不会混同。不新增 conditional opcode、jump 或 runtime field-name lookup。

VM 在读取 source 前验证每个 imported guard：它必须是无 operand 的 canonical `u64` leaf，使用
equality operator，并具有完整 exact import/publisher/export descriptor。即使 branch 运行时不会
被选择，malformed descriptor 仍属于 invalid typed definition。

到达 field guard 时，VM 使用既有 execution-context resolver 请求 scalar。
`RuleExecutionSession` 按 ADR-0045 与 ADR-0046 解析并缓存同一个 exact generation closure。guard
匹配时选择该字段；不匹配时跳过字段，不读取 source、不创建 analysis node，也不执行 field
constraint。missing、future 或 stale generation 仍为 `dependency-unavailable`，diagnostic 锚定到
已经物化的 import-key field；schema 或 payload mismatch 仍为 invalid runtime definition。执行
成功时返回同一 cached imported closure，imported value 不创建 presentation node。

## 影响

rule 可以声明 imported layout-presence flag，同时 analyzer 保持 format-neutral。imported guard
会与既有 field、switch 和 repeat guard 组合，因为它们都 lower 到同一有序 projection model。
只有选中的 field 消费 bit，后续语法因此紧接在此前最后一个选中字段的精确末尾。

## 非目标

本决策不允许 imported value 出现在 computed field、lazy byte count、array length、enum 或
annotation argument、payload dispatch、switch controller、repeat controller/bound、sentinel
termination 或一般 expression 中。不新增 `!=`、ordering、Boolean combination、imported Boolean
shorthand、任意 member access 或 context-generation fallback selection。

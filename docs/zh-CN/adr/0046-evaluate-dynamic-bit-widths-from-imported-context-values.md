# 使用 imported context value 计算 dynamic bit width

Status: Accepted
Date: 2026-08-04

## 背景

ADR-0045 会在 consumer structure materialize 后返回精确的 rules-owned context payload。
有界 H.264 slice header 需要使用所选 SPS generation 导出的
`log2_max_frame_num_minus4`，以 `value + 4` 作为 `frame_num` 宽度。VM 必须在 consumer
完成前取得这个标量，同时不能把 directory policy 或 H.264 field knowledge 放进 Annex B
analyzer。

## 决策

big-endian `bits` field 的宽度可以使用非 literal 的 unsigned arithmetic expression。
context export 只能通过保留形式
`context_value(import_key, context_kind, exported_field)` 引用：

```cpp
@context_import("h264-pps", pic_parameter_set_id)
struct SliceHeader {
    ue first_mb_in_slice;
    ue slice_type;
    ue pic_parameter_set_id;
    bits<context_value(pic_parameter_set_id,
                       h264_sps,
                       log2_max_frame_num_minus4) + 4> frame_num;
}
```

kind identifier 只接受 `h264_sps`、`h264_pps`、`aac_asc` 或
`iso_bmff_sample_description`。import key 必须是此前 field，并且在 structure 上唯一标识一个
`@context_import`。target kind 可以指向 imported root 或其 exact dependency closure 中的
definition。compiler 要求该 kind 只有一个 publishing structure，且 requested field 带
`@context_export`；lowering 只保存 root import index、target kind、publishing structure index
与 ordered export index，runtime 不比较 field name。

首个切片只允许在 dynamic `bits` width 中使用 `context_value`。width 类型必须为 `u64`，沿用
既有 checked arithmetic、pure-call inline 与 expression limit，只接受 big-endian。dynamic field
不能是 fixed array、enum、context key/dependency/import/export；它之后的 exact static offset
未知。literal `bits<N>` 保持原 typed IR 与行为。

VM 在 source read 前校验全部 dynamic-width expression 与 imported-reference descriptor，继续
使用既有 `read-unsigned-bits` opcode。field 被选择后，VM 向 execution context 请求目标 scalar，
求值后读取精确宽度。结果不在 `1..64` 或 checked arithmetic 失败时得到 `invalid-syntax`，且该
field 不消费 bit。truncation、mapping、cancellation、instruction/node budget 与 partial result
语义不变。

`RuleExecutionSession` 是唯一 production resolver。首次请求 value 时，它在 consumer enclosing
span 起点按 ADR-0045 解析 root import，并为本次 run 缓存 exact closure，再按静态 descriptor
选择 target structure 与 export ordinal。missing/future/stale 仍返回
`dependency-unavailable`，不回退；target 缺失或歧义、schema mismatch、payload 缺失属于 invalid
runtime definition。成功 run 仍返回 ADR-0045 的 exact imported closure，imported value 不创建
presentation node。

## 后果

rule 可以声明首个 layout-critical H.264 slice-header width，同时 analyzer 保持 format-neutral。
compiler 负责 schema name，session 负责 generation selection 与 payload identity，VM 只看到有界
scalar resolver 和稳定 typed descriptor。

## 非目标

本决策不把 imported value 加入 computed field、condition、lazy size 或 repeat bound；不新增
dynamic little-endian field/array、sentinel loop、compressed remaining-bit payload 或 H.264 slice
dispatch。

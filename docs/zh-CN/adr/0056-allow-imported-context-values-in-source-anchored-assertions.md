# 允许带源锚点的断言引用 imported context value

状态：已接受
日期：2026-08-07

## 背景

ADR-0054 新增了带源锚点的 assertion，其 condition 可以关联本地已解码或 computed scalar。
ADR-0046 与 ADR-0052 又分别允许 dynamic bit width 和受限 equality guard 从 exact imported
context generation 读取一个 scalar，但这些形式仍不能表达同时依赖本地语法与 PPS/SPS export
的顶层 conformance prerequisite。

下一个有界 H.264 non-IDR P-slice 增量只有在所选 PPS 禁用 weighted prediction 与 entropy
coding 时，才能安全省略 `pred_weight_table()` 和 `cabac_init_idc`。assertion 仍然只能是
无条件顶层 statement，因此 rule 需要在读取下一字段前表达
`!is_p_slice || context_value(...) == 0` 这样的短路条件。把这些检查移入 C++ analyzer 会破坏
format logic 由 rule 持有的边界。

## 决策

允许 source-anchored assertion 的 Boolean condition 使用保留 expression leaf
`context_value(import_key, context_kind, exported_field)`：

```cpp
assert(!is_p_slice ||
       context_value(pic_parameter_set_id,
                     h264_pps,
                     weighted_pred_flag) == 0)
    at pic_parameter_set_id;
```

该 call 必须恰好包含三个 identifier argument，静态类型为 `u64`。import key、可达 target
kind、唯一 publishing structure 与 named export 沿用 ADR-0046 的 exact static resolution
合同。该 leaf 可以参与 assertion 既有的 checked arithmetic、comparison、Boolean 与 pure-call
expression 合同，但不会隐式转换为 `bool`；完整 assertion condition 仍必须为 `bool`。既有
depth、node、pure inline 与 expansion-work 上限保持不变。

parser 只在校验 assertion condition 时识别这一保留 call。pure-function body、computed field、
lazy byte count 与其他一般 expression position 继续拒绝直接写入的 `context_value`。assertion
可以把 imported `u64` 作为 pure function argument，因为该 call 只会被静态内联，不会让
pure function 自身获得 context access。

compiler 只在 lower assertion condition 期间启用既有 exact imported-context resolver。结果仍是
canonical `ImportedContextReference` typed-expression leaf，保存 import ordinal、target definition
kind、publishing structure index 与 export ordinal。assertion descriptor、statement position、
source anchor 与 `AssertExpression` bytecode instruction 均不改变。

VM 在读取 source 前，使用其他 imported expression 的同一 publisher、export、reachability、
type 与 operand 合同校验 assertion condition 中的 descriptor。执行时，既有 context-value
resolver 会选择并缓存 exact generation closure。Boolean short-circuit 语义保持可观察：未选中的
operand 内的 imported leaf 不会触发 resolution。

如果 imported value 已成功解析而完整 condition 为 false，执行以
`Assertion condition is false` 返回致命 `invalid-syntax`，diagnostic path 与 mapped source
range 使用 assertion 的 `at` 字段。如果失败发生在 context resolution 本身，则保留 resolver
既有 status 与 message，并把 diagnostic 定位到已物化的 import-key field。missing、future 或
stale generation 因此仍为 `dependency-unavailable`；malformed context payload 或 descriptor
仍为 invalid definition。成功 assertion 仍不读取 source bit、不移动 cursor，也不创建
presentation node。

回归覆盖三个 identifier 的 parser 合同、assertion 外继续拒绝、typed imported descriptor 与
positioned bytecode、source access 前的 malformed descriptor preflight、exact generation 的
通过与失败、short-circuit 行为，以及 missing-generation diagnostic。

## 影响

rule 可以把本地语法与 exact imported parameter-set value 组合成带源定位的 conformance
prerequisite，同时 analyzer 保持 format-neutral。该扩展复用既有 bounded expression、context
generation、typed IR、bytecode 与 runtime resolver 模型，不新增第二套 assertion 或 context
机制。

diagnostic 的区别保持明确：conformance rule 违反时定位到 rule author 指定的 anchor；dependency
不可用时定位到 consumer 编码中的 import key。

## 非目标

本决策不允许 imported value 出现在 pure-function body、computed field、lazy byte count、
switch/repeat controller、repeat bound、sentinel termination、payload dispatch、annotation、
array length，或 ADR-0052 之外的一般 conditional expression 中；不新增 imported Boolean
隐式转换、fallback generation selection、conditional/repeated assertion、warning assertion、
custom message 或新的 context kind。

本决策本身不新增 H.264 P-slice、weighted-prediction table、`cabac_init_idc`、reference-list
modification loop、reference-picture marking 或 entropy-coded slice-data decoding。

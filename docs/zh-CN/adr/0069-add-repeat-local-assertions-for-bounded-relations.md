# 为有界关系新增 repeat-local assertion

状态：已接受
日期：2026-08-10

## 背景

有界 H.264 reference-picture marking loop 已经可以解码所有 operand 形态，但
clause 7.4.3.3 仍有作用于每个 operation 的关系。现有
`assert(condition) at anchor;` 故意只允许顶层。把 assertion 放在 sentinel repeat
之后又无法引用 repeat projection：projection 会展开成带索引的字段，而且某个分支没有
物化的值不能作为普通 dependency。

如果把检查移入 H.264 analyzer，正式规则的 conformance 行为就会变成格式专用 C++，也会
失去 DSL 的 source-anchored diagnostic 合同。

## 决策

允许 `assert(condition) at anchor;` 出现在 bounded 或 sentinel repeat 内部，包括其
conditional 与 switch body。这样的 assertion 称为 **repeat-local assertion**。compiler
会为每个静态 projection iteration 各展开一条，因此 condition 只能引用同一个 iteration
中更早声明的 scalar 字段，以及当前 branch 保证存在的值。anchor 也是该 iteration 中更早的
source-backed scalar 字段。

顶层或 repeat-local scope 之外的 assertion 仍然非法。它不新增 field 或 presentation node。
typed descriptor 会同时保存 active field conditions、condition 与 anchor。VM 在执行
assertion 前先评估这些 conditions；branch 未选中时跳过 assertion，branch 选中时在不读
source、不移动 cursor 的情况下求值 Boolean condition。条件为 false 时以现有
`Assertion condition is false` 消息返回致命 `invalid-syntax`，diagnostic 使用当前
iteration 的完整 anchor range。checked expression failure 保留现有 status 与 diagnostic
行为。

descriptor 与 bytecode preflight 继续校验 declaration order、descriptor 数量、field position、
operand/immediate，以及 condition/anchor dependency 关系。assertion 仍计入每个 structure
最多 1,024 条与 instruction budget；被跳过的 assertion 仍是 cancellation point。

正式 H.264 规则使用该能力，对 imported SPS context 中可用的值执行有界检查。本增量不跟踪
decoded-picture buffer，不建模 operation 顺序，不检测重复或矛盾 operation，也不声称完成
完整 clause 7.4.3.3 conformance。

## 影响

规则可以表达作用于 bounded projection 的 source-anchored relation，同时 VM 保持 format-neutral。
operation 失败会保留已物化前缀，Annex B analyzer 仍可继续分析后续 NAL。字段命名空间继续
扁平；repeat projection 保留带索引的呈现名，不新增 runtime array value。

## 非目标

本决策不新增 repeat 后 aggregate expression、可变 rule state、array indexing、warning assertion、
custom assertion message 或 DPB simulation。跨 iteration 与跨 NAL 的关系仍延期。

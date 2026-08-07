# 新增带源锚点的断言语句

状态：已接受
日期：2026-08-06

## 背景

DSL 可以用 `@equals` 让一个已解码字段与常量比较，也可以用 `@range` 报告非致命的 `ue`
value-domain 违规，但不能在不新增 synthetic computed field、也不把格式专用 conformance
逻辑移入 C++ analyzer 的情况下，表达两个已解码字段之间的致命关系。

H.264 clause 7.4.1 在 direct NAL header 中暴露了这个缺口。type-5 IDR NAL 不允许
`nal_ref_idc == 0`，但 payload dispatch 只根据 `nal_unit_type` 选择。该 prerequisite 必须在完整
八 bit header 解码后、当前 NAL payload mapping 前执行；失败 diagnostic 必须指向编码中的两 bit
`nal_ref_idc` 字段，同时后续 NAL 仍须可恢复扫描。

## 决策

新增不接受 annotation 的 structure statement：

```cpp
assert(boolean_expression) at source_field;
```

assertion 只允许作为无条件顶层 structure item。condition 使用既有 bounded expression 与
pure-function 合同，结果必须为 `bool`，并且只能引用此前声明、当前路径保证存在的 scalar
unsigned 或 computed value。imported-context leaf、array、signed Exp-Golomb value、未知/未来
字段与不可用的 branch-local value 都不能作为 condition dependency。

anchor 必须是此前声明、当前路径保证存在且 source-backed 的非数组 scalar syntax field。
fixed/dynamic `bits`、enum、`ue` 和 `se` 都可以作为 anchor，尽管 `se` 不是 expression value；
computed 或 generated field 与 region item 不能作为 anchor。一个 structure 最多包含 1,024 条 assertion。

compiler 把每条 statement 记录为 declaration-order `DslTypedAssertion`，其中包含 typed Boolean
condition、anchor field index、statement-position field index 与 source range；它不新增 typed
field 或 presentation node。每个 descriptor 生成一条 `AssertExpression` instruction，operand 为
descriptor index，immediate 为零。同一 field position 的稳定顺序是先完成 sentinel assertion，
再执行 expression assertion、repeat-count assertion，最后读取下一字段。对于包含 expression
assertion 的 structure，VM 会在 source access 前验证 descriptor 数量、position、operand、
immediate 与该顺序。

执行该 instruction 时，condition 求值不读取 source、不移动 reader，也不创建 node。`true` 继续；
`false` 以 `Assertion condition is false` 消息返回致命 `invalid-syntax`，保留 materialized prefix、
停止后续字段，并使用 anchor 的完整 mapped range 形成 diagnostic path 与 source span。checked
expression failure 使用同一 anchor 以及既有 runtime status/message。该 instruction 计入
instruction budget，并且仍是 cancellation point。

official H.264 rule 声明：

```cpp
assert(nal_unit_type != 5 || nal_ref_idc != 0) at nal_ref_idc;
```

package version `0.1.11` 发布这一 additive rule 变化，coverage depth 保持
`idr-slice-header`。回归覆盖 parser/compiler 拒绝、1,024/1,025 边界、确定性的 positioned
bytecode、malformed typed-IR preflight、multi-span anchor location、instruction/cancellation
边界、type-5 zero-priority 在 payload mapping 前失败，以及后续合法 NAL 继续扫描。

## 影响

rule 可以拥有 relational conformance prerequisite，无需 analyzer 专用分支或 synthetic
presentation field。assertion diagnostic 始终关联真实编码语法，包括互相分离的 mapped source
span；成功 assertion 不增加 source read、cursor movement 或 analysis node。

direct-header assertion 失败只阻止当前 NAL 的 payload mapping。Annex B analyzer 保留其完整
header 作为 partial result，并从下一条 scanner record 继续。

## 非目标

本决策不新增 warning assertion、custom message、assertion annotation、assertion array、
conditional/repeated assertion、imported context expression、assertion value 或 context export；
也不让 `se` 成为 expression value，不声称在 type-5 `nal_ref_idc` prerequisite 之外完成全部
H.264 NAL-header conformance。

本决策也不扩展 non-IDR、P/B/SP/SI、field-picture、reference-list、weighted-prediction、
adaptive-memory-management、slice-group 或 entropy-coded slice-data 支持。

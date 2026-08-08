# 在被派发 payload 中读取 sequence element 字段

状态：已接受
日期：2026-08-08

## 背景

ADR-0062 完成了有界 reference-list modification 语法。bundled profile 的下一个增量是
`dec_ref_pic_marking()`：clause 7.3.3 规定只要 `nal_ref_idc` 非零就读取它。该字段属于
`NalUnitHeader`，不属于 slice structure，而被派发的 payload structure 目前没有任何途径
读到它。

已考察三种现有机制，全都无法表达该依赖。

payload request 不携带 header 值。`RuleExecutionRequest` 只包含 source、structure
index、mapping、tree、parent、enclosing span 与 options；虚拟机随后按正在执行的
structure 单独建立环境，因此每个槽位初始都是未设置。

context 机制按 source position 解析，会选到错误的 generation。payload 的查询位置是
enclosing NAL unit 的起点，而一个 definition 只在其 exclusive end 处才变得可选，因此
slice 导入由 header 发布的 generation 时，拿到的是**上一个** NAL unit 的 header。
context kind 也是四个 parameter-set kind 的闭集。

dispatch controller 同样不可见。它针对 sequence element structure 解析，由 runner 消费
以选择 case，从不进入 payload structure 的环境，而且该 dispatch 有意不新增 opcode。

ADR-0037 拒绝把 dispatch 放进 `NalUnitHeader`，部分理由正是那样“也会把 header 自己的
字段与 payload 的字段混为一谈”。该理由依然成立：修复方案不得合并这两个命名空间。

## 决策

新增受限 expression leaf `header_value(field_name)`，用于在被派发的 payload structure
内部读取 sequence element structure 的字段。它有意镜像既有的 `context_value(...)` leaf，
而不是引入第二种外部引用风格。

```cpp
if (header_value(nal_ref_idc) != 0) {
    bits<1> adaptive_ref_pic_marking_mode_flag;
}
```

该语法接受且只接受一个 identifier 实参。实参个数不符或出现非 identifier 实参都是静态
错误，与 `context_value` 既有的 arity 检查一致。

被引用字段在编译期针对程序的 sequence element structure 解析。该语言最多允许一个顶层
payload dispatch 和一个 scan，因此 element structure 在整个程序范围内唯一确定；解析不
依赖“外层 structure 本身是否是某个 case target”这一信息——编译某个 structure body 时
编译器尚未确定这一点。被命名字段必须存在于该 structure 中，且必须是无条件、顶层、
非数组的 unsigned scalar，因此可达面恰好等于 context key 或 dispatch controller 已经要求
的那个面。被 guard 的、位于 repeat 中的、数组、dynamic-width 或 signed element 字段
一律拒绝。

没有声明 scan 的程序，或者外层 structure 本身**就是** sequence element structure 的
情况，都是静态错误。后者否则会让一个 structure 在自己的字段仍未设置时，通过外部通道
读取自身。

该 leaf 类型为 `u64`，只在 imported-context leaf 已被允许的位置被允许，并计入同一套
expression node 与 depth 预算。它在 pure-function body、lazy byte count 以及任何已经
拒绝 `context_value` 的位置同样不被允许。

typed IR 新增 `SequenceElementReference` expression kind，携带已解析的 element field
index。compiler 不新增 opcode，与 imported-context leaf 复用既有 expression 求值的方式
完全一致。

运行时由 analyzer 在 execution request 上提供已解码的 element 字段值，虚拟机按解析出的
index 从该向量读取。这些值在 payload 运行前即已发布，因为 runner 本来就会先物化
header，并从中读出 controller 来选择 case。虚拟机在读取 source 之前验证 descriptor：
index 越界、值缺失或整个值向量缺席都会作为 invalid definition 失败，而不是用猜测值继续
解码。

header 与 payload 的命名空间保持分离。`header_value` 是 call 而不是 identifier，因此任何
element 字段名都不会遮蔽 payload 字段名、也不会被其遮蔽；阅读 payload structure 的规则
作者可以在语法层面看到全部外部依赖。

回归覆盖 parser 的 arity 与 identifier 规则、element 字段的静态约束、自引用与缺少 scan
的拒绝、typed IR 降低且不新增 opcode、malformed descriptor 的 VM 预验证、false guard
不消费 source，以及被派发 payload 依据 header 字段分支的端到端 session 用例。

## 影响

payload structure 现在可以依赖它自己所属 NAL unit 的 header，这为
`dec_ref_pic_marking()` 以及后续任何“presence 取决于 header 而非 parameter set”的语法
解除了阻塞。bundled rule 仍需要一个独立增量来使用它；本决策只新增该能力。

该机制比通用跨 structure 读取窄得多。它只能到达 sequence element structure，只能读取
无条件顶层 unsigned scalar，并且只能从被派发的 payload 发起。将来若需要读取别的
structure 的字段，需要单独决策，而不是拓宽本决策。

由于该 leaf 在程序范围内针对 element structure 解析，写在一个从不被派发为 payload 的
structure 里的 `header_value` 调用能够编译通过，但在没有提供值向量时会在执行期以
invalid definition 失败。另一种做法是把解析推迟到 dispatch 已知之后，那需要对 structure
body 做第二遍编译，代价不成比例，故被拒绝。

## 非目标

本决策不新增通用跨 structure 字段访问，不允许一个 payload 读取另一个 payload 的字段，
不把 dispatch controller 暴露为 identifier，不新增 view 或 call opcode，不改变 payload
dispatch 的选择方式，不拓宽 context kind 闭集，也不改变 context generation 的解析方式。
它不新增 `dec_ref_pic_marking()`、不解除 type-1 的 `nal_ref_idc == 0` 前置约束，也不改动
任何 bundled rule 语法；这些留待下一个增量。

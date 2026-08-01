# 增加有界的 RBSP trailing-bits 终结项

状态：已接受
日期：2026-08-01

## 背景

ADR-0037 会把选中的 NAL payload 派发到 mapped RBSP view 上的结构，并要求被选中的
结构精确消费该 view。首版 access unit delimiter 规则把 `rbsp_trailing_bits()` 写成固定的
`rbsp_stop_one_bit` 字段和四个 `rbsp_alignment_zero_bit` 数组元素。这一声明之所以正确，
仅仅是因为它前面的 `primary_pic_type` 固定为三 bit。

下一个正式格式切片是 H.264 sequence parameter set。它在数量可变的语法元素之后以
`rbsp_trailing_bits()` 结束。补零 bit 的数量取决于当前 RBSP bit 位置，因此稳定 DSL 的固定
数组、有界 repeat 与表达式无法表达它；若要表达，只能把 H.264 解析知识写入 analyzer，或
接受未被完整消费的 RBSP。两者都违反实施计划和 ADR-0037。

## 决策

DSL 增加一个没有 annotation 的终结结构项：

```cpp
rbsp_trailing_bits;
```

它是上下文标识符，不是保留字。它在一个结构中恰好可以出现一次，只能位于该结构顶层，且
必须是最后一项。它不能出现于 conditional、switch 或 repeat body 内部。仅含这一项的结构
不是空结构。

该项读取并发布一个命名为 `rbsp_stop_one_bit`、约束为 `1` 的字段；然后读取零至七个分别命名
为 `rbsp_alignment_zero_bit[i]`、约束为 `0` 的字段，并在到达下一个 logical byte 边界时停止。
stop bit 与每个实际读取的 alignment bit 都保留自己的 source location，包括跨越
emulation-prevention exclusion 时的多个 source span。缺失 bit 是 `truncated-source`；stop bit
为零或 alignment bit 非零时，在该字段上报告 `invalid-syntax`。与所有结构相同，payload 的
精确消费仍由 runner 负责，并会捕获任何残余 RBSP bit。

只要使用该终结项，compiler 就预留八个 typed field slot：一个 stop bit 与七个可能的 alignment
bit。bytecode 包含一条 `read-rbsp-trailing-bits` 指令。执行期间，没有用到的 alignment slot
会标记为 skipped，使普通的 end-of-structure accounting 继续保持严格。由此得到固定上界：至多
八次读取、八个生成字段节点，以及一个 VM instruction/cancellation point。这一预留计入既有的
每结构 99,999 field 与每次 materialization 100,000 node 限制，即使读取最大 padding 时也保持
sandbox 契约。

内置 H.264 access unit delimiter 规则迁移到该终结项。这取代 ADR-0037 认可的固定数组；该 ADR
中其余 payload-dispatch 决策仍然有效。language compatibility string 保持 `0.1`，因为这是
一个有界 v0.1 syntax element 的追加实现。官方 H.264 package 的规则源码发生变化，因此自身
进行 patch version 变更。

## 影响

正式 H.264 规则能够忠实声明 `rbsp_trailing_bits()`，不需要通用 expression evaluator、动态且
无界的 loop、analyzer special case 或 opaque suffix。它的最大资源使用量与输入大小无关，并且
在执行前的 typed IR 中可见。

生成字段刻意保持可见，而非合并成一个 aggregate node。这样保留 AUD 规则已经建立的逐语法元素
source inspection 和 diagnostic 行为。未用到的可能 alignment field 不出现在 materialized tree
中。

终结项不能组合，也不能后跟另一项。将来若某种格式需要另一个依赖位置的 primitive，必须单独做
有界 DSL 决策，而不是悄悄扩张本原语。

## 考虑过的方案

- 继续在每条规则中声明固定数组：只在先前 bit 数恒定时精确，无法描述 SPS，而且容易被错误复制。
- 在 Annex B analyzer 中解析 trailing bits：会把正式格式知识移出官方规则，并绕过 compiler/VM
  budget。
- 允许已解析前缀之后有 opaque suffix：违反「精确消费」契约，后者使被派发的 payload 成为已验证的
  声明。
- 增加通用 alignment expression 与 repeat：这是比 H.264 当前需求更宽的语言能力，静态与运行时
  验证面也更大。

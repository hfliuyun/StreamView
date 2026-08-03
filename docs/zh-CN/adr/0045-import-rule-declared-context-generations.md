# 导入规则声明的 context generation

Status: Accepted
Date: 2026-08-04

## 背景

ADR-0044 通过 rules-owned `RuleExecutionSession` 发布 SPS 与 PPS generation，
但下一步 H.264 slice-header rule 必须在 consumer position 选择这些 generation。这个选择
不能写入 Annex B analyzer，也不能从 presentation node 回读 value。

首个 import 切片只需要稳定的选择与 payload 交接；dynamic bit width、使用 imported value
的 expression 和 slice dispatch 留到后续语言切片。

## 决策

新增可重复的 structure annotation：

```cpp
@context_import("h264-pps", pic_parameter_set_id)
struct SliceHeader {
    ue first_mb_in_slice;
    ue pic_parameter_set_id;
}
```

annotation 接受一个已识别的 context kind，以及同一 structure 中无条件、顶层、非数组的
unsigned scalar key field。一个 structure 最多声明 16 个 import。compiler 将每个 import
降低为稳定 typed-field key index，并保留 declaration order；重复 kind/key pair 会被拒绝。

VM 在读取 source 前验证 import metadata。字段成功执行后，它只返回每个被选择的 key value、
精确 location 与 import descriptor，不暴露完整 local environment。

`RuleExecutionSession` 在 consumer enclosing source span 的起点调用
`ContextDirectory::resolveBefore` 解析每个 import。它要求 execution mapped span 都位于该
enclosing span 内；缺少或 stale dependency 不回退，并返回 `dependency-unavailable`，不发布
context generation。找到 generation 后，result 按 `ContextDefinitionId` 附加 rules-owned
exported scalar payload 及其精确 dependency closure。root 位于首位；dependency 按声明顺序
进行 depth-first traversal，每个精确 definition 只出现一次；一个 closure 最多包含 64 个
definition。每项保留 kind、publishing structure index、有序 exported value 和精确 dependency
ID。directory 中存在 generation 却找不到 payload 属于 invalid runtime definition；closure
超限得到 resource-limit result。

本切片中 imported payload 只是 result data；它还不会进入 expression namespace、改变 source
消费或创建 node。后续 dynamic `bits<expression>` 与 computed-field 工作会沿用这个 session
interface 使用有序 values。

## 后果

rule 负责 context 选择与 payload 解释。analyzer 保持格式中立，stale generation 保持为
unavailable；测试无需构造 presentation-tree value bus 就能验证精确位置和 payload identity。

## 非目标

本决策不新增 dynamic-width field、expression 中的 imported identifier、sentinel loop、compressed
slice data 或 H.264 slice dispatch。

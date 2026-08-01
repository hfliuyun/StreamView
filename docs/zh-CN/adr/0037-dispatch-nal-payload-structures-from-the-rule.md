# 由规则派发 NAL payload 结构

状态：已接受
日期：2026-08-01

## 背景

ADR-0025 已经为每个非空 NAL unit payload 派生有界的 RBSP logical view，Annex B runner
把它发布为未解释的 `rbsp_payload` region，但没有任何东西解析该 region 的内部。到目前为止
所有已接受的 DSL 声明都是「一个结构对应一个 view」，entry sequence 也只能把单一 element
structure 绑定到 start-code scan。因此还无法表达「NAL unit 携带的语法取决于 NAL unit header
刚刚产出的 `nal_unit_type` 值」。

实施计划要求正式格式只能通过 DSL 实现。把派发表放进分析核心的 C++ 里，等于把「type 9 是
access unit delimiter」这一条格式知识写在了规则之外，而这正是规则应当拥有的知识。
format-language 参考也已经明确把 typed format declaration 与 payload access 推迟到正式格式
规则阶段。

首批 payload 结构同时也是体积最小的几个。clause 7.3.2.4 把 `access_unit_delimiter_rbsp`
定义为 `primary_pic_type` 后跟 `rbsp_trailing_bits`；clause 7.3.2.5 与 7.3.2.6 把
`end_of_seq_rbsp` 与 `end_of_stream_rbsp` 定义为空。它们足以检验 payload 派发、精确消费和
空 payload 的一致性，又不需要引入 parameter set 状态、scaling list 或通用表达式求解器。

有两处既有行为挡在前面：结构必须至少声明一个字段，因此空 RBSP 无法表示为结构；派生的
RBSP view 只在 payload 非空时创建，因此只有 header 的 NAL unit 根本不会经过规则。

## 决策

DSL 新增一个顶层 payload 派发声明：

```cpp
@spec("ITU-T H.264", "7.3.1")
payload<rbsp> nal_units switch (nal_unit_type) {
    case 9:  AccessUnitDelimiterRbsp;
    case 10: empty;
    case 11: empty;
}
```

`payload` 与 `empty` 是上下文标识符，不是保留字。该声明依次给出它解码的 view kind、
它扩展的 progressive sequence、它据以分支的 controller field，以及每个已处理 controller
值对应的一个 arm。声明前可以书写 annotation，作为该派发自身的 metadata 保留。

当前唯一接受的 view kind 是 `rbsp`，即 ADR-0025 已经派生的 mapped payload view。一个程序
至多声明一个 payload 派发。

### 静态规则

被命名的 sequence 必须是已声明的 progressive scan。controller 必须命名该 scan 的 element
structure 中的字段，且必须是在该结构顶层无条件声明的无符号 scalar `bits` 字段，从而在每条
路径上都保证存在，宽度至多 64 bit。computed field、Exp-Golomb field、数组元素、lazy region，
以及位于 conditional、switch 或 repeat body 内部的字段一律拒绝作为 controller。这与等值
条件、switch 和有界 repeat 已经使用的 controller 规则一致。

case 值必须互异，并且可以用 controller 的声明宽度表示。每个 `case` 目标必须是已声明的结构
或者 `empty`。`case` 目标结构不得是 scan element structure，因为 VM 仍然没有 call stack，
也没有 view 嵌套 opcode。

不存在 `default` arm。未列出的 controller 值保持既有的未解释 payload 行为。规则因此绝不会
宣称自己了解某个并未描述的 NAL type，新增一个 type 也始终是纯追加的改动。

payload 派发要求存在指向同一 sequence 的 entry。

### Typed IR

payload 派发降低为按声明顺序排列的 case 列表，每个 case 保存 controller 值以及目标结构
索引或空标记。它不新增任何 opcode。被选中的结构由 compiler 已经为每个已声明结构生成的
`begin-structure` 到 `end-structure` bytecode 执行。controller 索引、case 互异性和目标索引
在执行前完成校验；malformed 派发属于 invalid typed definition。

### 运行时

NAL unit header 结构物化之后，runner 从已发布的 header node 读取 controller 值，并在派发中
查找。

没有匹配 case 时行为不变：非空 payload 成为未解释的 `rbsp_payload` region，只有 header 的
NAL unit 完全没有 payload node。

存在匹配 case 时，派生的 RBSP view 一定会被创建，零长度 payload 也不例外。决定 view 是否
存在的是「是否存在派发 case」，而不是 payload 长度。这正是可观察的行为变化：描述了某个
NAL type 的规则会拿到一个精确的、可能为空的 view 去解码。

`empty` case 要求 mapped RBSP 的 logical length 恰好为零。非空 payload 在 payload path 上
报告 `invalid-syntax`，同时保留完整的 `rbsp_payload` region 与 excluded region。

结构 case 从 logical 零开始在 mapped RBSP view 上执行选中的结构，父节点为 `rbsp_payload`
region node，并沿用与 header 相同的 execution option、沙箱预算和 cancellation token。

已物化的结构必须消费完整的 RBSP logical length。剩余 bit 在 payload path 上报告
`invalid-syntax`。精确消费正是让 `rbsp_trailing_bits` 可校验的原因，也阻止 runner 静默接受
任何声明都没有描述的尾随字节。

所需 bit 多于 view 容量的结构报告 `truncated-source`，view 为空时同样如此。因此只有 header
的 access unit delimiter 是截断，而只有 header 的 end of sequence 是物化。

payload 失败使该 NAL unit 变为 invalid 或 cancelled，同时保留 header、`rbsp_payload` region、
被排除的 emulation-prevention region 和 Annex B trailing zero。它绝不终止 scan，后续 NAL unit
继续分析。

extension-header NAL type 14、20、21 在 ADR-0025 下仍然没有派生 RBSP view，因此无法派发。

### 规则资产

内置 H.264 规则新增 access unit delimiter 结构与上述派发：

```cpp
@spec("ITU-T H.264", "7.3.2.4")
struct AccessUnitDelimiterRbsp {
    bits<3> primary_pic_type;
    bits<1> rbsp_stop_one_bit @equals(1);
    bits<1> rbsp_alignment_zero_bit[4] @equals(0);
}
```

trailing bits 声明为固定四元素数组，而不是一个四 bit 字段。clause 7.3.2.11 中每个元素都是
一个 `f(1)` 语法元素，数组形式因此保留了逐 bit 的 source span 与逐 bit 的约束诊断。元素个数
是静态的：`primary_pic_type` 占三 bit，stop bit 占一 bit，恰好还需要四个对齐 bit 才到达字节
边界。

## 影响

格式知识留在规则里。runner 通过读取已编译规则得知应当执行哪个结构，而不是查询 C++ 中的表。
SEI、sequence parameter set 和 picture parameter set 之后可以直接复用同一声明，无需再改
runner。

精确消费把 payload region 从容器变成了可校验的断言。声明语法未能覆盖全部 RBSP bit 的
NAL unit 现在会被报告，而不再被静默接受。

为零长度 payload 创建 view 只改变已派发 type 的发布树形状。未描述的 type 保持原有形状，
因此这些 type 的既有 session 与已缓存 materialized page 仍然可比。

没有 `default` arm 意味着未知 NAL type 绝不会被重新解释为规则恰好描述的某种东西；同时也
意味着规则目前只能通过「不写」来表达「其余一律不透明」，而这正是预期的默认行为。

该声明刻意保持顶层且唯一。它不组合、不嵌套、不基于 computed 值派发，也不能出现在结构
内部。这些都是独立的后续决策，需要 VM 已经预留但尚未实现的 call 与 view opcode。

## 考虑过的方案

- 在 Annex B runner 中保留派发表：改动最小，但把「type 9 是 access unit delimiter」放进了
  分析核心，与「正式格式只能通过 DSL 实现」的要求冲突。
- 放开空结构并为每个空 RBSP 声明各自的结构：为了表达两个确实不含语法元素的 payload，
  而放宽一条全语言范围的限制。`empty` 只在真正适用的那一处表达该意图，结构规则保持不变。
- 增加 `default` arm：会迫使规则描述每一个 controller 值，并把规则并不了解的 NAL type
  静默重新解释。
- 把派发表达为 `NalUnitHeader` 内部的 `switch`：payload 属于另一个 logical view，而当前 VM
  没有 view 或 call opcode；这样做还会把 header 自身字段与 payload 字段混为一谈。
- 接受部分消费：`rbsp_trailing_bits` 将无法校验，任何声明都没有描述的尾随字节会被当作
  一致性数据通过。
- 把 `rbsp_trailing_bits` 声明为共享结构：它需要依赖当前位置的对齐循环，现有 expression 与
  repeat 语法无法表达。固定四元素数组对该 payload 是精确的，也不假装自己是通用形式。

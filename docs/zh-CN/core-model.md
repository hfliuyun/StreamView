# 核心 Source 与 Bit 模型

状态：阶段 1 实现基线。英文 [Core Source And Bit Model](../core-model.md) 是规范性
文档，本文件提供对应中文说明；如有歧义，以英文为准。

## Bit 坐标

Source bit 地址是未修改媒体源中的从零开始绝对 bit offset。一个字节内 bit offset
0 表示最高有效位，bit offset 7 表示最低有效位。例如 `byte=2, bit=3` 对应绝对
bit offset 19。

Source span 和 logical range 都使用半开区间 `[start, start + length)`。若计算排他
结束位置时发生 64 位溢出，该区间无效。空区间可以表示边界，但 source mapping
中不能包含空 span。

Logical 地址只有与 logical view identity 一起使用才有意义；一个 view 的 range
不能通过另一个 view 的 mapping 解析。

## Source Mapping

Source mapping 使用一组有序的绝对 source span 表示一个完整 logical view，并维持：

- 每个 source span 都非空；
- span 按 source 顺序排列且不重叠；
- 相邻 span 自动合并；
- logical view 长度等于所有 source span 长度经过溢出检查后的总和。

解析 logical range 时会返回包含原 logical range 与精确 source span 的字段位置。
因此跨越被排除 source byte 的 logical range 会解析为多个 span。例如，一个 view
由 source bits `[0, 16)` 与 `[24, 40)` 组成时，logical bits `[8, 24)` 会映射到
source bits `[8, 16)` 与 `[24, 32)`。

发生溢出、span 重叠或乱序、view identity 不一致、range 超出 logical view 时，
mapping 会拒绝请求。没有精确 mapping 的值后续必须表示为 computed field，不能
伪装成有 source 位置的字段。

## 严格只读 Source

随机访问 source 接口只暴露大小、身份和 `readAt`。本地文件实现只使用 Qt 只读
模式打开媒体源，不提供写入或调整大小操作。并发随机读取只在 seek/read 周围串行
执行，不会把完整文件载入内存。

读取结果分为：

- `complete`：目标 buffer 已完整填充；
- `end-of-source`：读取到达或开始于当前 source 末尾之后；
- `error`：source 无法完成读取。

`end-of-source` 可以包含有效的部分字节数，调用方不得解释该长度之后的内容。

## Bit Reader

有界 bit reader 每次按最高有效位优先顺序读取 1 至 64 bit，位置相对于声明的
source span。读取会先检查声明边界，再只请求组成该值所需的 source 字节。

成功读取后位置按请求宽度前进；非法宽度、超出声明范围、source 意外截断和读取
错误都不会推进位置。解析器因此可以保留此前的完整字段，并在未解析字段边界附加
精确诊断。

合法示例：从 `0b10110010` 先读取 3 bit 得到 `0b101`，再读取 5 bit 得到
`0b10010`。非法示例包括请求 0 或 65 bit，以及从 8-bit 有界范围请求 12-bit 值。

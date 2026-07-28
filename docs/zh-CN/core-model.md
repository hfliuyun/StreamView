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

## 位置感知上下文目录

`ContextDirectory` 记录供后续语法引用、已经完成且有 source backing 的 definition。
封闭的 definition kind 包含 H.264 SPS/PPS、AAC AudioSpecificConfig 和 ISO BMFF
sample description。key 由 kind、host 分配的数字 scope 和由 kind 解释的 value 组成。
scope 零是独立 elementary stream 的自然 scope；container 为 context ID 可能冲突的
不同 track 使用不同稳定 scope。

每个 definition 包含非空绝对 source span、analysis-node ID、单调 definition ID 和精确
dependency-generation ID。给定 key 和查询 source position，目录选择 exclusive
source-span end 不晚于查询位置、且结束位置最大的 definition。查询仍位于
definition 内部时它不可见；到达排他结束位置时恰好可见，也永远不会影响
更早位置。

同一 key 的 definition 不能重叠，但注册不必遵循 source 顺序，因此 lazy 与
known-offset 分析仍能保持确定的位置查询。ID 按追加顺序分配、永不复用；
lookup 返回的 snapshot 在后续注册之后仍有效。

dependency 必须是在 dependent definition 开始前选中的精确 generation。lookup 会在
consumer 位置重新检查每个绑定 dependency 是否仍是当前 generation。发生重定义
时，stale dependent context 会报告 `dependency-unavailable`，不会回退或猜测。
resolution 最多访问 64 个 definition；后续交叉重定义形成的 dependency cycle 与
超过该上限的 chain 都报告 unavailable。

malformed definition 不会注册，也不会遮蔽此前的有效 generation。被拒绝的注册
不改变可见目录状态。目录不保存格式专用 payload、不读取 source，并沿用
analysis worker 单写者模型。首个消费该目录的正式格式规则会把一个目录实例
接入自己的 analysis session。持久化和 SQLite paging 属于后续独立契约；详见
[ADR-0028](../adr/0028-resolve-context-generations-by-source-position.md)。

## 严格只读 Source

随机访问 source 接口只暴露大小、身份和 `readAt`。本地文件实现只使用 Qt 只读
模式打开媒体源，不提供写入或调整大小操作。并发随机读取只在 seek/read 周围串行
执行，不会把完整文件载入内存。

读取结果分为：

- `complete`：目标 buffer 已完整填充；
- `end-of-source`：读取到达或开始于当前 source 末尾之后；
- `error`：source 无法完成读取。

`end-of-source` 可以包含有效的部分字节数，调用方不得解释该长度之后的内容。

## 分页 Source 访问

`SourcePager` 在 `RandomAccessSource` 之上提供有界分页视图。每页最多 64 KiB，
通过经过检查的 page index 定位；加载一页不会读取相邻页，也不会保留历史页缓存。
当 source 大小不是 64 KiB 的整数倍时，最后一页只包含声明的剩余字节。到达声明的
source 末尾的页会标记为 `end-of-source`，但仍属于成功的页结果。

如果 source 在 `sizeBytes()` 声明的字节返回完之前报告 `end-of-source`，该页会被
视为错误，而不是合法的短页。source 错误只保留 source 明确报告的字节，不会暴露页
buffer 中未写入的部分。超出范围的 page 是空的 end-of-source 结果；发生 page 坐标
溢出则是错误。

## 分页分析缓存

`PagedCache` 使用 SQLite WAL 把可重建的 progressive-index 与 materialized-result
数据保存为不透明 page；它不会缓存媒体 source byte，也不解释 payload 编码。调用方
以数据库路径和不透明 namespace 打开一个 thread-affine cache instance，然后按精确
page key 读取，或一次提交完整 batch。在 M5 用 source fingerprint、精确 rule-package
identity 与 content hash、schema/cache namespace 以及 payload-format version 绑定之前，
namespace 只负责分区；仅凭 path-like source identity 不得跨 session 复用。

每个 payload 为 1 至 64 KiB。一次原子提交包含 1 至 256 个互异 page key，因此最多
16 MiB；成功时所有替换一起可见，失败时一个也不可见。page 缺失是正常 cache miss。
wrong-thread 访问和非法 key 会在接触 storage 前被拒绝。

打开 cache 时会检查 QSQLITE runtime driver、WAL 配置、StreamView application ID、
schema version、必需 schema 和 SQLite 完整性。SQLite 回滚中断的 transaction；
persistent marker 让下次打开可以显式识别并清理由 process crash 遗留的 batch，清理
失败则中止打开。在 `synchronous=NORMAL` 下，这不承诺断电后仍持久。cache 只保存
已经提交的可重建 page，不恢复 live analyzer、scanner、mapper、analysis tree 或
context directory。

## Bit Reader

有界 bit reader 每次按最高有效位优先顺序读取 1 至 64 bit，位置相对于声明的
source span。读取会先检查声明边界，再只请求组成该值所需的 source 字节。

成功读取后位置按请求宽度前进；非法宽度、超出声明范围、source 意外截断和读取
错误都不会推进位置。解析器因此可以保留此前的完整字段，并在未解析字段边界附加
精确诊断。

合法示例：从 `0b10110010` 先读取 3 bit 得到 `0b101`，再读取 5 bit 得到
`0b10010`。非法示例包括请求 0 或 65 bit，以及从 8-bit 有界范围请求 12-bit 值。

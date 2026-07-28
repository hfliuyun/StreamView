# 按 Source 位置解析上下文 Generation

状态：已接受
日期：2026-07-28

## 背景

H.264 sequence/picture parameter set、AAC AudioSpecificConfig 以及 ISO BMFF
sample description 都会建立上下文，供另一个 source 位置上的语法使用。只靠 identifier
不能正确选择上下文：同一 identifier 可能在后面重定义，不同 track 会重复使用相同
sample-description index，而 lazy 或 known-offset 分析也可能不按 source 顺序发现定义。

consumer 不能绑定未来或仍在解析中的定义。它也不能在自己依赖的精确 definition 已被替换
后，悄悄继续使用旧 dependent context。例如，一个针对某一 SPS generation 验证过的 PPS，
在同 ID SPS 后续重定义后不能继续假定有效。

analysis tree 已经提供 append-only node identity 与 source location，但不能回答任意 source
位置上某个 format key 当前对应哪个 definition。若把这套策略分别写进 H.264、AAC 与 ISO
BMFF 规则，会重复边界和失效处理。

## 决策

core 暴露一个内存中的 `ContextDirectory` 类型；存在 format context 的 analysis-session
source 后续应各自持有一个实例。该类型只保存与格式无关的 identity 和 position metadata。
规则 owner 单独保存 typed SPS、PPS、ASC 或 sample description payload，并用目录返回的稳定
`ContextDefinitionId` 建立关联。session ownership 与 rule-runner plumbing 随首个消费该目录的
正式格式规则接入。

context key 包含：

- 一个封闭的 definition kind：H.264 SPS、H.264 PPS、AAC AudioSpecificConfig 或 ISO BMFF
  sample description；
- host 分配的数字 scope；
- 一个由具体 kind 解释的无符号 value。

scope 零表示独立 elementary stream 的自然全局 scope。container host 为每个 track 或等价
codec context 分配稳定的非零 scope。H.264 key 使用 parameter-set ID，sample description
使用 description index，单一 ASC 可以使用 value 零；目录本身不解释这些 value。

注册的 definition 包含非空绝对 source span、展示它的稳定 analysis node，以及零个或多个
精确 definition-generation dependency。完整 span 就是可用边界。在位置 `P` 查询某个 key
时，选择满足 `sourceSpan.endExclusive() <= P` 且结束位置最大的 definition。因此：

- 当 `P` 仍在 definition 自身 span 内时，该 definition 不可用；
- 到达其排他结束位置时，它恰好变为可选；
- 后续 definition 在自身 span 结束前不会影响此前查询；
- 查询永远不会绑定未来 definition。

同一 key 的 definition span 不能重叠；不同 key 或 scope 的 span 可以重叠。注册顺序与
source 顺序相互独立，因此 lazy 或 known-offset worker 可以在发现后面定义之后再发布前面的
定义。稳定 definition ID 按追加顺序分配、永不复用，snapshot 按值返回。

只有已经完成且有效的 definition 才会注册。malformed definition 由其 parser 报告，不在目录
中创建 tombstone；在另一个有效 generation 注册前，之前的有效 generation 仍是最新可选项。
非法 metadata、同 key span 重叠或 dependency 注册失败都不会改变可见目录状态。

dependency 绑定精确 generation。注册时，每个 dependency 必须唯一、使用另一个 key，且必须
是在新 definition source-span 起点之前选中的 generation。missing、future、stale、duplicate
和 same-key dependency 都会被拒绝。

查询会在 consumer 位置重新验证每个已绑定 dependency，最大 resolution depth 为 64 个
definition；每个 dependency 都必须仍解析到相同 definition ID。因此后续重定义会让
dependent lookup 返回 `DependencyUnavailable`，查询不会
退回所请求 key 的更旧 generation。后续交叉重定义可能形成 dependency cycle，即使每一次注册
在自身 source 位置都有效；lookup 会检测这种 cycle 并报告 unavailable，不会无界递归。

目录显式区分 registered、invalid-definition、duplicate-definition、dependency-unavailable、
found 和 not-found outcome。它不会自行修改 analysis tree；analysis worker 按当前 parser 的
recovery contract，把 unavailable lookup 转换为 `waiting-dependency` 或
`DependencyUnavailable` diagnostic。

目录沿用 analysis worker 单写者模型，不提供内部锁。一次注册或位置查询只执行有界局部工作：
key selection 使用二分，dependency traversal 有深度上限，因此不包含 cancellation loop。
durable serialization、source-fingerprint 验证、session ownership、rule-runner integration
与 SQLite paging 仍属于后续 M4/M5 或消费该目录的正式格式工作。

## 后果

H.264、AAC 与 ISO BMFF analyzer 共享同一套精确 source-position 规则。track scope 防止相同
sample-description index 相互冲突；lazy 或 progressive discovery 也不会破坏 source 顺序。

generation-sensitive dependency 会暴露 stale context，而不是使用不匹配的 SPS、PPS、ASC 或
sample description 静默 decode。consumer 可以保留 partial result，并呈现既有的
dependency-unavailable 状态。

core directory 有意不持有 typed format payload。rule owner 必须维护 definition ID 到 payload
的关联；未来 persistent cache 必须在相同 source/rule identity 下同时序列化这两层。

## 考虑过的方案

- 每个数字 ID 只保留最新 definition：无法回答历史或 known-offset 查询，也会让不同 track 与
  definition kind 冲突。
- 只把 definition 放在 analysis tree 中，每次查询搜索 tree node：混合 presentation hierarchy
  与 context-selection policy，也没有精确 dependency-generation 契约。
- dependent context 永远使用创建时捕获的 generation：隐藏后续 dependency replacement，可能
  按 stale rule decode。
- dependency 失败时退回所请求 key 的旧 generation：会静默猜测上下文，而不是报告最新定义
  不可用。
- 要求按 source 顺序注册：与 lazy materialization 和 known-offset direct analysis 冲突。
- 在 core 中保存 typed SPS/PPS/ASC/sample-description variant：会把 core 绑定到属于 rules 且会
  独立演进的 format schema。

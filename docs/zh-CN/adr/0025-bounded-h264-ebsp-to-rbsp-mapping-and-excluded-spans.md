# 对 H.264 EBSP-to-RBSP 映射设限并显式呈现 excluded span

状态：已接受
日期：2026-07-28

## 背景

ADR-0024 已允许 `BitReader` 在不复制 transformed byte 的情况下消费 source-mapped
logical view。剩余的 H.264 路径仍没有组件负责构造 RBSP mapping，或对被排除的 byte
进行分类。因此当前 Annex B analyzer 只能解码 direct one-byte NAL header，并把之后的
payload byte 全部留作未解释数据。

ITU-T H.264 (08/2024) 7.3.1 会先解析 NAL-unit header，再生成 RBSP byte。从完整 header
之后开始，每个完整的 source sequence `00 00 03` 会把两个 zero byte 放入 RBSP，并把
`03` 作为 `emulation_prevention_three_byte` 丢弃。该解码规则不依赖第四个 byte。因此，
末尾的 `00 00 03` 仍会排除 `03`；`00 00 03 04` 也仍会排除 `03`，尽管 7.4.1 禁止该
four-byte sequence。

7.4.1 另外约束了 conforming NAL-unit content。它禁止 byte-aligned sequence
`00 00 00`、`00 00 01` 和 `00 00 02`，禁止第四个 byte 大于 `03` 的
`00 00 03 xx`，并禁止 NAL unit 的最后一个 byte 为 `00`。这些 validity rule 必须产生
diagnostic，但不能改变 7.3.1 transformation，也不能静默保留 decoder 本应丢弃的 byte。

一个 NAL unit 可能非常大，恶意输入也可以频繁交替 forwarded 与 excluded byte，使存储量
随输入线性增长。因此 mapper 在保持精确 source coordinate 的同时，还必须限制 work、mapping
segment、excluded-span record、diagnostic 和 cancellation latency。

## 决策

rules 模块将提供可复用且有状态的 H.264 EBSP-to-RBSP mapper。它接收一个
`RandomAccessSource`、由 caller 分配的 RBSP logical-view ID，以及一个只包含完整 NAL-unit
header 之后 EBSP byte 的精确有限 `SourceSpan`。该 span 必须 byte-aligned，长度必须是完整
byte，必须位于 source 内并可由 bit coordinate 表示。mapper 不推断 Annex B boundary 或
header length；Annex B 以及未来的 AVC-in-ISO-BMFF caller 必须先确定这些边界。

对于 Annex B 输入，caller 必须先按 B.1 和 B.2 完成 byte-stream extraction。
`leading_zero_8bits`、`zero_byte`、`start_code_prefix_one_3bytes` 和
`trailing_zero_8bits` 都属于 framing，不得进入传给 mapper 的 EBSP span。Annex B scanner
会把下一个 start code 或 source end 之前的最大 zero-byte run 从前一个 `nalUnit` span 中分离，
并单独公开该 run。analyzer 会用一个覆盖整段 run 的 materialized
`trailing_zero_8bits` region 使其保持可见。

基础的 one-byte AVC NAL header 继续使用 direct source data。需要 extension header 的
NAL-unit type 只有在 caller 已经解析并排除完整 header 后才会交给 mapper。初始 Annex B
集成会继续把这类超出当前范围的 payload 留作未解释数据，而不会把 extension-header byte
当作 RBSP。

mapper 会按 source 顺序单次检查 byte，只保留有界 scan state。当 scan cursor 到达完整的
source sequence `00 00 03` 时，会 forward 两个 zero 并排除 `03`；判断不需要后续 byte。
完成一次排除后会重置 zero-run state，避免后面的 source byte 跨被丢弃 byte 参与匹配。相邻
forwarded byte 会合并成一个 source span。每条 excluded record 包含精确的 eight-bit source
span，以及 gap 所在位置的 RBSP logical bit offset。

transformation 与 conformance check 是同一次 scan 的两个独立输出：

- `00 00 03` 会排除 `03`，包括它位于输入末尾时。
- `00 00 03 xx` 且 `xx > 03` 时仍排除 `03`，并同时报告被禁止的 four-byte sequence。
- `00 00 00`、`00 00 01` 和 `00 00 02` 按 transformation 要求继续 forwarded，同时报告
  被禁止的 three-byte sequence。互相重叠的禁止序列会在各自 byte-aligned source position
  上报告。
- 非空输入的最后一个 source byte 为 `00` 时，报告 final-byte violation。

这些检查只覆盖完全位于 mapper input 内的 sequence。header semantic 以及跨越
header/payload boundary 的 conformance condition，仍由确定 header length 的 caller 负责。

conformance issue 带有精确 source span，不会使已经成功构造的 mapping 失效。Annex B
analyzer 会把受影响的 NAL region 标为 invalid，保留完整 RBSP mapping 和 excluded node，并
继续扫描后续 NAL unit。RBSP syntax 与 `rbsp_trailing_bits()` validation 留给后续 parser。
NAL region 会在追加 payload、excluded、framing 与 diagnostic child 时保持 `Indexing`，只在
这些 child 全部提交后才 transition 到 `Invalid`。

mapper 会公开：规范化 `SourceMapping` 形式的累计 forwarded span、累计 excluded record、累计
conformance issue、source cursor 以及 RBSP logical length。batch result 区分 `InProgress`、
`Complete`、`Cancelled`、`SourceError`、`InvalidInput`、`InvalidBatchSize` 和
`ResourceLimit`。complete result 仍可以带有 conformance issue。

每个 mapping batch 接受一个正数的最大 source-byte 检查量，默认值为 64 KiB。开始工作前
以及至少每检查 1,024 byte 都会观察 cancellation。构造时还必须提供正数的累计 mapping
segment、excluded record 和 conformance issue 上限；初始默认值分别为 65,536、65,536 和
1,024。达到 output limit 时，会在提交导致超限的 item 前停止并返回 `ResourceLimit`。
batch limit 或累计 limit 为零时属于 invalid，且不会消费 source data。

`InProgress` 和 terminal failure 会保留完整 committed prefix 的合法 mapping。cancellation
不会消费下一个尚未检查的 byte。在已验证 input span 内遇到 source error、end-of-source 或
不一致的 successful read 时，返回 `SourceError`，并在可用时保留 source message。在
`Complete`、`Cancelled`、`SourceError` 或 `ResourceLimit` 之后重复调用只会重放 terminal
status，不会产生新输出。实现不会分配 transformed byte buffer。mapper 的 `InvalidInput`
表示 caller contract violation；受信任的 Annex B 集成会把这个正常情况下不可达的状态转换为
既有 `InvalidRule` terminal status，而不会把 malformed media 误当成 invalid mapping argument。

Annex B analyzer 会把 mapping 作为单独的 bounded stage 执行，不会在一个 analysis batch
内扫描完整的大型 NAL payload。mapping 进行中，之前完成的 NAL region 仍保持已发布状态。
mapping failure 会保留 direct NAL header 与 mapper 已提交的 prefix，并以对应 state 和
diagnostic 标记当前 NAL 与 RBSP payload，同时保持既有 root-level source-error、
resource-limit 和 cancellation 行为。

direct header 成功解码后，非空 mapped payload 会按以下方式呈现在既有
`nal_unit[index]` region 下：

- 成功的 `rbsp_payload` 是 materialized `Region`，其 location 覆盖完整 RBSP logical range，
  因此只包含 forwarded source span。metadata 会标识 H.264 RBSP logical view，以及 7.3.1
  和 7.4.1。
- mapper failure 会发布 `rbsp_payload`；若 committed prefix 非空则附带其 location，并设置
  对应的 `Cancelled` 或 `Invalid` state 与 diagnostic。
- 每个 excluded byte 对应一个 materialized
  `emulation_prevention_three_byte[index]` `Region`，按 source 顺序编号，并定位在 direct
  EBSP view 中。
- 新 node 追加在既有 `start_code` 与 `NalUnitHeader` child 之后，并位于 optional
  `trailing_zero_8bits` framing region 之前。empty final NAL、header failure 以及
  header-only NAL 都不增加 mapped payload 或 excluded node；独立识别出的 trailing-zero
  framing 仍可能存在。

所有 payload 与 framing child 都会在 NAL parent 保持 `Indexing` 时追加。只有这些 child 和
diagnostic 全部提交后，parent 才会 transition，因为 terminal analysis node 不接受后续 child。

excluded region 不是 syntax field，没有 RBSP logical coordinate，也永远不会被吸收到通过
RBSP mapping 解析的 field 或 diagnostic 中。选择未来跨越 excluded byte 的 RBSP field 时，
只会高亮它不连续的 forwarded span；选择被排除 byte 时则会解析到对应的命名 region。这里
使用 `Region` 而不是 `CompressedPayload`，因为 RBSP 既可能包含普通 syntax structure，也
可能包含 opaque slice data。

本切片不接受草案中的通用 `view`、`forward`、`skip` 或 `slice` DSL syntax，也不定义 RBSP
trailing-bit syntax、SPS/PPS 或 slice parsing、extension-header parsing、lazy payload
boundary 或 progressive persistence。

## 修订（2026-08-01）

ADR-0038 新增了有界的 `rbsp_trailing_bits;` DSL 终结项，使这一项原先延后的职责进入正式
RBSP parsing。本文 ADR 仍只负责 EBSP-to-RBSP mapping 与 excluded-span 契约；它不新增通用
view、forward、slice 或 alignment 语言能力。

## 影响

H.264 syntax 可以直接从未修改 source 的 logical RBSP 读取，并跨越任意数量但受预算限制的
emulation-prevention gap，同时保留精确 multi-span location。excluded byte 会在 analysis
tree 与 raw-data view 中保持逐个可见且可选择。

malformed EBSP 既不会被静默规范化为 conforming stream，也不会仅因 conformance issue 而
完全不可读。decoder mapping 遵循 7.3.1，而 7.4.1 violation 会继续以 source-located 形式
显示在受影响 NAL region 上。

mapping storage 与 forwarded run 加上 retained excluded/issue record 的数量成比例，而且每项
都有显式上限，而不是与 forwarded byte 数量成比例。per-batch work 和 cancellation latency
都有界；cumulative cap 可以防止恶意输入构造无界 mapping 或 analysis subtree。需要更多
payload 的 caller 必须继续按有界 batch 处理；在后续 lazy-boundary 切片完成后，则只物化被
请求的 region。

该 helper 是 H.264-specific，但不依赖 container。Annex B 与未来 MP4 sample navigation
会共享同一 transformation contract，而不把 codec escape rule 放进 core reader，也不复制
decoded payload byte。

## 考虑过的方案

- 只有后续 byte 不大于 `03` 时才排除 `03`：这混淆了 conformance restriction 与 7.3.1
  decoder transformation，并会错误映射 malformed `00 00 03 04` 输入。
- 把 RBSP byte 复制到 decoded buffer：parser 实现直接，但会复制大型 payload，并需要第二套
  coordinate authority。
- 把 emulation-prevention 处理放入 `BitReader`：对 H.264 很方便，却会把 codec rule 加入
  通用 core read contract。
- 把 excluded byte 隐藏在 mapping metadata 中：比较紧凑，但 raw-data selection 无法把
  excluded byte 解析到命名 structural role。
- 现在就增加通用 mapped-view DSL syntax：以后会有价值，但在 verified H.264 behavior 与
  limit 尚未建立之前，会过早扩张 language、compiler 和 VM。

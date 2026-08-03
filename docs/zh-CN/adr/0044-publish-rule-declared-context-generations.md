# 发布规则声明的 Context Generation

Status: Accepted
Date: 2026-08-03

## 背景

内置 H.264 规则现在可以 materialize 有界 SPS 与 PPS 结构，但这些结构尚未建立 slice header
需要的 position-aware generation。ADR-0028 有意把 session 与 runner plumbing 留到第一个实际
消费 context 的格式规则。既有 `ContextDirectory` 已经拥有格式中立的 source-position 与
exact-generation 策略；typed SPS/PPS 值必须继续属于 rules 层。

当前 `DslExecutor` 只是 VM 的无状态转发。Annex B runner 另外创建 reader、强制精确消费
RBSP，并通过字段名遍历 presentation tree child 来取得运行值。若沿这条路径增加 context
registration，publication 顺序、typed payload 所有权和 dependency failure recovery 会分散到
analyzer，以及未来 AAC 与 ISO BMFF caller 中。

slice-header 解码还需要规则声明的 context import、动态 bit width、有界 sentinel loop 和显式
compressed-payload terminal。这些后续语言能力需要稳定 execution seam，但无需全部实现之后
才能让 SPS/PPS generation 生效。

## 决策

在 rules 层增加名为 `RuleExecutionSession` 的深 module。每个 instance 属于一个 analysis
source 与一个精确 compiled rule。其 `run` interface 在给定 logical view 上执行一个结构，
执行请求的精确消费策略，stage rule effect，并且仅在结构成功 materialize 后发布这些 effect。
module 拥有一个格式中立的 `ContextDirectory`，以及以返回 `ContextDefinitionId` 为 key 的私有
typed-payload association。Annex B runner 提供 source、mapping、enclosing source span、parent
analysis node 与 cancellation option；它不解释 parameter-set 字段，也不直接调用 directory。

format definition language 为第一个有界 publication 切片增加三个 annotation：

```cpp
@context("h264-sps", seq_parameter_set_id)
struct SequenceParameterSetRbsp {
    ue seq_parameter_set_id;
    ue log2_max_frame_num_minus4 @context_export;
    // ...
}

@context("h264-pps", pic_parameter_set_id)
@context_dependency("h264-sps", seq_parameter_set_id)
struct PictureParameterSetRbsp {
    ue pic_parameter_set_id;
    ue seq_parameter_set_id;
    bits<1> entropy_coding_mode_flag @context_export;
    // ...
}
```

每个结构至多出现一次 `@context`，它需要一个已识别的 directory-kind 字符串和一个字段名
参数。已识别字符串只映射到 core 闭集 kind：`h264-sps`、`h264-pps`、`aac-asc` 与
`iso-bmff-sample-description`。`@context_dependency` 具有相同参数形状，最多可以重复 16 次，
并且只允许出现在同时具有 `@context` 的结构上。`@context_export` 不接受参数，为私有 typed
payload 选择最多 64 个值。
相同 dependency kind 与 field pair 属于静态重复错误，而不是幂等 declaration。

每个 key、dependency key 与 exported value 都必须是同一结构中无条件、顶层、非数组的
unsigned scalar。首个切片接受 `bits`、enum、`ue` 与 unsigned computed field。conditional、
switch、repeat 或固定数组内部的字段会被拒绝，因为 publication 不能依赖成功选择的路径中可能
缺席的值。signed value、lazy region、生成的 trailing-bit 字段和 presentation-only region
都不是 context value。

compiler 把所有 annotation 字段名解析为稳定 typed-IR index。VM 只返回这些已解析 key、
dependency 与 export index 的值；它不会暴露完整的结构局部 environment，runtime 也不会从
analysis tree 回读值。私有 payload 记录发布结构的 schema 与显式 export 的 scalar value。
directory mutation 前会完成 payload 准备与容量预留；directory registration 成功后，以不会
分配内存的 move 提交 prepared payload，因此在既有 single-writer 模型下 directory 与 rules-owned
association 会一起变为可见。

definition 使用完整 enclosing NAL-unit source span 作为 availability span，以 materialized SPS
或 PPS structure node 作为稳定 analysis node。standalone Annex B 使用 scope zero。PPS dependency
在 PPS span 起点解析；返回的 SPS definition ID 作为 exact-generation dependency 注册。missing、
future、stale、cyclic 或其他 unavailable dependency 都不得 fallback。PPS syntax structure 保持
materialized，但会收到带 source 位置的 `dependency-unavailable` diagnostic；该次运行的 RBSP 与
NAL 为 invalid，不发布 PPS generation 或 typed payload，并继续扫描后续 NAL unit。

malformed、truncated、cancelled、resource-limited、未精确消费或其他 invalid 结构都不发布
context。后续 malformed redefinition 不会隐藏此前 valid generation，与 ADR-0028 一致。
context registration metadata 被拒绝属于 invalid rule/runtime state，不是 media-syntax fallback。

第一次有效 `run` 会把 session 锁定到一个 source 和一个 analysis-tree instance。使用不同
source 或 tree 复用属于 invalid rule/runtime state；移动 session 或所属 analyzer 会保留该
identity。非零 logical start 会执行从该位置开始的 mapped suffix，保留 mapping 的 logical
coordinate，并对这个 suffix 应用精确消费。context execution suffix 映射出的每个 source
span 都必须包含在声明的非空 enclosing source span 中；不匹配会在读取或绑定前被拒绝。

内置 SPS/PPS declaration 采用这些 annotation，并发布计划中的有界 slice-header 规则需要的
unsigned scalar value。package coverage token 保持 `parameter-sets`；规则源码以 package
`0.1.7` 发布。

## 影响

SPS/PPS materialization 现在会建立真实 source-position generation。PPS 绑定自身 NAL 之前
current 的精确 SPS generation；该 dependency 被 supersede 后 PPS 会 unavailable，而无需让
Annex B analyzer 知道 H.264 字段名或 parameter-set 语义。

execution session 成为 context publication 与后续 import 的测试 interface。既有 VM 继续负责
确定性、有界的字段执行；session 拥有跨 execution state、精确消费策略、staged effect 与
diagnostic translation。没有 context annotation 的 caller 继续通过相同 `run` path，且不会产生
directory effect。

后续 slice-header 工作将在同一 module 后增加 context-import typed IR、动态
`bits<expression>`、有界 sentinel loop 与最终 compressed remaining-bit region。imported value
来自与 `ContextDirectory` 所选 exact generation 关联的 rules-owned payload；不会从
presentation node 回读，也不会实现成 H.264 analyzer callback。

## 非目标

本决策尚不派发或解码 slice NAL unit，不把 PPS/SPS value import 到 VM frame，不增加动态位宽
语法、sentinel-terminated loop，也不把 slice data 保留为 compressed payload。它不在 SQLite
或 saved session 中持久化 live context state 或 typed payload，不分配非零 container track
scope，不注册 malformed parameter set，也不向任意 installed rule 开放 core directory-kind enum。

# ADR-0086: 环境上下文导入与活跃参数集解析

- **状态**: 已接受 (Accepted)
- **日期**: 2026-08-15
- **决策者**: StreamView 核心团队

## 背景

在 H.264 Annex B 码流中，增强信息（SEI）NAL 单元（`nal_unit_type == 6`）包含一个或多个 SEI 消息。部分消息（如 `buffering_period`，`payload_type == 0`）在载荷起始处显式携带标识符字段 `ue seq_parameter_set_id`（已通过 ADR-0084 块内局部导入键机制解决），但其他消息在其载荷内**不携带任何参数集标识符**。

具体而言，**图像定时 SEI 消息**（Picture Timing SEI, `payload_type == 1`，ITU-T H.264 条款 7.3.2.3.1、D.1.2 与 D.2.2）包含：
- 具有动态位宽 `cpb_removal_delay_length_minus1 + 1` 的 `cpb_removal_delay`；
- 具有动态位宽 `dpb_output_delay_length_minus1 + 1` 的 `dpb_output_delay`；
- 由 `CpbDpbDelaysPresentFlag`、`pic_struct_present_flag` 与 `time_offset_length` 控制的条件字段。

根据 D.2.2 条款，这些 HRD 与 VUI 参数均由**当前活跃序列参数集（active Sequence Parameter Set）**决定。由于 `pic_timing` 载荷内没有 `seq_parameter_set_id`，且 SEI NAL 单元在线性流顺序中先于主编码图像 slice NAL 单元出现，单遍正向流解析器必须从环境流上下文中解析活跃 SPS。

### 已探测死路与闸门证据

在确立环境导入能力之前，团队对四种替代路径及编译器闸门进行了深入探测：

1. **闸门 1：注解参数数量闸门（`src/rules/dsl_ir.cpp:1125`）**：
   尝试无键声明 `@context_import("h264-sps")` 被 DSL 编译器拦截：
   ```
   error: @context_import requires a context-kind string and a field name
   ```
   *证据*：[`src/rules/dsl_ir.cpp:1125`](file:///Users/yun/code/streamview/src/rules/dsl_ir.cpp#L1120-L1130) 严格强制要求 2 个注解参数（`kind` 与 `identifier`）。

2. **闸门 2：解析器表达式参数数量闸门（`src/rules/dsl.cpp:1892`）**：
   尝试二参调用 `context_value(h264_sps, property)` 被 DSL 解析器拦截：
   ```
   error: context_value requires three identifier arguments
   ```
   *证据*：[`src/rules/dsl.cpp:1892`](file:///Users/yun/code/streamview/src/rules/dsl.cpp#L1885-L1897) 严格要求 `context_value` 必须携带 3 个标识符参数。

3. **死路 A：块内局部常量键（`computed<u64> assumed_sps_id = 0`）**：
   在 ADR-0084 落地后，在块内声明计算字段 `computed<u64> assumed_sps_id = 0;` 并调用 `context_value(assumed_sps_id, h264_sps, ...)` 已可通过静态校验（`svtool rule check` 报 Rule OK）。但该方案在设计与语义层面被**明确拒绝**：
   - 字面量 `0` 并非多 SPS 码流中 active SPS 的合法近似；
   - 若码流实际激活 SPS ID 1 或 2，强行假设 ID 0 会静默错解动态位宽、破坏比特流偏移，并将错误解析树误标为 `Materialized`。

4. **死路 B：跨 case 回退绑定（`computed<u64> k = optional_value(seq_parameter_set_id, 0)`）**：
   在 `case 0:` 中声明 `ue seq_parameter_set_id`，并在 `case 1:` 中引用 `optional_value(seq_parameter_set_id, 0)` 虽然可通过编译，但 `seq_parameter_set_id` 静态绑定于同循环迭代中 `case 0` 的执行槽位。在 `case 1:` 执行时该槽位未物化，因而恒走回退值 `0`。此方案在语义上退化为死路 A，且引入了脆弱的跨互斥 switch 分支槽位绑定。

## 决策

我们引入**环境上下文导入**（Ambient Context Imports，即无导入键的 `@context_import` 与二元参数的 `context_value`），允许结构体从当前流位置前最近活跃的同类上下文 generation 中读取导出属性。

### 1. 语义：基于流位置的环境上下文解析

二元环境上下文表达式 `context_value(kind, property)` 解析为：在 `ContextDirectory` 中**严格在当前流 bit 位置之前成功注册的、该 `(kind, scopeId)` 的最近一个 generation** 所导出的属性值。`scopeId` 来源与有键路径完全一致。

该解析机制与 ADR-0078 定义的基于位置的 generation 选择机制完全同构，仅去除了键等值（key equality）过滤条件。仅在 `ContextRegistrationStatus::Registered` 状态的 generation 中选取，依赖解析沿用既有有键路径语义。

### 2. 有界线性近似与已知局限性

根据 ITU-T H.264 7.4.1.2.3 与 D.2.2 条款，规范意义上的 active SPS 由访问单元的主编码图像激活。在单遍前向分析器架构中：
- 环境解析采用流位置上前置最近注册的 SPS generation 作为 active SPS 的有界线性近似；
- **已知局限性**：在存在多个并发 SPS 定义、且 `buffering_period` SEI 激活的 SPS 并非最近注册的 SPS 的特殊码流中，环境解析将选取最近注册的 SPS 而非缓冲周期指定的 SPS；
- 完整的访问单元状态机、跨 NAL 前瞻（lookahead）以及块内上下文重发布机制，在 v0.1 中明确属于 **out of scope**，留待后续里程碑探索。

### 3. 语法与文法规范

1. **结构体级注解**：
   ```dsl
   @context_import("h264-sps")
   struct SeiRbsp { ... }
   ```
   接受单个字符串参数，指定在环境模式下导入的上下文 kind。

2. **上下文求值表达式**：
   ```dsl
   computed<u64> nal_hrd_present = context_value(h264_sps, effective_nal_hrd_parameters_present_flag);
   ```
   接受两个标识符参数：上下文 kind 标识符与目标导出属性名。

3. **编译器文法放宽**：
   - `src/rules/dsl_ir.cpp:1125`：放宽 `parseContextAnnotation`，支持环境模式的 1 参形式（`@context_import("kind")`）与有键模式的 2 参形式（`@context_import("kind", key)`）；
   - `src/rules/dsl.cpp:1892`：放宽解析器 arity 检查，同时接受 2 个标识符参数（环境形式 `context_value(kind, field)`）与 3 个标识符参数（有键形式 `context_value(key, kind, field)`）；
   - `src/rules/dsl_ir.cpp:1508`：放宽 `resolveContextValue` 的 IR 降低逻辑，支持 2 操作数调用匹配外层结构体的环境 `@context_import`，报错文本相应更新（原 `context_value requires an import key, a context-kind identifier, and an exported field name`）；
   - *注意*：`src/rules/dsl_ir.cpp:1553`（ADR-0084 的分支保证支配检查）仅对有键导入生效，环境模式自然绕过该检查，不得改动该处逻辑。

### 4. 与有键导入共存

- **共存合法性**：同一结构体对同一 context kind 同时声明有键导入与环境导入完全合法（例如 `SeiRbsp` 在 `case 0:` 声明 `@context_import("h264-sps", seq_parameter_set_id)`，并在外层声明 `@context_import("h264-sps")` 供 `case 1:` 消费）；
- **消歧规则**：
  - 三参调用 `context_value(key, kind, field)` 绑定到匹配 `key` 的有键导入；
  - 二参调用 `context_value(kind, field)` 绑定到匹配 `kind` 的环境导入；
- **校验约束**：同一结构体上对同一 context kind 重复声明环境导入为编译错误；
- 任务 T11b 的测试矩阵将涵盖共存的正反例用例。

### 5. 失败隔离与持续扫描合同

沿用 ADR-0084 确立的失败隔离原则，并严格对齐核心层 `ContextLookupStatus` 枚举：
- 若在当前流位置之前不存在该 `(kind, scopeId)` 的任何 generation，查找返回 `ContextLookupStatus::NotFound`；
- 若找到环境 generation 但其传递依赖解析失败，查找返回 `ContextLookupStatus::DependencyUnavailable`；
- 无论上述何种失败，失败均被严格隔离在当前正在解析的单个 SEI 消息节点内（状态标记为 `MaterializationState::Invalid` 或 `MaterializationState::WaitingDependency`），分析器根据 `payload_size` 安全跳过并继续解析后续 SEI 消息；
- 环境上下文缺失绝不污染或中止整个 `SeiRbsp` 容器。

#### 5.1 环境导入依赖按需登记修订记录（Commit `6fbf585` / 任务 T11c）
在 `RuleExecutionSession`（`src/rules/rule_execution_session.cpp:338-343`）中，环境上下文导入依赖登记时机由静态无条件登记修正为**执行期实际访问触发**（基于 `importCache.at(importIndex).has_value()`）。
这是实现第 5 节「单消息粒度失败隔离」的必要前提：声明了 `@context_import("h264-sps")` 的容器结构体（如 `SeiRbsp`）可能执行完全不求值 `context_value` 的分支（例如 `case 5:` 用户数据、`case 47:` 显示方向）。若对每次结构体执行均无条件登记环境依赖，将在码流尚无前置 SPS 时错误地将这些独立的非依赖消息判定为 `WaitingDependency` / `NotFound`。改为按需登记后，非依赖消息保持完全隔离，在无参数集上下文时仍可成功物化。

### 6. 格式无关的核心层接口与复杂度

1. **核心目录接口**：
   `ContextDirectory` 核心类将扩充通用查询接口：
   ```cpp
   [[nodiscard]] ContextLookupResult resolveLatestBefore(ContextDefinitionKind kind,
                                                        quint64 scopeId,
                                                        SourceBitAddress sourcePosition) const;
   ```
2. **检索机制与复杂度**：
   - 基于现有 `definitionsByKey_` 索引（`std::map<ContextKey, std::vector<ContextDefinitionId>>`，[`src/core/include/streamview/core/context_directory.h:109`](file:///Users/yun/code/streamview/src/core/include/streamview/core/context_directory.h#L105-L113)），在 `(kind, scopeId)` 的连续键区间上查询；
   - 对于 `h264-sps`，键数量 $K \le 32$（受 `seq_parameter_set_id @range(0, 31)` 约束）；
   - 时间复杂度为 $O(K \cdot \log M)$（$M$ 为每个键的平均 generation 数量），零新增索引状态、零额外堆内存分配；
3. **纯度保证**：纯只读查询，直接检索不可变的 generation 记录，无跨批次可变状态。

## 分步实施切片计划

为确保规则消费节奏与底层能力切片的清晰隔离，后续任务按以下顺序执行：

1. **任务 T11a（当前）**：双语 ADR-0086 规范与探测证据归档（Markdown-only）；
2. **任务 T12a**：帧打包排列 SEI 消息解码（Frame Packing Arrangement, `payload_type == 45`，包版本 `0.1.36`）——纯规则消费，无上下文依赖；
3. **任务 T12b**：显示方向 SEI 消息解码（Display Orientation, `payload_type == 47`，包版本 `0.1.37`）——纯规则消费，无上下文依赖；
4. **任务 T11b**：环境上下文导入引擎能力实现（`ContextDirectory::resolveLatestBefore`、`dsl_ir`、`dsl_vm` 及定向测试）——纯能力切片，包版本不变；
5. **任务 T11c**：图像定时 SEI 消息规则消费（Pic Timing SEI, `payload_type == 1`，包版本 `0.1.38`）——规则消费切片，SPS 补齐参数导出并消费环境上下文。

## 参考文献

- ITU-T H.264 条款 7.3.2.3.1, 7.4.1.2.3, D.1.2, D.2.2
- ADR-0078: Bind Redefined Parameter Sets By Stream Position
- ADR-0084: Locally Scoped Context Import Keys
- ADR-0085: Decode the Buffering Period SEI Message

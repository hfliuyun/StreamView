# ADR-0105: 容器样本导航、时间线索引与访问单元执行合同

- **状态**：Proposed
- **日期**：2026-08-19
- **作者**：StreamView Contributors

---

## 上下文

StreamView 阶段 5 交付非分片 ISO BMFF MP4/MOV 容器解析、元数据树物化、超大 `mdat` 惰性封装、样本表窗口化分页，以及到基本流语法的跨层导航。

任务 P5a 至 P5i 顺利实现并验证了：
1. 顶层 Box 扫描与 `mdat` 惰性封装（Task P5e，`org.streamview.mp4` v0.1.0）；
2. `moov` 容器层级与时间头部解析（Task P5f，`org.streamview.mp4` v0.1.1）；
3. `stts`、`stsc`、`stsz`、`stco` 和 `co64` 样本表窗口化分页（Task P5g，`org.streamview.mp4` v0.1.2）；
4. `stsd`、`avc1`、`avcC`、`mp4a` 和 `esds` 样本描述与编解码配置架构（Task P5h，`org.streamview.mp4` v0.1.3）；
5. 从 `avcC`（SPS/PPS）和 `esds`（ASC）到 `video.h264.nal` 和 `audio.aac.asc` 子树的跨层导航、会话上下文共享、面包屑与双向坐标高亮（Task P5i，ADR-0103 / ADR-0104）。

### 问题陈述与能力差距审计

尽管 Task P5i 达成了静态编解码配置载荷（`avcC` / `esds`）的跨层导航，但 StreamView 产品需求（[PRD](../product-requirements.md) §§22, 37, 47）与阶段 5 验收标准明确要求支持从 **`mdat` 中的容器媒体 sample** 深入导航至底层 H.264 或 AAC 语法。

对现有架构与实现进行全面审计，发现 Task P5i 与完整容器样本导航之间存在以下规范与能力差距：

1. **静态配置 vs 动态媒体样本载荷**：
   - Task P5i 导航的是位于 `moov` 中的静态配置头（`avcC` SPS/PPS 与 `esds` ASC）。
   - `mdat` 中的媒体数据并非自描述的独立 Box；其物理字节区间、持续时间、呈现时间戳与关键帧属性必须由多个样本表组合计算得出。
2. **缺失样本索引与时间线表**：
   - 缺少 `stss`（Sync Sample Box）用于识别随机访问关键帧。
   - 缺少 `ctts`（Composition Time to Sample Box）用于在存在 B 帧 / 时域重排时精确计算呈现时间戳（$\text{PTS} = \text{DTS} + \text{composition\_offset}$）。
   - 缺少将 `stts`、`stsc`、`stsz`/`stz2`、`stco`/`co64`、`stss` 和 `ctts` 聚合为连续样本序列的格式中立复合索引服务。
3. **AVC 媒体样本的分帧形态不匹配**：
   - `mdat` 中的 AVC 视频样本**并非**单个独立 NAL 单元，亦**非**带 `00 00 01` 起始码的 Annex B 字节流。
   - 根据 ISO/IEC 14496-15，一个 AVC 样本包含一个或多个长度前缀 NAL 单元，长度前缀占用 `lengthSizeMinusOne + 1` 字节（由 `avcC` 定义）。
4. **AAC 音频样本的首版展示层级**：
   - `mdat` 中的 AAC 样本是包含单个 `raw_data_block` 的访问单元（Access Unit）。
   - 根据 PRD 要求，v0.1 暂不实现 Huffman 频谱解码。AAC 样本的展示粒度必须有明确边界。
5. **持久化边界澄清**：
   - PRD 提及保存的会话保留导航状态，而 ADR-0103 定义 v0.1 的导航栈为进程内临时交互状态。

本 ADR 建立 Task P5j 的规范架构决策与切片计划，补齐全部差距并完成阶段 5。

---

## 决策

### 1. 阶段 5 “MP4 Sample” 的规范定义

在 StreamView 阶段 5 中，**MP4 Sample** 规范定义为：
> 一个离散的逻辑访问单元（Access Unit），其在根媒体源（`mdat`）内的物理位置与字节区间由所在轨道的样本表（`stsc`、`stsz`/`stz2`、`stco`/`co64`）计算得出，其解码与呈现时间戳由 `stts` 和 `ctts` 导出，其随机访问关键帧属性由 `stss` 导出，其格式语义由关联的 `stsd` 样本描述项约束。

- **前置能力 vs 最终能力**：Task P5i 中对静态编解码配置（`avcC` / `esds`）的导航是前置能力切片；Task P5j 的容器样本导航是阶段 5 容器分析的最终闭环。
- **物理区间**：每个样本解析为根文件中一组精确的 `core::SourceSpan`（正常情况恰好一个），严禁对样本数据做堆内存复制。

---

### 2. `SampleDescriptor` 结构与接口

为了解耦样本表索引、UI 展示与编解码执行，核心层 `src/core/` 中定义格式中立的 `SampleDescriptor` 结构：

```cpp
namespace streamview::core {

struct SampleDescriptor final {
    quint32 trackId = 0;
    quint64 sampleIndex = 0;                 // 轨道内 0 起始样本序号
    quint32 sampleDescriptionIndex = 1;      // stsd 条目 1 起始索引
    std::vector<SourceSpan> sourceSpans;     // 根媒体源中的绝对区间
    qint64 dts = 0;                          // timescale 单位的解码时间戳
    qint64 pts = 0;                          // timescale 单位的呈现时间戳
    quint64 duration = 0;                    // timescale 单位的样本时长
    quint32 timescale = 1;                   // 来自 mdhd 的轨道时间基
    bool isSyncSample = true;                // 是否为关键帧 / 随机访问点
};

} // namespace streamview::core
```

以下三条声明约束是规范性的，不是风格偏好：

1. **`SourceSpan` 没有默认构造函数**。它只能通过工厂方法
   `SourceSpan::create(SourceBitAddress, quint64)` 构造，构造函数为 private
   （`src/core/include/streamview/core/coordinates.h:38-55`），因此裸成员
   `SourceSpan sourceSpan;` 无法编译。声明成员因此为 `std::vector<SourceSpan> sourceSpans`，
   与核心层既有先例 `ContextDefinitionSpec` 与 `ContextDefinition` 一致
   （`src/core/include/streamview/core/context_directory.h:46` 与 `:54`）。普通样本解析为
   恰好一个区间；vector 形态同时保持了项目「不得把不连续 spans 合并为连续包络」的既有规则。
2. **整数写法遵循核心层约定**：核心头文件使用 Qt 定宽别名（`quint64` / `quint32` / `qint64`），
   而不是 `uint64_t` / `int64_t`。
3. **`SourceSpan` 是 bit 寻址而非 byte 寻址**：`SourceBitAddress` 承载绝对 bit 偏移，
   `bitLength()` 是 bit 计数，因此索引器构造区间时必须把样本的字节偏移与字节长度换算为 bit 坐标。

`SampleDescriptor` 有意不包含编解码器名称或 `targetFormat` 字符串。选中的 `stsd` 条目由
`src/core/` 之外的规则/应用层绑定解析，该绑定携带不透明的规则入口身份及其声明元数据。
这样既遵守 ADR-0096 的格式中立核心边界，也允许 UI 与样本运行器选择正确的编解码入口。

---

### 3. `stss` 与 `ctts` 规范与索引规则

#### 3.1 关键帧表 (`stss`)
- **DSL 规范**：`stss`（`0x73747373`）解码 FullBox 头部与 `@window(SyncSampleEntry, entry_count)`，每项包含 `bits<32> sample_number;`（1 起始）。
- **缺省语义**：根据 ISO/IEC 14496-12 §8.6.2.1，**若轨道中未包含 `stss`，则该轨道中的每一个样本均视为同步关键帧（`isSyncSample = true`）**。这是适用于所有轨道类型的轨道级默认语义，不限于 AAC 或全 I 帧视频；索引器必须对每种轨道统一应用。
- **存在语义**：当 `stss` 存在时，仅在 `stss` 表中列出的样本标记为 `isSyncSample = true`；其余样本标记为 `isSyncSample = false`。

#### 3.2 呈现时间偏移表 (`ctts`)
- **DSL 规范**：`ctts`（`0x63747473`）解码 FullBox 头部与 `@window(CompositionOffsetEntry, entry_count)`。两个 version 声明的原始字段完全相同，均为 `bits<32> sample_count;` 与 `bits<32> sample_offset;`，因为 **DSL 没有有符号定宽字段类型**。以下为 `build/dev/tools/svtool/svtool rule check` 实测：
  - `i32 sample_offset;` → `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type`；
  - `bits<32> x @range(-100, 100);` → `error: @range bounds cannot be negative on unsigned fields`；
  - `computed<i64> v = ...;` → `error: Scalar types must be bool or u64`；
  - `se` 虽然存在，但它是有符号**指数哥伦布**编码，不是 32 位定宽字段，无法解码 `ctts` 表项。
- **符号重解释边界**：按 ISO/IEC 14496-12，`version == 1` 的偏移是二进制补码有符号 32 位值，但规则将其解码为无符号 32 位，且 **DSL 无法重解释该符号**。重解释（`offset >= 2^31 ? offset - 2^32 : offset`）由 `Mp4SampleTableIndex` 在 C++ 侧完成，判据是规则解码出的 `version` 字段。这是对已解码标量的时间线算术，不是格式专属语法，因此不违反「格式语义只能进 DSL」的约束。`version == 1` 时展示树中仍显示原始无符号字段值，有符号解释只体现在派生的 `pts` 上。P5j-1 不得声称规则解码了有符号字段；P5j-2 必须携带一条测试，证明 `version == 1` 的负偏移会产生小于其 `dts` 的 `pts`。
- **PTS 计算**：
  - 当 `ctts` 存在时：$\text{PTS} = \text{DTS} + \text{signed}(\text{sample\_offset})$。
  - 当 `ctts` 缺失时：$\text{PTS} = \text{DTS}$。
- **有符号时间线算术**：`dts` 与 `pts` 使用有符号 64 位时间线表示，并对加法执行受检运算。下溢或上溢必须在受影响样本页产生带源位置的 `InvalidSyntax`/`ResourceLimit` 诊断，不得回绕。计算结果为负的 `pts` 是合法的，不得钳位到零。
- **时间线真值性**：在含 B 帧的码流中，准确验证 PTS 必须解析并应用 `ctts`。

---

### 4. AVC 样本分帧（`lengthSizeMinusOne`）与多 NAL 执行合同

根据 ISO/IEC 14496-15 §5.3.4.2.1，`mdat` 中的 AVC 视频样本由一个或多个长度前缀 NAL 单元组成：

$$\underbrace{[\text{长度}]_{L\text{ 字节}}[\text{NAL 单元}]}_{\text{NAL } 1}\;\underbrace{[\text{长度}]_{L\text{ 字节}}[\text{NAL 单元}]}_{\text{NAL } 2}\;\dots$$

其中 $L = \text{lengthSizeMinusOne} + 1 \in \{1, 2, 4\}$（来自轨道的 `avcC` 配置记录，通常为 4 字节）。

#### 执行规则：
1. **长度前缀分帧**：样本运行器解析长度字段 $L$，校验 NAL 长度不超出样本边界，并为每个 NAL 单元创建子 `SourceMapping` 区间。
2. **多 NAL 聚合**：单个视频样本可能包含多个 NAL 单元（如 AUD、SEI、主 slice、冗余 slice）。执行过程按序生成包含所有 NAL 单元的子树。
3. **会话上下文继承**：每个 NAL 单元作为 `NalUnitHeader` + payload 执行，继承在 `avcC` 或先前端关键帧中发布的 SPS/PPS 上下文。
4. **映射转换合同**：RBSP 执行复用 ADR-0104 的 payload-transform 合同：防竞争字节保持为独立记录，转发后的子字段 spans 直接映射到样本根源，畸形转换输入只影响该 NAL 并产生带源位置的诊断。

---

### 5. AAC 访问单元首版展示层级

根据 ISO/IEC 14496-14 与 14496-3，`mdat` 中的音频样本是包含单个 `raw_data_block` 的 AAC 访问单元。

根据 PRD（“AAC Huffman 频谱负载解析...后置”）：
1. StreamView v0.1 不在样本内部进行 Huffman 频谱系数或 MDCT 解码。
2. 进入 AAC 样本时生成**访问单元封装子树**，展示：
   - 样本元数据（样本序号、DTS/PTS、时长、物理字节大小）；
   - 直接映射到 `mdat` 的 raw 访问单元字节区间；
   - 引用与相同 `trackId` 和 `sampleDescriptionIndex` 绑定的 `AudioSpecificConfig` 格式化描述（采样率、声道数、AOT）。若该配置缺失或不兼容，导航返回 `DependencyUnavailable`，不得猜测 ASC。
3. `RawDataView` 保留对整个访问单元字节区间的逐 bit 精确高亮。

---

### 6. 分页、资源预算、取消与错误隔离

1. **渐进索引**：对百 GB 级包含数十万样本的文件，`Mp4SampleTableIndex` 通过现有分页缓存合同按需渐进构建。缓存命名空间与 `streamId` 必须区分轨道和索引类型；不同轨道绝不能碰撞。SQLite WAL 只是不透明缓存页的持久化机制，不是样本表语义层。
2. **虚拟化 UI**：`MainWindow` 中的样本列表采用分页与虚拟化呈现（如每个 UI 页 256 或 1000 个样本），确保树节点数量远低于 `defaultMaximumMaterializedNodes() = 100,000`。UI 样本页不等同于现有 64 KiB 物理缓存页；一个描述符批次可以跨越多个缓存页。
3. **按需执行**：仅在用户显式点击导航某个样本时触发该样本的解码执行，每次导航具有独立预算。
4. **错误隔离**：`mdat` 中损坏或截断的单个样本发出局限于该帧的 `TruncatedSource` 或 `InvalidSyntax` 诊断，不损坏父容器、其他样本或会话状态。
5. **预算隔离**：独立页请求拥有独立的索引/分页预算与取消作用域。VM `RunnerExecutionBudget` 不得被之前页请求永久消耗；另设 session/UI 物化预算限制保留的样本行与节点数。

---

### 7. 导航状态与 SessionDocument 契约对齐

- **PRD 对齐**：StreamView v0.1 中，`SessionDocument` 严格持久化根会话状态（源身份、规则版本、书签、注释与根视图展示状态）。PRD 中“导航状态”明确仅指根视图状态；活动的子格式/样本导航栈在 v0.1 不持久化。
- **导航栈边界**：导航栈（子格式帧、样本帧）在 v0.1 中作为内存中的临时交互状态，子树导航栈的完整持久化序列化留待阶段 7 会话扩展。

---

### 8. ADR-0103 状态转移

随着 Task P5i（P5i-1、P5i-2、P5i-3、P5i-4a、P5i-4a-R、P5i-4b）的完整实现与严格验证，ADR-0103 的架构契约已全部被实测证明。在阶段 5（Task P5j）收官时，ADR-0103 正式由 `Proposed` 转为 `Accepted`。

---

## 阶段 5j 实施切片计划

为确保增量验证、职责隔离与严格质量门禁，Task P5j 拆分为八个顺序切片：

```
[Task P5j-0 (规范)]: 差距审计与架构决策 (ADR-0105)
      │
      ▼
[Task P5j-1 (规则/MP4)]: org.streamview.mp4 v0.1.4 stss、ctts 与 stz2 DSL 规则
      │
      ▼
[Task P5j-2 (规则/核心)]: Mp4SampleTableIndex 复合时间线与样本服务
      │
      ▼
[Task P5j-3 (规则/运行时)]: AVC 长度前缀多 NAL 与 AAC 访问单元执行器
      │
      ▼
[Task P5j-3b (规则/MP4)]: 分析树 → Mp4TrackSampleTables 提取与读取器绑定
      │
      ▼
[Task P5j-4 (应用/核心)]: AnalysisSession 样本导航 API 与坐标映射
      │
      ▼
[Task P5j-5 (应用/UI)]: MainWindow 轨道/样本面板、时间线表格与面包屑集成
      │
      ▼
[Task P5j-6 (验证/关闭)]: 阶段 5 大文件矩阵、参考工具比对与里程碑收官
```

### 切片明细：

1. **Task P5j-0（规范与差距审计）**：
   - 交付物：双语 ADR-0105（Markdown-only）。
   - 范围：审计能力差距，定义规范合同，规划 P5j-1 至 P5j-6 切片。

2. **Task P5j-1（DSL 与官方 MP4 规则包 v0.1.4）**：
   - 交付物：在 `mp4_isobmff.svfmt` 中增加 `stss`（`SyncSampleBox`）、`ctts`（`CompositionOffsetBox`）与缺失的 `stz2` 紧凑样本尺寸结构；升级 `org.streamview.mp4` 版本为 `0.1.4`。
   - 涉及文件：`src/rules/official/org.streamview.mp4/src/mp4_isobmff.svfmt`、`rule.toml`、`tests/rules/mp4_isobmff_analyzer_test.cpp`。

3. **Task P5j-2（复合样本索引与时间线服务）**：
   - 交付物：`Mp4SampleTableIndex`，组合 `stts`、`stsc`、`stsz`/`stz2`、`stco`/`co64`、`stss` 和 `ctts` 为有界内存的 `SampleDescriptor` 序列，支持受检有符号时间线算术、按轨道缓存键与取消。该类**刻意是 MP4 专属的，位于 `src/rules/`** 而非 `src/core/`：它消费 ISOBMFF box 语义，称其「格式中立」自相矛盾。只有它的输出类型（`core::SampleDescriptor`）是格式中立的。`src/core/` 中不得出现任何 ISOBMFF box 名称。
   - 涉及文件：`src/rules/mp4_sample_table_index.h`、`src/rules/mp4_sample_table_index.cpp`、`tests/rules/mp4_sample_table_index_test.cpp`。
   - 接缝说明（P5j-4 准备阶段补记）：本 ADR 并未冻结样本表数值*从分析树何处提取*。P5j-2 以「调用方绑定读取器接缝」（`Mp4SampleTableReaders`，三个 `std::function`）解决了该歧义，从而避免把逐样本表展开进内存。该选择对有界内存合同是正确的，但它顺延了提取责任而没有指派归属，导致没有任何切片负责从真实分析树产出 `Mp4TrackSampleTables` 与已绑定读取器。下面的 Task P5j-3b 补上这个缺口。

4. **Task P5j-3（AVC 长度前缀多 NAL 与 AAC 样本运行器）**：
   - 交付物：格式中立的样本载荷分帧与执行器，处理 `lengthSizeMinusOne + 1` NAL 前缀、多 NAL 聚合、ADR-0104 映射 RBSP transform/排除字节合同、畸形转换诊断与会话上下文解析。
   - 涉及文件：`src/rules/sample_payload_runner.h`、`src/rules/sample_payload_runner.cpp`、`tests/rules/sample_payload_runner_test.cpp`。

5. **Task P5j-3b（分析树 → 样本表提取与读取器绑定）**：
   - 交付物：MP4 专属桥接层，遍历已分析的 ISOBMFF 树，按轨道产出填充好的 `Mp4TrackSampleTables`，以及三个回调均绑定到既有窗口解码器接缝的 `Mp4SampleTableReaders`，使 `stsz`/`stco`/`co64`/`stss` 条目按页惰性读取而非全量物化。包含 `stz2` field_size 处理、`stsz.sample_size` 非零的统一尺寸路径（该路径必须完全不调用 `sampleSize` 读取器）、`ctts` version 0/1 选择、按 §3.1 的 `stss` 缺失语义，以及携带 `targetFormat` 的 `stsd` sample description 绑定。
   - 独立成片的理由：定位这些表必须依赖 box 类型或规则结构名判别，而这在 `src/core/` 与通用 session/UI dispatch 中被禁止，因此桥接层必须位于规则层；同时禁止把新能力与它的第一个消费者塞进同一提交，因此它不能并入 P5j-4。故独立成片，独立提交，独立测试。
   - 涉及文件：规则层（`src/rules/`，具体组件边界在实现时决定——独立组件或 `Mp4IsobmffAnalyzer` 扩展），以及 `tests/rules/` 下自己的测试文件。
   - 约束：本切片不得让任何 ISOBMFF box 名称、box 类型常量或 MP4 规则结构名泄漏进 `src/core/` 或 `src/app/`。

6. **Task P5j-4（AnalysisSession 样本导航 API）**：
   - 交付物：`AnalysisSession::enterSample(trackId, sampleIndex)`、`samplesForTrack`、样本坐标到 `mdat` 的投影、样本帧导航状态与明确的错误映射/回滚语义。
   - 涉及文件：`src/app/analysis_session.h`、`src/app/analysis_session.cpp`、`tests/app/analysis_session_test.cpp`。

7. **Task P5j-5（MainWindow 轨道/样本导航与时间线 UI）**：
   - 交付物：轨道/样本面板、虚拟化样本表格、同步关键帧徽标、双击/键盘样本导航、面包屑路径 `video.mp4 > Track 1 (avc1) > Sample #42 [Sync] > NalUnitHeader` 与双向坐标联动。
   - 涉及文件：`src/app/main_window.h`、`src/app/main_window.cpp`、`tests/app/main_window_test.cpp`。

8. **Task P5j-6（阶段 5 里程碑验证与收官）**：
   - 交付物：100 GB 虚拟稀疏大文件验证、参考工具比对、offset/timestamp/keyframe 自动化 ground-truth fixture、ADR-0103 与 ADR-0105 正式转为 `Accepted`，在 `docs/implementation-plan.md` 中签署阶段 5 完工。
   - 参考工具可用性（本环境实测）：`ffprobe` 与 `ffmpeg` 存在；`mediainfo` 与 `MP4Box` **未安装**。因此交叉验证只规定基于 `ffprobe`，沿用 ADR-0097 既有先例（`ffprobe -v trace` 取 box 结构，`ffprobe -show_packets` 取 sample offset / 时间戳 / 关键帧 ground truth）。P5j-6 不得声称执行了无法运行的 `mediainfo` 比对；若确需第二个独立工具，其安装属于该切片范围并必须在报告中说明。

---

## 影响

### 正面
- 补齐阶段 5 最后一项能力差距，达成从顶层容器一直深入到编解码字段的端到端样本导航。
- 提供经 `ctts` 严格计算的精确 DTS 与 PTS 时间线，完整支持含 B 帧码流。
- 正确处理 AVC 长度前缀多 NAL 样本分帧，符合标准规范。
- 对超大文件保持严格零复制与虚拟化内存边界。

### 代价
- 需要为 `org.streamview.mp4` 增加 `stss` 和 `ctts` 并升级至 v0.1.4。
- 需要实现跨多张样本表的复合索引服务。

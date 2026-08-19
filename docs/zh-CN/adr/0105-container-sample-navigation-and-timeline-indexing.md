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
- **物理区间**：每个样本解析为根文件中的精确 `core::SourceSpan`，严禁进行堆内存数据复制。

---

### 2. `SampleDescriptor` 结构与接口

为了解耦样本表索引、UI 展示与编解码执行，核心层 `src/core/` 中定义格式中立的 `SampleDescriptor` 结构：

```cpp
namespace streamview::core {

struct SampleDescriptor final {
    uint32_t trackId = 0;
    uint64_t sampleIndex = 0;              // 轨道内 0 起始样本序号
    uint32_t sampleDescriptionIndex = 1;   // stsd 条目 1 起始索引
    SourceSpan sourceSpan;                 // 根媒体源中的绝对字节区间
    uint64_t dts = 0;                      // timescale 单位的解码时间戳
    uint64_t pts = 0;                      // timescale 单位的呈现时间戳
    uint64_t duration = 0;                 // timescale 单位的样本时长
    uint32_t timescale = 1;                // 来自 mdhd 的轨道时间基
    bool isSyncSample = true;              // 是否为关键帧 / 随机访问点
    QString targetFormat;                  // 如 "video.h264.sample", "audio.aac.sample"
};

} // namespace streamview::core
```

---

### 3. `stss` 与 `ctts` 规范与索引规则

#### 3.1 关键帧表 (`stss`)
- **DSL 规范**：`stss`（`0x73747373`）解码 FullBox 头部与 `@window(SyncSampleEntry, entry_count)`，每项包含 `bits<32> sample_number;`（1 起始）。
- **缺省语义**：根据 ISO/IEC 14496-12 §8.6.2.1，**若轨道中未包含 `stss`，则该轨道中的每一个样本均视为同步关键帧（`isSyncSample = true`）**。此规则严格适用于 AAC 音频轨道与全 I 帧视频流。
- **存在语义**：当 `stss` 存在时，仅在 `stss` 表中列出的样本标记为 `isSyncSample = true`；其余样本标记为 `isSyncSample = false`。

#### 3.2 呈现时间偏移表 (`ctts`)
- **DSL 规范**：`ctts`（`0x63747473`）解码 FullBox 头部与 `@window(CompositionOffsetEntry, entry_count)`。
  - `version == 0`：表项包含 `bits<32> sample_count;` 与 `bits<32> sample_offset;`（无符号 32 位）。
  - `version == 1`：表项包含 `bits<32> sample_count;` 与 `bits<32> sample_offset;`（有符号 32 位补码，支持负向前置偏移）。
- **PTS 计算**：
  - 当 `ctts` 存在时：$\text{PTS} = \text{DTS} + \text{sample\_offset}$。
  - 当 `ctts` 缺失时：$\text{PTS} = \text{DTS}$。
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

---

### 5. AAC 访问单元首版展示层级

根据 ISO/IEC 14496-14 与 14496-3，`mdat` 中的音频样本是包含单个 `raw_data_block` 的 AAC 访问单元。

根据 PRD（“AAC Huffman 频谱负载解析...后置”）：
1. StreamView v0.1 不在样本内部进行 Huffman 频谱系数或 MDCT 解码。
2. 进入 AAC 样本时生成**访问单元封装子树**，展示：
   - 样本元数据（样本序号、DTS/PTS、时长、物理字节大小）；
   - 直接映射到 `mdat` 的 raw 访问单元字节区间；
   - 引用当前轨道有效 `AudioSpecificConfig` 的格式化描述（采样率、声道数、AOT）。
3. `RawDataView` 保留对整个访问单元字节区间的逐 bit 精确高亮。

---

### 6. 分页、资源预算、取消与错误隔离

1. **渐进索引**：对百 GB 级包含数十万样本的文件，`Mp4SampleTableIndex` 经由 `WindowDecoder` 与 `CancellationToken` 按需渐进构建并缓存于 SQLite WAL。
2. **虚拟化 UI**：`MainWindow` 中的样本列表采用分页与虚拟化呈现（如每页 256 或 1000 样本），确保树节点数量远低于 `defaultMaximumMaterializedNodes() = 100,000`。
3. **按需执行**：仅在用户显式点击导航某个样本时触发该样本的解码执行，每次导航具有独立预算。
4. **错误隔离**：`mdat` 中损坏或截断的单个样本发出局限于该帧的 `TruncatedSource` 或 `InvalidSyntax` 诊断，不损坏父容器、其他样本或会话状态。

---

### 7. 导航状态与 SessionDocument 契约对齐

- **PRD 对齐**：StreamView v0.1 中，`SessionDocument` 严格持久化根会话状态（源身份、规则版本、书签、注释）。
- **导航栈边界**：导航栈（子格式帧、样本帧）在 v0.1 中作为内存中的临时交互状态，子树导航栈的完整持久化序列化留待阶段 7 会话扩展。

---

### 8. ADR-0103 状态转移

随着 Task P5i（P5i-1 至 P5i-4b）的完整实现与严格验证，ADR-0103 的架构契约已全部被实测证明。在阶段 5（Task P5j）收官时，ADR-0103 正式由 `Proposed` 转为 `Accepted`。

---

## 阶段 5j 实施切片计划

```
[Task P5j-0 (规范)]: 差距审计与架构决策 (ADR-0105)
      │
      ▼
[Task P5j-1 (规则/MP4)]: org.streamview.mp4 v0.1.4 stss 与 ctts DSL 规则
      │
      ▼
[Task P5j-2 (规则/核心)]: Mp4SampleTableIndex 复合时间线与样本服务
      │
      ▼
[Task P5j-3 (规则/运行时)]: AVC 长度前缀多 NAL 与 AAC 访问单元执行器
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
   - 交付物：在 `mp4_isobmff.svfmt` 中增加 `stss`（`SyncSampleBox`）与 `ctts`（`CompositionOffsetBox`）架构；升级 `org.streamview.mp4` 版本为 `0.1.4`。
   - 涉及文件：`src/rules/official/org.streamview.mp4/src/mp4_isobmff.svfmt`、`rule.toml`、`tests/rules/mp4_isobmff_analyzer_test.cpp`。

3. **Task P5j-2（复合样本索引与时间线服务）**：
   - 交付物：格式中立的 `Mp4SampleTableIndex`，组合 `stts`、`stsc`、`stsz`/`stz2`、`stco`/`co64`、`stss` 和 `ctts` 为有界内存的 `SampleDescriptor` 序列，支持取消。
   - 涉及文件：`src/rules/mp4_sample_table_index.h`、`src/rules/mp4_sample_table_index.cpp`、`tests/rules/mp4_sample_table_index_test.cpp`。

4. **Task P5j-3（AVC 长度前缀多 NAL 与 AAC 样本运行器）**：
   - 交付物：格式中立的样本载荷分帧与执行器，处理 `lengthSizeMinusOne + 1` NAL 前缀、多 NAL 聚合、RBSP 转换与会话上下文解析。
   - 涉及文件：`src/rules/sample_payload_runner.h`、`src/rules/sample_payload_runner.cpp`、`tests/rules/sample_payload_runner_test.cpp`。

5. **Task P5j-4（AnalysisSession 样本导航 API）**：
   - 交付物：`AnalysisSession::enterSample(trackId, sampleIndex)`、`samplesForTrack`，以及样本坐标到 `mdat` 的投影。
   - 涉及文件：`src/app/analysis_session.h`、`src/app/analysis_session.cpp`、`tests/app/analysis_session_test.cpp`。

6. **Task P5j-5（MainWindow 轨道/样本导航与时间线 UI）**：
   - 交付物：轨道/样本面板、虚拟化样本表格、同步关键帧徽标、双击/键盘样本导航、面包屑路径 `video.mp4 > Track 1 (avc1) > Sample #42 [Sync] > NalUnitHeader` 与双向坐标联动。
   - 涉及文件：`src/app/main_window.h`、`src/app/main_window.cpp`、`tests/app/main_window_test.cpp`。

7. **Task P5j-6（阶段 5 里程碑验证与收官）**：
   - 交付物：100 GB 虚拟稀疏大文件验证、参考工具比对（`ffprobe`/`mediainfo`）、ADR-0103 与 ADR-0105 正式转为 `Accepted`，在 `docs/implementation-plan.md` 中签署阶段 5 完工。

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

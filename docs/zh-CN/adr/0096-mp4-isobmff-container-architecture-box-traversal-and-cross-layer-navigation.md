# ADR-0096: MP4/ISOBMFF 容器架构、Box 遍历、跨层导航与样本索引边界

- **状态**：Accepted
- **日期**：2026-08-16
- **作者**：StreamView 核心团队

---

## 背景

StreamView 阶段 5 引入非分片 MP4/ISOBMFF（`video/mp4`、`audio/mp4`，ISO/IEC 14496-12 / 14496-14 / 14496-15）容器解析、元数据树检查、样本表索引映射以及到底层基本流（`avcC` 中的 H.264 NAL 单元与 `esds` 中的 AAC ASC）的跨层导航支持。

与依赖连续同步字节模式（`00 00 01` / `0x7FF`）的 H.264 Annex B 和 AAC ADTS 码流不同，ISOBMFF 容器由长度前缀的 Box（Atom）构成分层树形结构。此外，媒体数据载荷（`mdat`）体积可达数百兆甚至数十吉字节，而样本索引表（`stts`、`stsc`、`stsz`、`stco`、`co64`）通常包含数万至数十万个条目。

为防止架构偏离，本 ADR 明确确立以下三大核心边界：
1. **D1：Box 遍历与 `mdat` 惰性封装职责划分**（内核与 DSL 的边界）；
2. **D2：跨层导航与规则间引用模型**（`avcC`/`esds` $\to$ H.264/AAC 联动）；
3. **D3：样本表索引与资源预算边界**（`stsc`/`stsz`/`stco`/`co64` 处理机制）。

---

## 决策 1：Box 遍历、层级结构与 `mdat` 惰性封装边界（D1）

### 结论
1. **核心库保持格式中立**：严格遵循全局架构禁令，`src/core/` 与 `src/rules/*.cpp` 中不得引入任何格式专属字符串、Box FourCC 标识符（`ftyp`、`moov`、`mdat`、`trak`、`stbl`）或容器分类学逻辑。
2. **DSL 结构化 Box 模型**：所有 ISOBMFF Box 头部、层级关系及载荷结构均在官方 MP4 规则包（`org.streamview.mp4`）内声明。
3. **Box 头部语法定义**：每个 Box 均以标准 8 字节头部起始（`size: u32`，`type: bytes(4)`）。扩展长度（`size == 1 \to u64 largesize`）与扩展类型（`type == 'uuid' \to bytes(16) usertype`）直接在 DSL 语法中表达。
4. **`mdat` 惰性区域化**：媒体数据（`mdat`）在 DSL 中使用 `@lazy(payload_bytes) bytes data;` 声明。超大媒体载荷绝不预先物化到内存中，对媒体样本消耗零堆内存，同时完整保留物理与逻辑坐标范围。
5. **边界情形处理**：
   - `size == 0`（延伸至文件末尾）：通过 DSL 剩余范围计算（`computed<u64> payload_bytes = available_bytes;`）处理；
   - `size == 1`（64 位 largesize）：通过 `if (size == 1) u64 largesize; computed<u64> payload_bytes = (size == 1 ? largesize - 16 : size - 8);` 处理。

### 依据与证据
- `src/core/include/streamview/core/analysis_model.h:38-42`：分析树与缓存载荷引擎（`analysis_cache_payload.cpp:277`）原生支持 `MaterializationState::Lazy`；
- `src/rules/official/org.streamview.aac/src/aac_adts.svfmt:24-27`：已在阶段 4 中成功验证 `@lazy` 载荷封装契约（`raw_data_block`）。

### 被否决方案及理由
- **C++ 硬编码 Box 扫描器**：在 C++ 层面使用 `switch (fourcc)` 分发。
  - *否决理由*：直接违反全局架构规范中禁止在核心塞入格式专属逻辑的强制约束。
- **纯 DSL 无界递归且无惰性封装**：预先完整物化包含 `mdat` 在内的所有 Box 节点。
  - *否决理由*：在大型媒体文件上会迅速超出 `maximumMaterializedNodes`（20,000）并耗尽系统内存。

---

## 决策 2：跨层导航与规则间引用模型（D2）

### 结论
1. **规则包自包含性**：在 v0.1 中，规则包保持独立自包含。`org.streamview.mp4` 声明容器语法、描述符结构（`ES_Descriptor`、`DecoderConfigDescriptor`）与解码配置记录（`AVCDecoderConfigurationRecord`）。
2. **目标格式注解语义关联**：编解码配置 Box 导出的结构化字节区域标注元数据（例如 `@target_format("video.h264.nal", "SPS")` 与 `@target_format("audio.aac.asc", "AudioSpecificConfig")`）。
3. **会话级跨层导航机制**：当用户或自动化检查器在 UI 中触发 `avcC` 或 `esds` 节点时，`AnalysisSession` / UI 层向 `RulePackageStore` 查询已注册的目标包入口点（`video.h264.annexb` 或 `audio.aac.asc`），并在对应坐标区间上创建跨层链接分析视图。
4. **后续演进路径**：完整的编译期跨包依赖管理（`rule.toml` 中的 `dependencies = [...]`）将在 Phase 7+ 作为独立的 DSL 编译器能力切片引入。

### 依据与证据
- `src/rules/include/streamview/rules/rule_package_store.h:35-52`：`RulePackageStore` 与 `RulePackageCatalog` 已提供跨包入口点查找与解析能力；
- `src/rules/dsl_vm.cpp:1113-1117`：核心引擎原生支持 `MaterializationState::WaitingDependency` 状态。

### 被否决方案及理由
- **在 v0.1 中引入动态跨包 AST 链接器**：在编译器模块语义尚未完备前强行支持跨包符号导入。
  - *否决理由*：引入过高的架构风险与范围膨胀，且对于基础容器检查并非必要。
- **C++ 跨分析器硬编码耦合**：在 `Mp4IsobmffAnalyzer` 内部直接实例化 `H264AnnexBAnalyzer`。
  - *否决理由*：破坏核心解耦原则，阻碍容器规则的独立自动化测试。

---

## 决策 3：样本表索引与资源预算边界（D3）

### 结论
1. **窗口化样本表检查**：样本表元数据（`entry_count`、表格配置）完整物化，而重复性海量索引表（`stsz` 条目数组、`stco`/`co64` 块偏移数组）采用 `@lazy` 区域与窗口化批次迭代控制。
2. **资源预算上限**：
   - 最大批次限制：单批次 `maximumRecords = 1000` 个样本；
   - 物化节点上限：严格限制在 `maximumMaterializedNodes = 20,000` 以内；
   - 内存预算：每 100,000 个索引样本占用内存 $< 32\text{ MB}$。
3. **状态完整性**：
   - `MaterializationState::Indexing`：用于容器发现与索引阶段；
   - `MaterializationState::WaitingDependency`：用于外部轨道定义未就绪时的状态标记。

### 依据与证据
- `src/core/analysis_model.cpp:59`：`MaterializationState::Indexing` 是分析树节点的合法生命周期状态；
- `src/rules/dsl_vm.cpp:1116`：`DslExecutionStatus::DependencyUnavailable \to MaterializationState::WaitingDependency` 已在核心执行引擎中完整验证。

### 被否决方案及理由
- **为每个样本无节制物化独立 AST 节点**：在包含 500,000 个样本的码流中创建 500,000 个 `AnalysisNode`。
  - *否决理由*：消耗数百兆堆内存，导致 UI 树形视图卡顿崩溃。

---

## 阶段 5 任务切片与实施计划

| 任务 | 类型 | 描述 |
| :--- | :--- | :--- |
| **任务 P5a** | 规范 | 双语 ADR-0096 架构设计与实施计划细化（Markdown-only）。 |
| **任务 P5b** | 能力 | DSL 编译器/语言闸门加固（编译期拦截未识别注解并报错）。 |
| **任务 P5c** | 规则 | 官方 MP4 规则包 `org.streamview.mp4` v0.1.0（`rule.toml`、`mp4_box.svfmt`、`mp4_ftyp.svfmt`、`mp4_moov.svfmt`、`mp4_trak.svfmt`、`mp4_mdat.svfmt`）。 |
| **任务 P5d** | 能力 | `Mp4IsobmffAnalyzer` 运行器、探测器注册与容器遍历测试。 |
| **任务 P5e** | 规则 | 样本表与编解码配置规则（`stsd`、`avc1`、`avcC`、`mp4a`、`esds`、`stts`、`stsc`、`stsz`、`stco`、`co64`）。 |
| **任务 P5f** | 能力 | `avcC` / `esds` 到 H.264 SPS/PPS 与 AAC ASC 入口点的跨层导航集成。 |
| **任务 P5g** | 验收 | 逐 bit 验收审计、超大 `mdat` 惰性验证与阶段 5 里程碑关闭。 |

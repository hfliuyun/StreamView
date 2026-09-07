# ADR-0112: 规则包存储、格式探测器与 MP4 Sample 提取器模糊测试框架

- **状态**：Proposed
- **日期**：2026-09-07
- **作者**：StreamView 贡献者

---

## 背景

在 ADR-0111 建立针对 DSL 核心引擎（`DslParser`、`DslCompiler` 和 `DslVirtualMachine`）的模糊测试框架之后，StreamView 必须进一步加固其面向外部二进制与格式资产的次级信任边界：
1. **规则包分发与存储（`RulePackageStore`）**：摄入外部 `.svrule` ZIP 归档包与目录资产，包含不可信的文件路径、清单文件（`rule.toml`）和格式规则描述源码；
2. **格式探测器与流式切片扫描器（`H264AnnexBDetector`、`H264StartCodeScanner`、`AacAdtsDetector`、`AacAdtsScanner`、`Mp4BoxDetector`、`Mp4BoxScanner`）**：摄入任意文件的原始不可信二进制字节流，以仲裁格式归属并切分流式记录；
3. **MP4 Sample 表提取与索引构建引擎（`Mp4SampleTableExtractor`、`Mp4SampleTableIndex`）**：从潜在畸形的 ISOBMFF `stbl` 树结构中重构 chunk-to-sample 映射、合成呈现时间戳、同步关键帧索引以及样本源区间。

依据 ADR-0110 第 1.1 与 1.2 节，这三大子系统直接构成了不可信二进制解析面，畸形输入、恶意压缩包（Zip Slip 目录穿越、zip bomb 拒绝服务）、整数乘法溢出、自循环容器引用及高频伪同步字风暴均可能导致拒绝服务、挂起、程序崩溃或内存损坏。

此外，ADR-0110 第 1.2 节明确要求，规则包与格式扫描器 fuzzing 语料必须显式纳入字段名与节点名包含 `#<数字>`（如 `tag#1`、`entry#0`）的合成语法树，以实证路径解析鲁棒性，并确认与 `#<row>` 同名兄弟消歧机制完全隔离、零冲突。

---

## 决策

### 1. 双模架构扩展与驱动体系

**决策**：将 ADR-0111 确立的双模架构（`STREAMVIEW_ENABLE_LIBFUZZER` 条件编译与基于 `standalone_fuzz_driver.h` 的跨平台确定性独立回放）推广至三个全新目标：
1. `fuzz_rule_package_store`：针对归档解包、目录导入与清单校验；
2. `fuzz_format_detectors`：针对 H.264 Annex B、AAC ADTS 及 MP4 box 格式探测与流式切片扫描器；
3. `fuzz_mp4_sample_extractor`：针对 ISOBMFF sample 表树结构提取与样本索引构建。

所有目标均导出标准入口 `extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)`，在禁用 `STREAMVIEW_ENABLE_LIBFUZZER` 时自动接入 `standalone_fuzz_driver.h`。

---

### 2. 规则包存储 Fuzz 目标（`fuzz_rule_package_store`）

**决策**：实现 `fuzz_rule_package_store`，针对 `streamview::rules::RulePackageStore::importArchive` 与清单有效性校验。

- **输入摄入**：
  - 将输入数据作为不可信的 `.svrule`（ZIP 格式）二进制包；
  - 通过 Qt `QTemporaryFile` 写入临时文件，并调用 `RulePackageStore::importArchive(tempFile.fileName())`；
  - 若归档成功导入，进一步验证 `RulePackageStore::writeArchive` 与 `RulePackageStore::importDirectory` 能够确定性执行且无错误。
- **威胁模型与不变式**：
  1. **Zip Slip 路径遍历防御**：任何包含 `..`、绝对路径、前导斜杠、Windows 盘符或保留字符的路径必须被拒绝（返回 `RulePackageImportStatus::InvalidArchive` 或 `InvalidPackage`），严禁在暂存目录之外提取或写入任何文件；
  2. **Zip Bomb 与资源配额防御**：
     - 归档文件大小超过 `maximumArchiveBytes = 64 MiB` 必须立即拒绝；
     - 未压缩单文件超过 `maximumFileBytes = 8 MiB` 或解压总字节数超过 `maximumTotalBytes = 64 MiB` 必须干净终止；
     - 路径长度超过 `maximumPathBytes = 240` 必须拒绝；
     - 清单文件（`rule.toml`）超过 `maximumManifestBytes = 64 KiB` 必须拒绝；
  3. **清单与语法鲁棒性**：畸形 TOML 键值、循环依赖、非法版本字符串以及字段/节点名包含 `#<数字>`（如 `tag#1`、`entry#0`）的语法树必须安全解析，绝不破坏包身份或路径解析机制；
  4. **零崩溃/零泄漏保证**：损坏的 Central Directory 结构、不支持的压缩算法及 CRC32 不匹配必须返回明确错误状态，严禁崩溃。

---

### 3. 格式探测器与扫描器 Fuzz 目标（`fuzz_format_detectors`）

**决策**：实现 `fuzz_format_detectors`，覆盖 H.264 Annex B、AAC ADTS 与 MP4 box 探测器与批量流式切片扫描器。

- **输入摄入**：
  - 将任意不可信原始字节流（最多 4 MiB）依次注入：
    1. `detectH264AnnexBCandidate` 与 `H264StartCodeScanner::scanBatch`；
    2. `detectAacAdtsCandidate` 与 `AacAdtsScanner::scanBatch`；
    3. `detectMp4Candidate` 与 `Mp4BoxScanner::scanBatch`。
  - 使用由 `std::span<const std::byte>` 支持的内存数据源（`MemorySource`）模拟真实随机访问源，无需磁盘 I/O。
- **威胁模型与不变式**：
  1. **零挂起与有界执行**：
     - 高密度伪同步字风暴（如连续 `0xFF 0xFF` 或 `0x00 0x00 0x01` 模式）与自循环 MP4 box 结构（如 `boxSize = 0`、自引用偏移）必须严格受限于 `defaultWorkBudget()`（64 KiB）与最大批次记录数（256 条），在有限确定性时间内完成；
  2. **零越界内存读取**：
     - 在 `std::span` 与 `core::RandomAccessSource` 上的每次读取必须进行严格边界校验；截断帧或截断 box 必须标记 `truncated = true` 或干净收尾，严禁读取超出 `sourceSizeBytes` 的数据；
  3. **确定性分类**：
     - 对相同输入重复探测必须输出完全一致的置信度评分与扫描记录。

---

### 4. MP4 Sample 提取与索引 Fuzz 目标（`fuzz_mp4_sample_extractor`）

**决策**：实现 `fuzz_mp4_sample_extractor`，针对 `streamview::rules::Mp4SampleTableIndex::build` 与 `streamview::rules::Mp4SampleTableExtractor`。

- **输入摄入**：
  - 将模糊测试字节反序列化为结构化表输入：
    - 变长 run-length 行：`stts`（时间转样本）、`stsc`（样本转 chunk）、`ctts`（合成时间戳偏差）；
    - Chunk 偏移条目（`stco` / `co64`）；
    - 样本大小条目（`stsz` / `stz2`）；
    - 关键帧同步条目（`stss`）。
  - 调用 `Mp4SampleTableIndex::build` 并在边界页（第 0 页、中间页、越界页）上执行 `requestPage`；
  - 同时将序列化 box 注入 `Mp4IsobmffAnalyzer` 并执行 `Mp4SampleTableExtractor::extract`。
- **威胁模型与不变式**：
  1. **整数溢出与回绕防护**：
     - 必须使用带溢出检测的算术运算，防止在 chunk-to-sample 计算（`firstChunk`、`samplesPerChunk`）、时间戳累加（`sampleDelta`）与源区间偏移计算中发生 64 位乘法或加法溢出；发生溢出时必须干净返回 `Mp4SampleTableIndexStatus::ArithmeticOverflow`；
  2. **内存与行数配额熔断**：
     - 声明超出 `maximumRunRows = 65,536` 或表读取预算时必须立即触发 `ResourceLimit` 或 `InconsistentTables`，杜绝因天文数字级别的 entry_count 导致 OOM；
  3. **区间合法性与重叠安全**：
     - 超出 `sourceSizeBytes` 或在码流中存在重叠的样本描述符必须干净返回 `OutOfSourceRange`。

---

### 5. 种子语料组织与 CTest 集成

**决策**：在 `tests/fixtures/fuzz/` 中构建包含合法、边界与病态畸形用例的完备种子语料：
- `tests/fixtures/fuzz/rule_package/`：
  - 10 个种子文件，覆盖合法 `.svrule` 包、zip bomb 嵌套、Zip Slip 路径遍历尝试（`../../etc/passwd`）、损坏 manifest、截断归档及包含 `#<数字>` 字段名的合成 schema；
- `tests/fixtures/fuzz/format_detectors/`：
  - 10 个种子文件，覆盖合法 H.264 NAL 流、合法 AAC ADTS 音频、合法 MP4 头部、高密伪同步字、自循环 box 链、64 位超大 largesize 及 0 字节截断输入；
- `tests/fixtures/fuzz/mp4_sample_extractor/`：
  - 10 个种子文件，覆盖标准 sample table、整数溢出触发器、单样本轨道、巨型 run-length 行、非单调合成时间戳及重叠 chunk 偏移。

**CMake 与 CTest 目标注册**：
- `streamview_fuzz_rule_package_store`
- `streamview_fuzz_format_detectors`
- `streamview_fuzz_mp4_sample_extractor`
- 全部接入 CTest；全量测试用例总数由 56 增至 59。

---

## 影响

### 正面
- 彻底覆盖 DSL 核心引擎之外的不可信二进制摄入攻击面；
- 加固归档解包，防御目录穿越与解压炸弹；
- 保证流式扫描器与候选探测器在损坏、错位或对抗性码流下绝不挂起；
- 实证 `#<数字>` 合成语法树在解包与路径解析时的隔离性；
- 在 Ubuntu、macOS、Windows 上通过 CTest 自动回归。

### 负面
- CTest 用例数增加 3 项；构建与测试执行时间小幅增加（~2-3 秒）。

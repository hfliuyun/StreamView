# StreamView v0.1 分阶段实施计划

Status: In Progress
Current Phase: 5
Last Completed Step: Task P5c-R4 — ADR-0097 final contract & evidence closure (markdown-only + scratch probes; checked coordinates + SourceError; ASCII state table + RunnerExecutionBudget with nodes/instructions; UUID underflow error semantics; EntryStruct static constraints + WindowDecoder session object + full 9-state DslExecutionStatus; probe driver non-zero exit on failure; no code, no rule assets, no hosted run)
Next Action: 主 Agent 复审 P5c-R4；未经复审不得开始 P5d（P5d 仍为 P5c/P5c-R/P5c-R2/P5c-R3/P5c-R4 决策能力的实现切片：P5d-1 DslScannerKind::Mp4Box scanner+detector；P5d-2 @container + @window + available_bytes() + @target_format registration/metadata/cache/resolveByFormat；P5d-3 Mp4IsobmffAnalyzer runner skeleton + container re-entry + shared execution budget + window decoder + D7 continuation/fixture tests）
Last Verification: P5c-R4 — 18 scratch `svtool rule check` probes executed (P1–P9b) via Python driver (exit=0 on all matched, non-zero on failure) with normalized expected errors / Rule OK, including P8d correct UUID wire order (Rule OK), P8e first exposes missing available_bytes gap (`error: Pure function is not declared before this call`, dsl_ir.cpp:772), and P9a/P9b @window name gate rejection (`error: Unknown annotation '@window'`, dsl.cpp:1880); ffmpeg fixture generator commands (regular 6513 bytes, fragmented 4505 bytes) and ffprobe trace/packets normalized excerpts verified; `markdown_hygiene` (CTest #36) passed; `git diff --check` clean; bilingual ADR-0097 symmetry check passed (505 lines, 16 headings, 51 table rows, 22 code fences line-for-line); markdown-only change per ADR-0019, no hosted run produced this round. Prior hosted baselines remain: P5b-R run 31965623068 (macOS job 95210238754, Windows job 95210238781, Ubuntu job 95210238826) on commit 2624276; P5b-R2 run 31969610307 (macOS job 95219881189, Ubuntu job 95219881227, Windows job 95219881244) on commit 7671916
Blockers: None

本文件是实施与恢复入口。英文产品需求、DSL 规范和 ADR 仍是权威设计来源。

## 执行规则

- 每个阶段及步骤使用复选框记录状态。
- 每完成一个阶段，记录验证命令、结果摘要和对应 commit。
- 中断前更新 `Current Phase`、`Last Completed Step`、`Next Action` 和 `Blockers`。
- 发现计划外决策时先暂停，实现前补充 ADR 和中英文文档。
- 不删除已完成记录；需求变化通过追加修订记录追踪。
- 每次 push 前必须先完成本机 Debug、Release、ASan/UBSan 构建与全部测试；CI 专属平台差异仍需由 hosted matrix 验证。

### 纪律条款

1. commit SHA、CI run/job ID 必须是粘贴的命令或 API 原始输出，不得转述或占位；
2. ADR 验证矩阵只允许记录实际执行过的检查，不得写入未运行的命令或不存在的产物路径；
3. 内核/能力行为改动不得搭载在格式消费 commit 中，反向亦然；
4. struct 名称、条款号、测试名与 file:line 必须逐字核对仓库或规范原文，并附核对证据；
5. 每个任务经评审后才派发下一个任务；
6. 新增测试用例默认追加到测试类末尾；若必须中部插入，同一 commit 内修正所有受影响的文档行号引用；
7. 任何使被引用文件行号发生位移的 commit（含 tests/ 与 docs/ 自身），提交前用 grep -n 全量复核所有引用文档中的行号，发现漂移即修正；
8. 历史记录中的 `file:line` 为提交时快照，不随后续测试扩展引发的行号位移追改；仅 ADR、语言参考与实施计划当前状态/待办项（Active State & Todo）需保持实时行号精确对齐。

## 架构与接口

- 模块划分为 C++20/Qt Core 分析核心、DSL/规则运行时、Qt Widgets 应用和内部工具 `svtool`；正式格式只能通过 DSL 实现。
- 核心类型包括源 bit 地址、源区间、逻辑范围、字段位置、分析节点、诊断和物化状态。字段可映射多个源区间；状态区分 lazy、indexing、waiting-dependency、cancelled、unsupported、invalid、materialized。
- SPS/PPS、AudioSpecificConfig 和 sample description 使用带源位置的版本目录；解析时选择当前位置之前最近的有效定义。
- DSL 当前使用手写 lexer、递归下降声明/控制流/受限表达式解析器、静态类型 IR 和受限
  bytecode VM；Pratt 或更通用的表达式解析器留待一般表达式切片。
- 文件接口固定为 `.svfmt`、`.svrule`、`rule.toml` 和 `.svsession`；应用、DSL、官方规则初始版本分别为 `0.1.0`、`0.1`、`0.1.0`。
- SQLite WAL 保存大型索引和物化结果；内存只保留 64 KiB 源页面及节点 LRU，默认预算 128 MiB。
- 单窗口单会话；Qt UI 采用左右 dock、中央自绘原始数据视图、分页分析树和全局诊断面板。
- Qt 基线为 6.11.x，CI 默认固定 6.11.1；Windows 暂时按 ADR-0017 使用 6.10.1，CMake 最低 3.28。FFmpeg 只用于开发验证，不进入运行时。

## 剩余工作重排（2026-07-21）

现有阶段 1–7 仍是范围清单；下面的里程碑是实际执行顺序。每个里程碑都必须产出可运行、可测试的增量，并在完成时回写对应阶段复选框，避免让 GUI、CLI 和规则各自复制一套解析路径。

### M0：恢复点与跨平台基线

- [x] 确认 `2e90d98`、`c316269` 已推送到 `origin/main`。
- [x] 记录 hosted run `29758037457` 的三平台成功结果；Windows Qt 6.10.1 fallback 继续受 ADR-0017 约束。

### M1：统一规则分析入口（runner 与 GUI 接入已完成）

依赖：阶段 1 已完成的 source、coordinates、analysis model、DSL parser/executor 和 start-code scanner。

- [x] 新增官方最小 Annex B 规则资产（`NalUnitHeader`、`h264_start_code`、`entry`）及其加载/校验入口。
- [x] 新增共享 runner：把 scanner record 转成分析树 region，限制 header reader 到 NAL 前 8 bit，并调用 `DslExecutor`；支持批次、取消、空/截断 payload 和 source error 的部分结果。
- [x] 用端到端测试锁定三/四字节 start code、NAL header 字段值、精确 source spans、非法 `forbidden_zero_bit` 和截断诊断。
- [x] GUI 与 `svtool` 只能调用该 runner，不在入口层复制解析逻辑。

验收：合法样例完整 materialized；非法或截断样例保留已发布节点、附 source-located diagnostic，并且所有字段可反查到原始 bit。

### M2：阶段 1 桌面/工具纵切面

依赖：M1 的共享 runner。

- [x] 实现本地文件打开、Annex B 候选检测和会话生命周期；打开失败不得替换当前会话。（本地打开、原子 session 替换、首 64 KiB bounded detector、source-located evidence/confidence 已完成；手动规则选择后续实现。）
- [x] 实现虚拟化 raw hex/binary 视图及统一 source-bit selection，字段选择与原始视图双向同步。（已完成 64 KiB 分页、Hex/Binary/Combined、精确 per-bit 高亮、跨页保留，以及 tree/raw 双向定位。）
- [x] 实现 `QAbstractItemModel` 分页分析树和字段检查器，展示值、宽度、逻辑/绝对坐标、说明、规范引用和诊断。（分析 worker 按有界批次发布完整 NAL 子树；Qt model 使用 append-only `rowsInserted` 保留既有 index；检查器从节点 metadata 与 diagnostics 渲染字段详情。）
- [x] 实现内部 `svtool rule check` 与 `svtool analyze`，输出与 GUI 使用同一 runner；固定用法、诊断和退出码。
- [x] 增加最小 UI/CLI 回归样例，并验证合法、截断和无 start code 输入。

验收：从一个本地 Annex B 文件可完成打开、树/原始视图查看、字段与 bit 双向选择，以及 CLI 文本分析；全部行为不修改源文件。

### M3：DSL v0.1 执行骨架

依赖：M1/M2 暴露出的稳定 rule-runner 接口；先定义类型/IR/预算边界，再逐项扩展语法。

- [x] 建立静态类型 IR 与受限 bytecode/VM 边界，统一错误、资源预算和确定性。（parser 输出 source-oriented model；compiler 生成 declaration-order typed IR 和确定性 bytecode；VM 拒绝 malformed IR，兼容入口保留。）
- [x] 逐项加入 enum、显式 endian、`ue/se`、数组、条件、switch、有界循环、纯函数和 computed fields；每项先补英文规范、中文说明、正反例和 TDD。
  - [x] enum、显式 endian、`ue/se`。
  - [x] 固定长度数组：一维正整数长度，编译期扁平展开，逐元素 metadata/constraint、坐标、诊断和预算。
  - [x] 条件语句：可嵌套的前置 scalar `bits`/enum 等值判断、可选 `else`、branch-aware
    可用性与对齐、guarded typed fields，以及未选分支不读取 source/不创建节点的 VM 语义。
  - [x] switch：前置 scalar `bits`/enum controller、互异整数 case、可选末尾 default、嵌套
    switch/条件、全 arm 静态验证与对齐、case/default guard lowering，以及 selected-only VM
    语义。
  - [x] 有界循环：前置无符号 `bits`/enum/`ue` controller、正整数字面量 maximum、嵌套
    body、静态投影与局部作用域、`count > index` guard、边界断言、selected-only VM 语义，
    以及超限、部分结果、预算、取消和 malformed IR 回归。
  - [x] 纯函数与 computed fields：`bool`/`u64` typed expression、声明顺序纯函数内联、
    计算字段投影与 controller、checked arithmetic、short-circuit、无 source location 节点、
    部分结果、预算和 malformed IR 回归。
- [x] 固化调用/视图深度 64、节点深度 256、单次物化 100,000 节点和每 1,024 指令取消检查。（另固化单次结构 1,000,000 指令预算；当前最小子集没有 runtime call 或 view，预算已由 VM/API 保留。）

验收：稳定子集按声明顺序生成相同 typed IR/bytecode 和结果；超限与取消保留部分树并附可定位诊断，Annex B runner 在创建时编译一次规则，`svtool rule check` 同时执行 parser/compiler。

### M4：映射、lazy 与大型文件底座

依赖：M3 的运行时边界。

- [x] 实现 mapping-preserving EBSP→RBSP、excluded span、lazy boundary 和可恢复 progressive index。
  - [x] 完成 source-mapped logical read 基座：mapped reader、执行前 backing/mapping 校验、
    multi-span 字段/诊断、mapped Exp-Golomb 与 logical-byte little-endian。
  - [x] 实现 bounded H.264 EBSP→RBSP mapping 与 excluded-span tree presentation。
  - [x] 实现 lazy boundary。
  - [x] 实现可恢复 progressive index。
- [x] 实现位置感知上下文目录，支持按源位置选择最近有效 SPS/PPS/ASC/sample
  description。
- [x] 以稀疏/虚拟 100 GB 源验证初始打开、已知 offset 读取、批次发布、取消和恢复。
- [x] 定义并实现 SQLite WAL 分页缓存、批次提交、schema 版本和崩溃恢复。

验收：内存不随源大小线性增长，跨排除字节的字段仍能返回多个 source spans。

### M5：规则分发、身份与持久化

依赖：M3 的语言/引擎版本契约和 M4 的稳定 source/rule identity。

- [x] 先固定 TOML manifest、content hash、兼容范围和 rule catalog，再实现目录导入与 `.svrule` deterministic ZIP。
- [x] 拒绝绝对路径、parent traversal、重复/非规范路径、符号链接和 zip bomb；安装内容按 hash 只读保存。
- [x] 完成 source fingerprint、SQLite cache namespace、owner payload 与 `.svsession` 的持久身份链和恢复管线。
  - [x] 固定并实现 version 1 本地文件 source fingerprint：小文件全文 SHA-256；大文件绑定
    size、纳秒 mtime 与首/中/尾各 1 MiB SHA-256；计算期间变化显式失败。
  - [x] 固定 durable cache namespace 与 version 1 payload envelope，绑定 source fingerprint、
    完整 rule entry-point identity、SQLite schema、envelope 与两类 payload version。
  - [x] 实现 progressive-index 与 materialized-result owner payload body serializer。
  - [x] 实现 typed background cache owner：dedicated owner thread、有界 request/byte queue、
    exact-key read stack、atomic write batch、flush 与 draining shutdown。
  - [x] 实现 `.svsession` version 1 闭合 JSON、`QSaveFile` 原子保存、同句柄 fingerprint
    校验与完整 rule entry-point identity 精确恢复；cache page 保持在 session 文件外。
  - [x] 从 `AnalysisSession` 派生同句柄 namespace 并提交 stable progressive/materialized page；
    cache failure 只关闭可选加速，不替换有效 session。

验收：规则版本冲突、源变化和损坏包均显式诊断；旧会话不会静默绑定新源或新规则。

### M6：正式格式增量

依赖：M4 的映射/上下文和 M5 的规则资产管理。每个格式按“规则、fixture、诊断、双语字段文档、source-span 断言”独立验收。

- [x] H.264：EBSP/RBSP、trailing bits、SPS/PPS/VUI/HRD、slice header、SEI 与按位置重定义。
- [x] AAC-LC：ADTS、AudioSpecificConfig/GASpecificConfig/PCE，压缩 payload 保持 opaque。（T18-R 已补齐 Unsupported profile 语义、全字段逐 bit 验收与 CRC 边界说明；本地 dev/ci/sanitize 36/36。）
- [ ] MP4/MOV：box 层级、sample tables、`avcC`/`esds`、分页 sample index 与跨层导航；对 `moof` 明确 unsupported。

### M7：安全、性能与发布门禁

依赖：M2–M6 的稳定接口和可分发样例。

- [ ] fuzz lexer/parser/VM、映射、规则包和官方规则；三平台静态检查，Linux/macOS ASan/UBSan。
- [ ] 验证约两秒初始视图、100 GB RSS 不超过 512 MiB、已知 offset p95 小于 100 ms。
- [ ] 生成并校验 Windows ZIP、macOS `.app.zip`、Linux AppImage、SHA-256、SBOM 和许可证材料，再按 alpha/beta/rc/v0.1.0 发布。

## 阶段 0：持久化计划与工程基线

- [x] 将本计划写入 `docs/implementation-plan.md`。
- [x] 初始化 Git、MIT License、`.gitignore`、双语 README、CMake Presets 和代码规范。
- [x] 建立核心、规则运行时、应用、内部 CLI 和测试目标。
- [x] 建立 Windows 2022/MSVC、macOS 15 ARM64/Apple Clang、Ubuntu 24.04/GCC CI。
- [x] 固定规范基线：ITU‑T H.264 (08/2024)、ISO/IEC 14496‑3:2019、14496‑12:2026、14496‑15:2024+Amd1:2025、14496‑1:2010。
- [x] 验证三个平台均能构建、测试、启动并生成空应用包。

## 阶段 1：H.264 NAL 端到端纵切面

- [x] 实现严格只读随机访问源、bit reader、源/逻辑坐标和多区间映射。
- [x] 实现节点、诊断、部分结果与取消模型。
- [x] 实现最小 DSL：结构、1–64 bit 字段、注解、入口和渐进 start-code 扫描。
- [x] 编写 Annex B 规则，解析 start code 和 NAL header。
- [x] 完成文件打开、格式检测、分析树、hex/binary 视图、字段检查器和双向 bit 选择。（文件打开、候选检测、分页 raw/tree 视图、增量树发布、字段元数据检查器和双向 bit 选择均已完成。）
- [x] 提供内部 `svtool rule check` 与 `svtool analyze`。
- [x] 验证合法和截断 H.264 均能显示精确字段与部分结果。

## 阶段 2：DSL v0.1、沙箱与大型文件基础设施

- [x] 完成枚举、显式大小端、`ue/se`、数组、条件、switch、有界循环、纯函数和计算字段。（枚举、显式大小端、`ue/se`、固定长度数组、条件语句、switch、有界循环、纯函数和计算字段已完成。）
- [x] 完成 mapped transformation、lazy 区域、渐进索引和位置感知上下文目录。
  （四项均已完成。）
- [x] 完成 bytecode 预算和取消：调用/视图深度 64、节点深度 256、单次物化 100,000 节点，每 1,024 指令检查取消。（当前子集没有 runtime call 或 view；对应深度预算已经保留。）
- [x] 完成 SQLite 分页缓存、原子批次提交、schema 版本和崩溃恢复。
- [x] 在 M5 固定 source fingerprint、精确 rule identity 与 cache namespace 后，接入
  后台 cache owner 和 versioned index/materialized-result payload。（typed background owner 与
  versioned body、`AnalysisSession` stable write path 均已完成；cached snapshot 与 live analyzer
  recovery 未实现。）
- [x] 完成 TOML 清单、本地规则目录导入、`.svrule` 打包安装、哈希和版本并存。
- [x] 防御路径穿越、zip bomb、符号链接和 Unicode 非规范路径。
- [x] 为全部稳定 DSL 功能补齐英文规范、中文说明和正反例。
- [x] 验证 100 GB 虚拟/稀疏源可快速打开、渐进索引、取消和恢复。（自动化验证
  有界访问和恢复语义；约两秒、RSS 与 p95 性能门禁仍留在阶段 7 实测。）

## 阶段 3：H.264 正式结构支持

- [x] 完成 Annex B、EBSP→RBSP、trailing bits、SPS、PPS、VUI/HRD。
  - [x] payload 派发与 access unit delimiter、end of sequence、end of stream 的
    RBSP 与 trailing bits。
  - [x] 有界 SPS、PPS 与 VUI core，以及 layout-critical unsupported branch 和
    non-fatal value-domain constraint。
  - [x] 有界 HRD parameters。
- [x] 完成 Baseline/Main/High 8-bit 4:2:0 slice header；slice data 标记为压缩载荷。
- [x] 所有 SEI 解析 payloadType/payloadSize。
- [x] 深入解析 buffering period、pic timing、用户数据、recovery point、frame packing 和 display orientation。
- [x] 支持同 ID SPS/PPS 中途重定义和按位置选择。
- [x] 为声明范围内每个 source-backed 字段建立局部规范引用和双语说明；按 NAL、SPS/VUI/HRD、PPS、slice 与 SEI 语法族提供代表性的合法/非法样例和 source-span 断言。

## 阶段 4：AAC-LC 正式结构支持

- [x] 解析 ADTS fixed/variable header、frame length、buffer fullness、raw block count 和 CRC 字段布局；不包含 CRC-16 算术校验。
- [x] 解析 AudioSpecificConfig、GASpecificConfig 和 Program Config Element。（验收：ADR-0094 9 项测试矩阵 + resolvesAscEntryPointFromBundledRulePackage，本地 dev/ci/sanitize 35/35，hosted run 31928187049 全平台通过）
- [x] 将 `raw_data_block` 整体标记为压缩载荷，不隐藏实现 Huffman 解码。（验收：ADR-0095 §2 `@lazy raw_data_block` 规则声明，当前包 v0.1.4；`decodesAdtsHeaderBitByBitRangesAndZeroLengthPayload` 验证无/有 CRC 时 payload 从 bit 56/72 起，非零 payload 为 `MaterializationState::Lazy`。）
- [x] 对 HE-AAC、ELD 和其他 profile 明确报告部分识别或不支持。（T18-R 验收：通用 `unsupported("reason") at field;` 保留已解码前缀并产生 `UnsupportedSyntax`；ASC AOT 5/29/39 分别由 `reportsAscNonGaAot5SbrAsUnsupported`、`reportsAscNonGaAot29ParametricStereoAsUnsupported`、`reportsAscNonGaAot39EnhancedLowDelayAsUnsupported` 锁定，且不再误解码 GA/PCE 后缀。）
- [x] 验证 ADTS、ASC、截断、CRC 字段存在/位置/截断和不支持 profile 的逐 bit 结果。（T18-R 验收：ADR-0095 §6；`decodesAdtsHeaderBitByBitRangesAndZeroLengthPayload`、`decodesAscAndPceFieldsBitByBit`、`verifiesTruncatedFramesLogicalRangesAndDiagnosticLocations`；CRC 测试值 `0x1234` 仅证明原样解码，不代表校验正确。）

## 阶段 5：非分片 MP4/MOV 与跨层导航

### 目标与交付物
支持标准非分片 MP4/ISOBMFF 容器解析、元数据树物化、超大 `mdat` 惰性封装、样本表索引分页以及 `avcC`/`esds` 到 H.264/AAC 基本流的跨层导航。

### 阶段 5 任务切片与依赖关系
- **Task P5a**（规范）：双语 ADR-0096 架构设计与边界决策定义（Markdown-only）；
- **Task P5b**（能力切片）：DSL 编译器未识别注解编译闸门（加固消除 N2 隐患，严格报错非法注解）；
- **Task P5c**（规范切片）：ISOBMFF 容器语言原语探测与 ADR-0097（`size==0` 尾部跨度、`@target_format` 宿主位置、Box 序列作用域）；
- **Task P5d**（能力切片）：容器语言能力实现（运行器与解析器原语）；
- **Task P5e**（规则切片）：官方 MP4 规则包 `org.streamview.mp4` v0.1.0（顶层 Box 遍历、`ftyp`、`mdat` lazy 封装）；
- **Task P5f**（规则切片）：`moov` 容器层级规则 v0.1.1（`moov`、`trak`、`mdia`、`minf`、`stbl`）；
- **Task P5g**（规则切片）：样本表索引分页规则 v0.1.2（`stts`、`stsc`、`stsz`、`stco`、`co64`）；
- **Task P5h**（规则切片）：编解码配置 Box 规则 v0.1.3（`stsd`、`avc1`、`avcC`、`mp4a`、`esds` + `@target_format`）；
- **Task P5i**（能力切片）：跨层导航会话与坐标视图集成；
- **Task P5j**（验收与审计切片）：逐 bit 验收审计、超大 `mdat` 惰性验证与阶段 5 里程碑关闭。

### 阶段 5 检查清单
- [ ] 支持普通、64 位、size=0 和未知 box；`mdat` 默认 lazy。
- [ ] 实现 `ftyp`、movie/track/media 层级、sample descriptions、时间与 sample tables、编辑列表。
- [ ] 实现 `avcC`、`esds`、AVC/AAC sample entry。
- [ ] 根据 `stsc`、`stsz`、`stco/co64` 建立分页 sample 索引。
- [ ] 从 MP4 sample 进入 H.264/AAC 规则，并可返回容器字段。
- [ ] 对 `moof` 明确报告 fragmented MP4 不在首版范围。
- [ ] 使用参考工具交叉验证 sample offset、时间戳和关键帧。

## 阶段 6：会话、规则管理和桌面体验

- [x] 使用版本化 JSON 与 `QSaveFile` 实现 `.svsession` 原子保存。
- [x] 保存源身份、规则精确版本/哈希、书签、注释、展开路径和视图状态。
- [x] 大文件指纹使用大小、纳秒 mtime、首/中/尾各 1 MiB SHA-256；小文件全文哈希。
- [ ] 实现保存/另存为、未保存关闭提示、格式手动覆盖和规则版本管理。
- [ ] 完成进度、取消、诊断汇总、明暗主题和中英双语切换。
- [x] 验证源变化不会误绑定，旧会话按 package ID/version/content hash/entry-point ID
  精确恢复，且 missing/conflicting/incompatible rule 都显式失败。

## 阶段 7：安全、性能与发布

- [ ] fuzz DSL lexer/parser/VM、规则包、映射和官方规则。
- [ ] Linux/macOS 运行 ASan/UBSan；三平台运行静态检查与完整测试。
- [ ] 验证初始视图约两秒可用、100 GB 测试源 RSS 不超过 512 MiB、已知 offset 页面读取 p95 小于 100 ms。
- [ ] tag 自动生成 Windows x64 ZIP、macOS ARM64 `.app.zip` 和 Linux x86_64 AppImage。
- [ ] 发布包附带 SHA-256、SBOM、MIT/Qt/第三方许可证和 Qt 源码获取说明。
- [ ] 依次发布 `v0.1.0-alpha`、`beta`、`rc`，最终发布 `v0.1.0`。

## 测试与阶段门禁

- 单元测试覆盖位序、大小端、跨防竞争字节的多区间映射、溢出、截断、lazy 边界、上下文重定义和依赖失效。
- DSL 测试覆盖语法、类型、资源限制、取消、确定性和三平台相同结果。
- 规则包测试覆盖路径穿越、zip bomb、版本冲突和不兼容 DSL。
- 格式测试使用 Python 生成的最小结构样例、小型可分发真实样例和变异损坏样例；FFmpeg/ffprobe 仅作为测试参考。
- UI 测试覆盖分页树、bit 联动、高 DPI、中英文、保存提示和源变化警告。
- 一个阶段只有在测试通过、双语文档更新、计划文件记录验证证据后才能标记完成。

## 明确延期项

- 网络和实时输入、播放、编辑回写。
- CABAC/CAVLC 与 AAC Huffman 频谱解析。
- fragmented MP4、MPEG-TS、Matroska/WebM。
- 内置 DSL 编辑器、正式公共 CLI、在线规则市场和自动更新。
- JSON 导出不阻塞 v0.1.0。
- Windows 签名和 macOS notarization 后置。

## 执行记录

- 2026-07-19：计划已批准并持久化。下一步初始化工程基线。
- 2026-07-19：Git/CMake/Qt 工程骨架、测试和 CI 工作流已建立；Qt 6.11.1 本机 Debug/Release 构建、3 项测试与 macOS 部署树验证通过。等待配置 GitHub remote 后运行三平台门禁。
- 2026-07-19：阶段 0 基线已提交为 `5732f31`（`chore: establish StreamView project baseline`）。
- 2026-07-19：已配置并推送 GitHub remote；`18ab02f` 的 Actions 矩阵中 macOS 通过，Windows 与 Ubuntu 失败。当前环境无法读取 GitHub 日志，等待失败步骤日志后继续修复。
- 2026-07-19：根据下载的日志确认 Windows 因 Qt 仓库元数据获取失败而中止，Ubuntu 构建及 3/3 测试通过但相对安装前缀不满足 Qt 6.11 部署要求。Windows 安装已增加有限重试，三平台部署统一改用绝对前缀；本机 3/3 测试及 macOS 部署树回归通过，等待 hosted CI 重跑。
- 2026-07-19：确认 Qt 官方 Windows `qt6_6111` 元数据为 404，而 `qt6_6101` 提供 MSVC 2022 64 位包。按新增 ADR-0017 及中英文说明，Windows CI 暂时显式使用 6.10.1；产品与开发基线仍为 Qt 6.11.x，待上游发布后恢复 Windows 6.11.1。
- 2026-07-19：运行 `29690756262` 确认 Windows Qt 6.10.1 安装成功，但 Configure 失败。新增 Windows 专用 Configure 步骤，直接传入 runner 上的 `Qt6_DIR` 与 `CMAKE_PREFIX_PATH`，避免依赖跨步骤环境变量解析。
- 2026-07-19：显式 Qt 路径修复已提交为 `355ed68`；运行 `29690948648` 的 Ubuntu/macOS job 通过，Windows 仍在 `Configure (Windows)` 失败。公开 API 仅提供失败步骤而不提供原始日志，等待最新 Configure 日志后继续。
- 2026-07-19：最新日志确认 Windows CMake 选中了 MinGW GNU 14.2，而 Qt 包为 MSVC 2022；workflow 增加固定 SHA 的 `ilammy/msvc-dev-cmd` x64 环境步骤，使 Configure、Build、Test 和 Install 使用同一 MSVC 工具链。
- 2026-07-19：MSVC 环境修复已提交为 `06b776b`；运行 `29691377216` 的 Ubuntu/macOS job 通过，Windows 仍在 `Configure (Windows)` 失败，等待新日志确认编译器选择或后续 CMake 错误。
- 2026-07-19：新日志确认 MSVC 19.44 与 `cl.exe` 已正确选中；失败原因是默认 `find_package(Qt6 6.11)` 拒绝 CI fallback 的 6.10.1。ADR-0017 已补充：CMake 默认最低版本保持 6.11，仅 Windows CI 可显式覆盖为 6.10，发布构建不得使用该覆盖。
- 2026-07-19：实现 `STREAMVIEW_MINIMUM_QT_VERSION`，默认值为 6.11；Windows CI 显式覆盖为 6.10。默认配置、6.10 override 配置、Release 构建、3/3 测试和绝对前缀部署均在本机通过，等待 hosted matrix 重跑。
- 2026-07-19：阶段 0 完成。实现提交 `1554e3b` 对应 hosted run `29691705979`；Windows 2022/MSVC、macOS 15/Apple Clang、Ubuntu 24.04/GCC 的 Build、3/3 Test、Install 和 Upload 全部通过，`streamview_version` 覆盖 Qt 应用运行时启动。Windows Qt 6.10.1 临时 fallback 继续按 ADR-0017 追踪。
- 2026-07-19：阶段 1 第一项已在本机实现：严格只读随机访问文件源、MSB-first 1–64 bit reader、source/logical 坐标、字段位置和多 source-span mapping；新增中英文核心模型文档以及溢出、截断、跨排除字节映射和只读文件测试。Debug、Release、ASan/UBSan 均为 6/6 测试通过；本机未安装 `clang-format`，已执行 diff 与 100 列机械检查。
- 2026-07-19：阶段 1 第一项完成并提交为 `23ac7dd`；hosted run `29692399047` 的 Windows、macOS、Ubuntu 三平台 Build、6/6 Test、Install、Upload 全部通过。
- 2026-07-19：阶段 1 第二项已在本机实现：append-only 分析树、稳定 snapshot、节点状态转换、source-located diagnostics、部分结果保留和 C++20 cancellation token/source；新增中英文分析模型文档及状态/取消测试。Debug、Release、ASan/UBSan 均为 8/8 测试通过。
- 2026-07-19：提交 `1acb8d4` 的 hosted run `29693108274` 中 Ubuntu 与 Windows 通过，macOS 在 Build 失败。虽然 push 前本机 Debug/Release/ASan 均为 8/8，通过用户要求进一步固定“本机完整门禁后再 push”的执行规则，等待 macOS Build 日志定位 runner 工具链差异。
- 2026-07-19：macOS Build 日志确认固定 runner 的 Apple libc++ 未提供 C++20 `std::stop_token`/`std::stop_source`，而本机 Apple Clang 21 已提供，因此产生 CI-only 编译失败。ADR-0018 决定保持取消 API 行为不变，底层改用引用计数共享状态与原子标志；等待完整本机门禁验证后再提交和 push。
- 2026-07-19：可移植取消状态已通过本机 Apple Clang 21 的 Debug、Release、ASan/UBSan 完整配置与构建；三套配置均为 8/8 测试通过，包含跨线程观察、幂等请求和 token 生命周期回归。按本机门禁规则，可以提交并 push 后运行 hosted 三平台矩阵。
- 2026-07-20：尝试创建修复提交时，当前 Codex 会话因 `.git` 元数据只读而无法创建 `index.lock`；源码与文档均已完成，本机三套门禁仍为 8/8，通过恢复 Git 写权限或由用户执行提交后继续 hosted CI 验证。
- 2026-07-20：阶段 1 第二项完成。可移植取消修复提交为 `1f2af3d`；hosted run `29694921048` 的 Windows 2022、macOS 15 ARM64、Ubuntu 24.04 三平台 Build、8/8 Test、Install、Upload 全部通过。上一条 Git 元数据只读阻塞已由用户提交并推送解除，下一步实现最小 DSL 与渐进 start-code 扫描。
- 2026-07-20：阶段 1 第二项检查点提交前，本机重新完成 `cmake --preset dev/ci/sanitize`、三套构建和三套 `ctest`；Debug、Release、ASan/UBSan 均为 8/8 测试通过。计划记录可提交并推送。
- 2026-07-20：计划检查点内容已完成，但当前 Codex Git 写入审批服务返回 HTTP 503，无法执行 `git add`；等待用户代为提交并推送该文档，或审批服务恢复后继续。
- 2026-07-20：用户已提交并推送阶段 1 第二项计划检查点 `4e2782a`。根据用户要求，ADR-0019 记录 Markdown-only 提交跳过 hosted CI；混合代码与文档的提交仍运行完整矩阵。
- 2026-07-20：阶段 1 第三项已在本机实现：手写 lexer、递归下降 parser、静态最小 IR、`@equals` 约束执行器、direct source mapping 字段物化，以及支持三/四字节前缀、64 KiB 窗口跨界、分批和取消的 `h264_start_code` scanner。Debug、Release、ASan/UBSan 均为 11/11 测试通过；待提交后验证 hosted 矩阵。
- 2026-07-21：用户已将阶段 1 第三项拆分为两笔本地提交：`2e90d98`（Markdown-only CI 跳过）与 `c316269`（最小 DSL/执行器/start-code scanner）。本机 Debug/Release/ASan/UBSan 再次 11/11 通过；下一步 push 并由 hosted 矩阵验证 DSL 提交。
- 2026-07-21：已确认 `c316269` 位于本地与远端 `main`；hosted run `29758037457` 的 Windows 2022/Qt 6.10.1、macOS 15/Qt 6.11.1、Ubuntu 24.04/Qt 6.11.1 Build/Test/Install/Upload 全部通过。阶段 1 第三项跨平台门禁完成。
- 2026-07-21：剩余工作按 M0–M7 依赖重排；本机完成 bundled `h264_annex_b.svfmt`、scanner→DSL→analysis-tree 共享 runner、source-located partial diagnostics、oversized-source 防护，以及内部 `svtool rule check`/`analyze`。合法、非法 forbidden bit、空 NAL、header I/O failure、取消、无 start code 和 CLI 进程路径均有回归测试。Debug、Release、ASan/UBSan 三套完整配置、构建与 CTest 均为 13/13；代码尚未 push，等待后续 hosted matrix。
- 2026-07-22：确认 `911ce28` 已加入 File > Open 与最小分析树后，继续完成首个 M2 session/raw-data 纵切面：新增 64 KiB `SourcePager`、原子 `AnalysisSession`、可复用 `StreamView::App` target、分页 Hex/Binary/Combined raw view 及窗口/UI 回归。打开或首 raw page 失败不会替换当前会话；合法、截断、无 start code 和模式切换均有 UI 测试。本机 Debug、Release、ASan/UBSan 三套重新配置、完整构建与 CTest 均为 17/17；本轮增量尚未提交。
- 2026-07-22：上一条 M2 session/raw-data 增量已拆分为 `7d6df25`（core pager）、`2763581`（app raw workspace）和 `a8043e4`（进度文档）；用户确认对应 hosted CI 已成功，`main` 与 `origin/main` 同步。
- 2026-07-22：完成 M2 Annex B bounded candidate detection：rules 层最多检查首 64 KiB，发布 start-code/NAL-header source evidence 与 weak/probable/strong confidence；`AnalysisSession` 复用首 raw page 保存报告，未命中不拒绝未知 source。规则与 session 实现分别提交为 `e2f2c90`、`5057f37`；本机 Debug、Release/CI、ASan/UBSan 完整构建与 CTest 均为 18/18，双语格式语言契约同步更新。
- 2026-07-22：完成 M2 unified source-bit selection：core 按树深度、source coverage 和稳定 NodeId 确定性解析最具体 materialized 节点；raw view 以 MSB-first 八段精确命中/绘制 bit，多 source span 与跨页高亮保持；`MainWindow` 统一 selection 写入并以 `QSignalBlocker` 实现无回写环的 tree/raw 双向定位，成功 session 替换清除选择而失败替换完整保留。实现拆分为 `58e6736`（core resolver）、`4a65439`（raw bit view）和 `4d25384`（bidirectional synchronization）。本机 Debug、Release/CI、ASan/UBSan 完整配置、构建与 CTest 均为 20/20；Hex/Binary/Combined 实际 Qt 渲染点验无文字遮挡，英文规范与中文伴随文档同步更新。下一步发布增量 analysis-tree update 并实现 field inspector。
- 2026-07-23：完成 M2 最后一项：scanner/analyzer/session 以默认 64 KiB inspected-position work budget 分批推进并暴露 scan cursor；`AnalysisTreeModel` 按完整 NAL 子树 append-only 发布 `rowsInserted`，保留既有 `QModelIndex` 与 selection；`FieldInspector` 展示值、类型、逻辑宽度、source spans、逻辑范围、说明、规范引用和诊断，raw/tree 两条选择路径均显式同步。功能提交拆分为 `a1958bc`（field presentation metadata）、`e2cd5a2`（bounded analysis batches）和 `05c73a9`（incremental desktop workspace）。英文规范与中文伴随文档同步更新；本机 `dev`、`ci`、`sanitize` 完整构建与 CTest 均为 21/21 通过。下一步进入 M3 静态类型 IR 与受限执行边界。
- 2026-07-23：完成 M3 第一切片：`DslCompiler` 把 parser model 编译为 declaration-order typed IR 和确定性 bytecode；`DslVirtualMachine`/`DslExecutor` 实现指令、节点、深度、取消和部分结果边界；重复 `@equals`、约束越界和 malformed bytecode 有确定性诊断；Annex B analyzer 创建时 compile-once 并保存 resolved structure index；`svtool rule check` 同时执行 parser/compiler。实现拆分为 `fdb2b18`（typed IR/bytecode VM 与预算/取消测试）、`9b30a5c`（Annex B compile-once runner 与 ResourceLimit UI 状态）、`7ba5a0f`（compiled-rule CLI check 与回归）和 `4d8c273`（拒绝超出沙箱契约的 VM 预算）。英文规范与中文伴随文档同步记录 IR/VM 契约和当前默认预算；本机 `cmake --preset dev/ci/sanitize`、三套 build preset 与三套 `ctest` 均通过 22/22。提交后等待 hosted matrix 验证，下一步实现 enum 与显式 endian。
- 2026-07-23：确认 M3 第一切片检查点 `5fd236a` 的 hosted run `29990574802` 成功；Windows 2022/Qt 6.10.1、macOS 15/Qt 6.11.1、Ubuntu 24.04/Qt 6.11.1 的 Build、22/22 Test、Install 和 Upload 全部通过。
- 2026-07-23：完成 M3 enum 与显式 endian 切片：parser/AST 支持 declaration-order enum、`bits<N, big|little>` 和 `@enum(Type)`；compiler 解析 typed enum/endian、命名空间、宽度及字节对齐；VM 在不改变 MSB-first source coordinate 的前提下解释 8–64 bit 小端值，并对未知 enum 值、未对齐 source 及 malformed typed IR 保留部分结果和可定位诊断。实现拆分为 `0b69530`（enum/endian parser 与 typed IR）和 `c30f28a`（VM 数值语义与运行时回归）；英文规范、中文伴随说明及正反例已同步。本机 `cmake --preset dev/ci/sanitize`、三套 build preset 与三套 `ctest` 均通过 22/22。下一步实现 `ue/se`。
- 2026-07-23：完成 M3 `ue/se` 切片：parser/AST 接受 `ue` 与 `se`；compiler 生成 `UnsignedExpGolomb`/`SignedExpGolomb` typed fields 和独立 read opcodes；VM 实现 H.264 Exp-Golomb 解码、signed 映射、63 个前导零/127-bit 码字上限、事务式 reader 回滚、部分结果保留、source-located diagnostics 及有符号/无符号节点值。实现拆分为 `a67099e`（parser/IR）和 `9b288c3`（VM 与运行时回归）；英文规范、中文伴随说明及正反例已同步。本机 `dev`、`ci`、`sanitize` 配置、完整构建与 CTest 均为 22/22；hosted run `30002644468` 验证 `a67099e` 三平台成功，hosted run `30003006906` 验证 `9b288c3` 三平台成功。下一步实现固定长度数组。
- 2026-07-23：完成 M3 固定长度数组切片：parser/AST 接受 scalar 字段后的一维正整数字面量长度；compiler 在 99,999 字段结构上限内展开 `name[index]` typed fields 和逐元素 bytecode，并按数组总宽计算静态 endian 对齐；既有 VM 逐元素保留 metadata、`@enum`、`@equals`、source location、诊断和预算语义，截断、约束或资源超限保留此前元素。实现拆分为 `1b5f07b`（parser/compiler、展开上限与 typed IR 回归）和 `208b37d`（executor 的 bits/little/ue/se/enum/partial-results/budget 回归）；英文规范、中文伴随说明及正反例已同步。本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 22/22；hosted run `30008470576` 与 `30008968266` 均在 Windows、macOS、Ubuntu 成功。下一步实现条件语句。
- 2026-07-27：完成 M3 等值条件切片：parser/AST 接受可嵌套的 `if (previous_field == integer) { ... } else { ... }`，`else` 可省略；parser/compiler 拒绝未知、未来、数组、`ue/se`、超宽或当前路径不可用的 controller，按分支合并静态 offset，并把所有可能字段降低为 declaration-order guarded typed fields。VM 在 read 前验证并短路 guard，未选字段不读取 source、不创建节点、不执行 enum/`@equals` 数值检查，但其 read/assert instruction 仍计预算和取消点；malformed guard 与藏在未选分支中的非法 field definition 均被拒绝。实现拆分为 `158c674`（parser/compiler 与静态回归）和 `e023e1a`（VM 与真假分支、嵌套数组、enum、little-endian、`ue/se`、截断、约束、预算及 malformed IR 回归）；英文规范、中文伴随说明、ADR-0020 和正反例同步。本机 `dev`、`ci`、`sanitize` 完整配置、构建与 CTest 均为 22/22；hosted run `30269316726` 与 `30270854648` 均在 Windows、macOS、Ubuntu 成功。下一步实现 switch。
- 2026-07-27：完成 M3 等值 switch 切片：parser/AST 接受可嵌套的 `switch (previous_field)`、一个或多个互异整数 `case` 和可选的末尾 `default`，每个 arm 使用显式花括号 body；parser/compiler 拒绝未知、未来、数组、`ue/se`、超宽或路径不可用 controller，以及重复 case、缺少 case、重复或非末尾 default，并从同一入口 offset 验证所有路径。compiler 把 case 降低为正向等值 guard，把 default 降低为全部 case guard 的否定合取；无 default 时静态合并包含未匹配空路径，不新增 opcode。executor 回归覆盖 case/default/无匹配、嵌套条件、enum、小端、数组、`ue/se`、截断、`@equals`、预算、malformed guard 和未选 arm 中的非法字段。实现拆分为 `7d37025`（parser/compiler 与静态回归）和 `0cc6c5d`（executor 回归）；英文规范、中文伴随说明和 ADR-0021 同步。本机 `dev`、`ci`、`sanitize` 完整配置、构建与 CTest 均为 22/22；hosted run `30274571076` 与 `30275735715` 均在 Windows、macOS、Ubuntu 成功。下一步实现有界循环。
- 2026-07-27：完成 M3 有界 repeat 切片：parser/AST 接受可嵌套的 `repeat (count_field, maximum) { ... }`，controller 为此前且当前路径保证存在的无符号 scalar `bits`、enum 或 `ue`，maximum 为正整数字面量；compiler 按 maximum 静态投影 body，使用 `count > index` guard、repeat-local scope、索引化名称和 statement-position bound assertion，保持线性 bytecode 并把全部投影计入 99,999 字段上限。VM 在消费 body 前拒绝超过 maximum 的计数，缺席迭代不读取 source 或创建节点，但其 read/assert 指令仍计预算和取消点；回归覆盖嵌套 `ue` controller、小端、数组、metadata、`ue/se`、enum、截断、`@equals`、预算、执行中取消和 malformed IR。实现拆分为 `c00ba9b`（ADR-0022 与切片决策）、`4800f41`（parser/compiler/VM）和 `4875f42`（executor 深度回归）；英文规范、中文伴随说明和正反例同步。本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 22/22；hosted run `30281017826` 与 `30282525878` 均在 Windows、macOS、Ubuntu 成功。下一步定义并实现纯函数与 computed fields。
- 2026-07-28：完成 M3 纯函数与计算字段切片：parser/AST 接受顶层 expression-bodied `pure bool/u64` 函数、最多 16 个 typed parameter、结构内 `computed<bool/u64>`、完整受限表达式优先级和 `if (computed_bool)`；compiler 按声明顺序 type-check 并静态内联 pure call，把 bounded typed expression、guard、repeat scope、零 bit 对齐和计算 controller 降低到 `evaluate-computed`。VM 预验证表达式和 controller，执行 checked arithmetic、short-circuit 与 bool/u64 物化；false guard 不求值但 instruction 仍计预算，成功节点为无 `FieldLocation` 的 `ComputedField`，运行时算术或 computed repeat bound 失败保留部分结果并报告无 location 诊断。决策与实现拆分为 `2c0fee4`（ADR-0023）、`39187e3`（parser/compiler）和 `100d98d`（VM/executor）；英文规范、中文伴随说明及正反例同步。本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 22/22；hosted run `30288820605` 与 `30290768155` 均在 Windows、macOS、Ubuntu 成功。M3 语言切片闭合，下一步进入 M4 mapping-preserving EBSP→RBSP、excluded spans、lazy boundary 和 recoverable progressive index。
- 2026-07-28：完成 M4 source-mapped logical read 基座：`BitReader` 支持完整 mapping 与 logical slice backing，跨 source gap 以 MSB-first 读取 `1..64` bit，后段 source failure 保持事务性，并公开 logical length 与 normalized spans；VM 在执行 bytecode、创建节点或读取 source 前精确验证 reader backing 与 mapping slice，字段和诊断可保留多个 forwarded source spans，Exp-Golomb 与 logical-byte little-endian 可跨 gap 执行，后续非对齐 segment 不影响小端值。决策与实现拆分为 `7e4c794`（ADR-0024）、`ac172cb`（core mapped reader）和 `8ef6973`（VM/executor）；本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 22/22，hosted run `30294797317` 与 `30296371878` 均在 Windows、macOS、Ubuntu 成功。下一步定义并实现 bounded H.264 EBSP→RBSP mapping 与 excluded-span tree presentation。
- 2026-07-28：完成 M4 bounded H.264 EBSP→RBSP mapping 与 Annex B excluded-span tree presentation：`fec671c` 以 ADR-0025 固化 clause 7.3.1 transformation、clause 7.4.1 conformance、有限输出和失败前缀语义；`9d78124` 新增有界、可恢复且不复制 payload 的 mapper；`ef3a432` 让 scanner 在既有单向扫描中把最大 `trailing_zero_8bits` run 从 NAL payload 拆出；`87f6849` 在 analyzer 中按独立 mapped-byte budget 渐进映射，并按 `start_code`、`NalUnitHeader`、`rbsp_payload`、excluded bytes、trailing zeros 的顺序发布完整终态子树。extension-header NAL type `14`、`20`、`21` 暂不映射；conformance issue 保留完整 mapping、只使当前 NAL invalid 并继续后续 NAL；取消、source error 和 resource limit 保留已提交 RBSP 前缀。本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 23/23；hosted run `30300358918`、`30301298871`、`30303087560` 均在 Windows、macOS、Ubuntu 成功。下一步定义并实现 lazy boundaries。
- 2026-07-28：完成 M4 checked lazy byte regions：`80fb548` 以 ADR-0026 固化专用 `@lazy(byte_count) bytes name` 语法、checked logical-byte boundary、mapped location、零长度和失败事务语义；`40ff649` 完成 parser/AST、typed IR、bytecode、compiler 与静态校验；`bb25db4` 在 VM 执行前验证 lazy definition，按 guard 求值 checked `u64` byte count，检查 byte-to-bit overflow、absolute logical byte alignment、enclosing range、mapping 和预算，再创建 `Region` 并只 seek、不读取 payload。正长度节点为 `lazy`，零长度节点直接 `materialized`；跨 source gap 的 location 与截断 available prefix 保留全部 mapped spans，logical byte-aligned region 允许映射到非对齐 source span。针对性 executor 套件为 85/85；本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 23/23，hosted run `30308878377` 在 Windows、macOS、Ubuntu 成功。下一步定义并实现 recoverable progressive index。
- 2026-07-28：完成 M4 recoverable progressive index：`589a9f5` 以 ADR-0027 固化同一
  analyzer 内恢复、事务式拒绝、append-only node 和非持久 checkpoint 边界；`4ea0964`
  新增只恢复 cancelled node、清理其直接 cancellation diagnostics 的 core 操作；`03f73db`
  允许 scanner 原状态替换 token，并让 Annex B analyzer 保留 cursor、queue、deferred
  result、partial descendants 和单调 identifiers 后继续。回归覆盖 scanner pending/trailing
  state、空 token、requested token 拒绝、连续 mapper 取消、非 cancellation terminal 拒绝及
  已发布 node 不重放。本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 23/23；
  hosted runs `30311449960` 与 `30312007044` 均在 Windows、macOS、Ubuntu 成功。下一步
  定义并实现 position-aware context directories。
- 2026-07-28：完成 M4 position-aware context directory：`b7347ff` 以 ADR-0028
  固化 definition kind/scope/value key、完整 source-span 可见边界、乱序发现、精确
  generation dependency、禁止静默 fallback 和 64 层解析上限；`01f1e68` 在 core
  新增 append-only `ContextDirectory`，按同 key 的有序 non-overlapping spans 二分
  选择最近已完成定义，并以 stable definition/analysis-node ID 关联未来的
  SPS、PPS、ASC 和 sample-description payload。回归覆盖 span 内/排他结束位置、
  四类 key 与 track scope、乱序 snapshot、transactional rejection、stale/future
  dependency、重定义失效、交叉 cycle 和深度边界。
  本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 24/24；hosted run
  `30366546716` 在 Windows、macOS、Ubuntu 成功。下一步以 sparse/virtual 100 GB
  source 验证初始打开、known-offset read、batch publication、取消与恢复，再接
  SQLite WAL 分页缓存。
- 2026-07-28：完成 M4 sparse/virtual 100 GB source 自动化验证：`481edfd` 覆盖
  session 只读取一个 64 KiB 初始页即可打开 100 GB virtual source、在已知 80 GiB
  offset 读取 12-bit 字段时只请求所需的两个 byte、SourcePager 单页读取，以及 sparse
  source 上有界 H.264 batch 发布、取消、原地恢复和 append-only node 保留；并明确
  这些回归不替代阶段 7 的两秒、RSS 与 p95 实测门禁。英文产品需求与中文伴随说明已
  同步。本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 24/24；hosted
  run `30368687174` 在 Windows、macOS、Ubuntu 成功。下一步定义并实现 SQLite WAL
  分页缓存、批次提交、schema 版本和崩溃恢复。
- 2026-07-28：完成 M4 SQLite WAL paged cache store：`07f6961` 以 ADR-0029 固化
  caller namespace、opaque progressive-index/materialized-result page、64 KiB page、
  256-page atomic batch、thread affinity、exclusive database ownership，以及 M5 前禁止
  path-only 跨 session 复用；`c1d7fe2` 在 core 新增不暴露 Qt SQL 的 `PagedCache`，
  使用 QSQLITE/WAL、固定 application ID 与 schema version 1、`quick_check`、精确
  schema 校验、pending marker 和 transaction rollback。回归覆盖 runtime driver、
  WAL/PRAGMA、binary payload、replacement、namespace isolation、完整 64 KiB page、
  batch/coordinate 上限、forced-write atomic rollback、abandoned-marker recovery、
  incompatible/corrupt schema、live lock、thread violation 和 connection cleanup。cache
  不复制媒体 source byte，也不宣称恢复 live analyzer；后台 owner 与 versioned payload
  在 M5 identity 固定后接入。英文 ADR/core model 与中文伴随说明同步。本机 `dev`、
  `ci`、`sanitize` 重新配置、完整构建与 CTest 均为 25/25；hosted run `30372439547`
  在 Windows、macOS、Ubuntu 成功。下一步固定 M5 TOML manifest、content hash、
  compatibility range 与 rule catalog identity。
- 2026-07-29：M5 rule package、Windows extended directory path、durable source
  fingerprint 与 cache identity/envelope 已依次完成；`510b78c`、`05f38d8`、`ea1717e`
  对应 hosted runs `30423302221`、`30425652345`、`30426917683` 均在 Windows、macOS、
  Ubuntu 成功。
- 2026-07-29：完成 `.svsession` version 1 exact-pinning 切片：闭合且有界的 UTF-8 JSON
  保存 source path/identity、完整 fingerprint、package ID/version/content hash/entry point、
  bookmark、annotation、expanded path 与 view state；64-bit value 使用 canonical decimal
  string，duplicate/unknown/missing/mistyped/out-of-source value 显式拒绝。保存使用关闭
  direct-write fallback 的 `QSaveFile`；恢复先比较同一打开 handle 的 fingerprint，再做精确
  catalog lookup，missing/conflicting/incompatible rule 均不 fallback，最后才绑定 user state。
  bundled H.264 analyzer 也保留 catalog-resolved identity；cache page 与 live analyzer state
  仍不进入 session。英文规范、中文伴随说明和 ADR-0033 已同步；本机 `dev`、`ci`、
  `sanitize` 重新配置、完整构建与 CTest 均为 29/29。实现提交为 `45e6252`；hosted run
  `30430443355` 在 Windows、macOS、Ubuntu 成功。下一步实现两类 owner payload body
  serializer。
- 2026-07-29：完成 M5 owner payload body serializer 切片：progressive-index version 1
  保存 page key、global record index、indexed-through offset、end-of-source 与经过 checked
  byte/span/order 验证的 H.264 start-code record；materialized-result version 1 保存完整 stable
  node/parent identity、闭合 `QVariant` value、multi-span location、metadata、specification 与
  diagnostic，拒绝瞬态 indexing/waiting state。两种 big-endian body 都重复并验证完整 page
  key、受 envelope 的 65,440-byte body 上限约束，并通过 envelope 与 SQLite `PagedCache`
  round-trip；固定 binary vector、错误 key/version/magic/reserved/UTF-8/topology、截断、
  trailing byte 与 size bound 均有回归。该表示不包含 scanner pending prefix、mapper、queue、
  context、allocator 或 thread ownership，不能恢复 live analyzer。英文规范、中文伴随说明和
  ADR-0034 已同步；本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 30/30。
  实现提交为 `3c54716`；hosted run `30452836151` 在 Windows、macOS、Ubuntu 成功。
  下一步接入 background cache owner，仍不宣称 live analyzer recovery。
- 2026-07-29：完成 M5 typed background cache owner runtime 切片：`AnalysisCacheOwner` 在
  dedicated worker thread 打开、读写并销毁唯一 `PagedCache`；write submission 在入队前执行
  body/envelope/full-key preflight，queue 同时限制 outstanding request 与 retained encoded byte，
  exact-key read 区分 missing、corrupt、invalid 与 storage failure。`flush` 等待此前 accepted work；
  draining shutdown 停止 admission、完成已接收请求、在 owner thread 销毁 cache 并 join。
  回归覆盖 caller-thread typed round-trip、count/byte queue pressure、future/flush ordering、atomic
  storage failure 后继续、错误 full-key copied page、open failure 与 lock release。ADR-0035、英文
  core/payload 规范和中文伴随文档已同步；version 1 materialized page 没有 complete-page manifest，
  因而仍不宣称 cached presentation snapshot 或 live analyzer recovery。本机 `dev`、`ci`、
  `sanitize` 重新配置、完整构建与 CTest 均为 31/31。实现提交为 `60b76f8`；hosted run
  `30456734552` 在 Windows、macOS、Ubuntu 成功。下一步把 stable write path 接入
  `AnalysisSession`。
- 2026-07-29：完成 M5 stable session cache write 切片：production local-file session 从同一
  `FileSource` handle 的 fingerprint 与 exact rule identity 派生 namespace；H.264 analyzer 每个
  scanner batch 暴露至多一个 stable progressive update，frontier 只推进到 completed record 末端，
  scan complete 时才推进到 source size。session 在 stream 0 提交 progressive page，并按 stable
  node ID 把 terminal tree 确定性分页成一个最多 256 page 的 atomic materialized batch。cache 默认
  关闭，仅 production executable 使用 `QStandardPaths::CacheLocation` 显式启用；virtual source、
  fingerprint/open/preflight/queue/storage failure 都不影响有效 session/tree。accepted future 只被
  nonblocking poll，terminal 后由 event loop 继续收割；换源时先释放旧 path owner，再启用尚未
  analysis 的 candidate。version 1 仍只写不读，不发现 complete page set、不发布 cached snapshot、
  不恢复 live analyzer，cache page 也不进入 `.svsession`。ADR-0036、英文 core/payload/session 规范
  与中文伴随文档已同步；回归覆盖 stable frontier/index、deterministic export 与字段保真、256/257
  page 边界、local/restore namespace、queue/open/accepted storage failure、换源 owner release 与
  析构 drain。本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 31/31。实现提交为
  `35b2739`；hosted run `30463839242` 在 Windows 2022 / Qt 6.10.1、macOS 15 / Qt 6.11.1
  与 Ubuntu 24.04 / Qt 6.11.1 的 Configure、Build、Test、Install、Upload 均成功。下一步审计
  全部稳定 DSL 功能的英文规范、中文说明与正反例，然后进入 M6 H.264 正式格式增量。
- 2026-07-29：完成稳定 DSL 文档与示例审计：英文 format-language 规范和中文伴随说明已经覆盖
  enum/endian、Exp-Golomb、固定数组、等值条件与 switch、有界 repeat、pure/computed、lazy
  boundary 和 progressive-index recovery 的 grammar、静态规则、lowering/runtime、source
  coordinate、诊断恢复、资源/取消行为与正反例。修正条件摘要中已经落后的 fixed-width-only
  表述，并在 ADR-0020、ADR-0022 的英中版本中记录 computed 与 lazy 后续切片的历史扩展，
  避免改写首个切片的原始决策。非法示例明确加入多个 `entry` 声明，parser 回归锁定第二个
  `entry` 的 `DuplicateName` 诊断。重新编译后的定向 DSL 测试通过；本机 `dev`、`ci`、
  `sanitize` 重新配置、完整构建与 CTest 均为 31/31。实现提交为 `4b54808`；hosted run
  `30467004306` 在 Windows 2022 / Qt 6.10.1、macOS 15 / Qt 6.11.1 与 Ubuntu 24.04 /
  Qt 6.11.1 的 Configure、Build、Test、Install、Upload 均成功。下一步界定并实现首个 M6
  H.264 正式结构切片。
- 2026-08-01：完成首个 M6 H.264 正式结构切片，并为它新增 DSL payload 派发。`65044a2`
  以 ADR-0037 固化决策：`nal_unit_type` 到结构的映射必须由规则声明，不能写进分析核心；
  唯一接受的 view kind 是 `rbsp`；一个程序至多一个顶层 `payload<rbsp> sequence
  switch (controller) { case integer: Structure | empty; }`；没有 `default` arm，未列出的
  type 保持既有未解释 payload 行为。`b59f250` 实现语言与运行时：`payload` 和 `empty` 仍是
  上下文标识符，parser 拒绝不支持的 view kind、第二个派发、未知 sequence/controller/target、
  非顶层无条件无符号 scalar `bits` 的 controller、重复或超出宽度的 case 值、没有 case 的
  派发、等于 element structure 的 case 目标、`default` arm，以及缺少对应 entry 的 sequence；
  typed program 只保存已解析 case，不新增 opcode，选中的结构复用既有 `begin-structure` 到
  `end-structure` bytecode。Annex B runner 从已发布 header 读取 controller，只要命中 case 就
  一定派生 RBSP view，零长度 payload 也不例外，因此决定 view 是否存在的是 case 而非 payload
  长度；结构 case 在 `rbsp_payload` 下解码并且必须精确消费完整 RBSP，剩余 bit 为
  `invalid-syntax`，`empty` case 要求零长度 RBSP，未派发 type 行为完全不变。内置规则新增
  `AccessUnitDelimiterRbsp`（逐 bit 的 `rbsp_alignment_zero_bit[0..3]`）并派发 type 9、10、11。
  因此一字节 AUD 可完整解码、只有 header 的 AUD 是 `truncated-source`、只有 header 的 end of
  sequence/stream 物化，而这两种 type 携带 RBSP 字节即 `invalid-syntax`，并且失败 payload 不
  终止 scan。回归覆盖 parser 静态规则与恢复、typed IR 降低、AUD 逐 bit source span、截断、
  非法 stop bit、未声明尾随 bit、空 RBSP 正反例和未派发 type 的向后兼容。英文规范、中文
  伴随说明和 ADR-0037 双语版本已同步；本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与
  CTest 均为 31/31。hosted run `30699364480` 在 Windows 2022 / Qt 6.10.1、macOS 15 /
  Qt 6.11.1 与 Ubuntu 24.04 / Qt 6.11.1 的 Configure、Build、Test、Install、Upload 均成功。
  下一步界定并实现 sequence parameter set 切片。
- 2026-08-02：完成有界 `rbsp_trailing_bits;` DSL 终结项，为变长 H.264 结构的精确
  RBSP 消费提供受限 lowering。ADR-0038（`b9f2963`）决定该项只能作为结构顶层最后一项；
  `bea0a8f` 实现 parser、typed IR、单条 VM instruction、逐 bit source-span 物化、恶意
  typed IR 拒绝、官方 AUD 规则迁移与双语 DSL 参考。回归覆盖静态 placement、八 slot 资源
  预留、stop/padding 约束与截断、跨 excluded source span、失败后继续扫描以及非终端 IR。
  本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 31/31；hosted run
  `30717519949` 对 `bea0a8f9acb969d025353d903932da59653c3170` 在 Windows 2022 /
  Qt 6.10.1、macOS 15 / Qt 6.11.1 与 Ubuntu 24.04 / Qt 6.11.1 的 Configure、Build、
  Test、Install、Upload 均成功。下一步界定并实现 sequence parameter set 切片。
- 2026-08-02：完成有界 H.264 SPS 结构核心。ADR-0039（`1d121d3`）把 type-7
  `SequenceParameterSetRbsp` 限定为 Baseline/Main/Extended 核心及 8-bit 4:2:0 的受限
  High 子集：接受 `pic_order_cnt_type == 0`、无 VUI 的路径，并在 unsupported profile、
  High chroma/depth/transform/scaling 值、picture-order 分支和 VUI 边界保留已解码前缀后报告
  `invalid-syntax`。`30109e0` 新增 `ue @equals` 约束（包括可编码 `ue` 上界、malformed typed
  IR 防护、source-located failure 与 constrained-repeat controller），将官方规则分派 type 7，
  并把 package 更新到 `0.1.2`，清单同步声明 Extended。双语语言参考和 SPS 结构、High 子集、
  不支持分支、profile/reserved-bit 拒绝以及 `ue` runtime/IR 回归均已覆盖。本机 `dev`、`ci`、
  `sanitize` 重新配置、完整构建与 CTest 均为 31/31；hosted run `30719765999` 对
  `30109e0c0fa1c22bb299ecaa41012e445db59c1d` 在 Windows 2022 / Qt 6.10.1、macOS 15 /
  Qt 6.11.1 与 Ubuntu 24.04 / Qt 6.11.1 的 Configure、Build、Test、Install、Upload 均成功。
  下一步界定并实现 H.264 clause 7.4.2.1.1 的 SPS `log2_*_minus4` 语义范围约束。
- 2026-08-02：完成 H.264 clause 7.4.2.1.1 的 SPS `log2_*_minus4` 语义范围约束。
  ADR-0040（`8653fc0`）区分两类约束：`@equals` 与 enum 成员属于 layout-critical，违反意味着
  bit 位置假设已被破坏，必须把结构标记为 Invalid 并停止解码；`0..12` 属于 value-domain，
  Exp-Golomb 码字本身读取正确、后续字段位置精确无误，因此必须继续解码，只在该字段挂一条
  Warning 诊断。`4e5326b` 实现 `@range(min, max)`：parser 静态校验每字段至多一次、只允许
  `ue` 字段、必须是两个整数实参、最小值不得大于最大值、最大值不得超出可编码 `ue` 上界；
  typed IR 增加 `DslTypedUnsignedRange` 与 `AssertRangeMinimum`/`AssertRangeMaximum` 两条
  指令，在读取指令之后按声明顺序发射，数组元素逐个展开；VM 在执行前拒绝出现在非 `ue`
  字段、computed controller、`se`、lazy bytes 与 rbsp trailing bits 上的 range 约束，并拒绝
  operand、immediate 或字段类型与 typed IR 不一致的恶意指令。违规时结构仍然物化、
  `bitsConsumed` 继续推进，诊断为 `invalid-syntax` + Warning，`fieldPath` 为
  `Structure.field`，location 精确覆盖该码字的逐 bit source span；被条件跳过的字段不做检查。
  官方规则给 `log2_max_frame_num_minus4` 与 `log2_max_pic_order_cnt_lsb_minus4` 标注
  `@range(0, 12)` 并附 clause 引用与双语说明，package 更新到 `0.1.3`。回归覆盖 parser 正反例、
  与 `@equals` 共存的降低顺序、数组展开、in-range 物化、越上界与越下界的告警（含 severity、
  fieldPath、bit 长度与后续字段仍然正确解码）、跳过分支不检查、五种 malformed typed IR，
  以及真实 SPS 中 `log2_max_frame_num_minus4 == 13` 的分析器级告警。双语 DSL 参考与 ADR-0040
  双语版本已同步。本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 31/31；
  hosted run `30744876898` 对 `4e5326b546ac46711a825b2763cd80734babf4d8` 在 Windows 2022 /
  Qt 6.10.1、macOS 15 / Qt 6.11.1 与 Ubuntu 24.04 / Qt 6.11.1 的 Configure、Build、Test、
  Install、Upload 均成功。下一步界定并实现有界 H.264 picture parameter set 切片。
- 2026-08-02：完成有界 H.264 picture parameter set base syntax。在开始 PPS 前，`e3d67db`
  与 `f5721dd` 先把上一条 `@range` 切片的 bundled-profile 行为、typed IR constraint 和
  `assert-range-minimum`/`assert-range-maximum` bytecode 清单在英中参考中对齐。ADR-0041
  （`da0f87c`）把 type-8 `PictureParameterSetRbsp` 限定为无需 SPS lookup 即可精确解析的
  clause 7.3.2.2 base 字段：只接受 `num_slice_groups_minus1 == 0` 且没有 PPS extension，
  不声称已经解析 SPS generation、注册 PPS 或可供 slice header 使用。`27bd8d0` 新增 type 8
  payload dispatch、PPS/SPS ID 与默认 reference-index count 的非致命 range、三种
  `weighted_bipred_idc` enum 值、signed QP 字段和 base control flag，以
  `rbsp_trailing_bits;` 精确终结；package 更新到 `0.1.4`，entrypoint coverage depth 更新为
  `parameter-sets`。回归覆盖最小与非默认合法 PPS、逐字段 metadata/source span、PPS ID 256
  warning、非零 slice group 后继续扫描、reserved weighted biprediction 与 PPS extension 拒绝；
  H.264 analyzer 定向测试为 44/44。双语参考包含全部已声明 PPS 字段的含义和延期边界。本机
  `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 31/31；hosted run
  `30752902472` 对 `27bd8d01a151e4a42a672191e1e81248283c44b9` 在 Windows 2022 /
  Qt 6.10.1、macOS 15 / Qt 6.11.1 与 Ubuntu 24.04 / Qt 6.11.1 的 Configure、Build、Test、
  Install、Upload 均成功。下一步界定并实现有界 H.264 VUI core slice。
- 2026-08-03：完成有界 H.264 VUI core slice。ADR-0042（`12c4241`）决定把 SPS 的
  `vui_parameters_present_flag @equals(0)` 边界替换为 Annex E.1.1 的 inline 可选分支：支持
  aspect ratio 与 Extended SAR、overscan、video signal 与 colour description、chroma sample
  location、timing、`pic_struct_present_flag` 和完整 bitstream-restriction syntax；NAL/VCL HRD
  presence flag 暂时要求为零，存在任一 HRD branch 时保留已解码前缀并以 `invalid-syntax`
  停止。`a0317ed` 实现上述结构，把 package 更新到 `0.1.5`，并为两个 chroma location 增加
  非致命 `@range(0, 5)`，为两个 denominator 与两个 motion-vector length 字段增加非致命
  `@range(0, 16)`。回归覆盖 minimal 与完整 VUI、Extended SAR、全部 presence branch、六个
  range warning、两条 HRD fatal 边界、精确 diagnostic field/source span、trailing bits 和失败
  NAL 后继续扫描；H.264 analyzer 定向测试为 48/48。英文规范与中文伴随文档同步记录全部
  已声明 VUI 字段、warning/fatal 语义及延期的 fixed-width、relational、HRD 与 registration
  边界。本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 31/31；hosted run
  `30753887431` 对 `a0317ed6fc097306b36d7825bd9512023cedd48a` 在 Windows 2022 /
  Qt 6.10.1、macOS 15 / Qt 6.11.1 与 Ubuntu 24.04 / Qt 6.11.1 的 Configure、Build、Test、
  Install、Upload 均成功。下一步界定并实现有界 H.264 HRD parameters slice。
- 2026-08-03：完成有界 H.264 HRD parameters slice。ADR-0043（`b620a9c`）决定移除两个
  HRD presence flag 的 `@equals(0)` 边界，并用独立 `nal_hrd_` / `vcl_hrd_` 前缀 inline
  展开 Annex E.1.2：每个分支包含 `cpb_cnt_minus1`、两个 scale、由 computed count 控制且
  最多 32 次的 CPB schedule repeat，以及四个 delay-length 字段；任一分支存在时，通过
  computed Boolean 读取共同的 `low_delay_hrd_flag`。`e093de8` 实现上述规则，把 package
  更新到 `0.1.6`。两个 `cpb_cnt_minus1` 带非致命 `@range(0, 31)`；越界 count 保留字段
  warning、scale 与派生 count，再由 layout-critical repeat bound 在 schedule entry 前停止，
  因此既报告 value-domain 问题，也不允许 unsupported count 改变声明布局。回归覆盖 NAL-only、
  VCL-only、两者同时存在、重复 schedule index、computed field、精确 E.2.2 source span、
  `low_delay_hrd_flag`、trailing bits，以及 count 32 warning/fatal 后继续扫描；H.264 analyzer
  定向测试为 49/49。英中参考同步记录全部 HRD syntax/computed 字段与延期的 SEI consumer、
  level-dependent bitrate/CPB/delay relation 和 context registration。本机 `dev`、`ci`、
  `sanitize` 重新配置、完整构建与 CTest 均为 31/31；hosted run `30754634886` 对
  `e093de848baf6ab3515c6f4704c524a25376b2b8` 在 Windows 2022 / Qt 6.10.1、macOS 15 /
  Qt 6.11.1 与 Ubuntu 24.04 / Qt 6.11.1 的 Configure、Build、Test、Install、Upload 均成功。
  下一步界定并实现有界 H.264 slice-header 与 parameter-set dependency slice。
- 2026-08-04：完成有界 H.264 slice-header 的 rule-declared context generation 与
  parameter-set dependency 前置切片。ADR-0044（`24623ff`）固化 `@context`、最多 16 个
  可重复 `@context_dependency` 与最多 64 个 field-level `@context_export`，只接受同一结构中
  无条件、顶层、非数组的 unsigned `bits`、enum、`ue` 或 `computed<u64>`；重复 dependency、
  未知 kind、guarded/repeated/array/signed key 或 export 均为静态错误。`b035b9f` 新增
  move-only `RuleExecutionSession`，把一个 compiled program、analysis source/tree identity、
  position-aware directory 与 rules-owned typed payload 关联起来；VM 在 source read 前验证
  context typed IR，只返回声明的 key/dependency/export value 及精确 location，不暴露完整
  local environment，也不从 presentation tree 回读。session 支持非零 logical-start suffix，
  要求 context mapping 完全位于 non-empty enclosing source span 内，并且只在 materialization、
  requested exact consumption、dependency resolution 与 payload preparation 全部成功后发布。
  官方 H.264 package 更新到 `0.1.7`：SPS 发布 slice-header 所需字段，PPS 在自己的 NAL 之前
  解析并绑定精确 SPS generation；missing/future/stale dependency 不回退，缺少 SPS 的 PPS
  structure 保持 materialized，但 RBSP/NAL invalid、不发布 generation，scanner 继续后续 NAL。
  `AnalysisTree` 增加跨 move 保持、copy 时更新且不返回零的 runtime instance identity，防止
  session 跨分析复用。英中 DSL 规范、ADR 与正反例已同步；新增独立 session 测试 executable，
  本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 32/32；hosted run
  `30837740934` 对 `b035b9fd8c60a3b7baa1ffe81a45e929f4d39f0b` 在 Windows 2022 /
  Qt 6.10.1、macOS 15 / Qt 6.11.1 与 Ubuntu 24.04 / Qt 6.11.1 的 Configure、Build、
  Test、Install、Upload 均成功。完整 slice-header 项仍未完成；下一步定义 rule-declared
  context import，再继续 dynamic `bits<expression>`、bounded sentinel loop 与 compressed
  remaining-bit payload。
- 2026-08-04：完成有界 H.264 slice-header 的 rule-declared context import 前置切片。
  ADR-0045（`2118632`）固化可重复 structure annotation `@context_import(kind, key)`：一个
  structure 最多 16 个 import，只接受同结构中无条件、顶层、非数组的 unsigned `bits`、enum、
  `ue` 或 `computed<u64>` key；重复 kind/key pair 静态拒绝。`8b00e1c` 在 typed field 中保留
  context-key eligibility，使 VM 在 source read 前拒绝数组元素、非法 kind、重复、越界索引与
  超量恶意 IR；成功执行只返回 import descriptor、key value 与精确 location，不暴露完整 local
  environment。`RuleExecutionSession` 在 consumer enclosing span 起点选择 generation，
  missing/future/stale 都不回退；成功结果按 root-first、dependency declaration-order DFS 返回
  rules-owned exact payload closure，按 definition ID 去重并限制 64 项。import 与同结构 publication
  保持事务性：后续 import 失败不泄漏先前 closure，也不发布 generation。英中 DSL 规范与正反例
  已同步；回归覆盖 exact payload/dependency identity、位置重定义、future/stale/missing、64/65
  closure 边界和 malformed typed IR。本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest
  均为 32/32；hosted run `30842615170` 对 `8b00e1ccc8fd49ced57da2605e340e6af765a841`
  在 Windows 2022 / Qt 6.10.1、macOS 15 / Qt 6.11.1 与 Ubuntu 24.04 / Qt 6.11.1 的
  Configure、Build、Test、Install、Upload 均成功。完整 slice-header 项仍未完成；下一步定义
  exact imported context value 的 expression namespace 与 dynamic `bits<expression>`，之后再做
  bounded sentinel loop 和 compressed remaining-bit payload。
- 2026-08-04：完成 imported dynamic `bits<expression>` width evaluation。ADR-0046
  固化 `context_value(import_key, context_kind, exported_field)` 这一受限 expression leaf：
  compiler 只接受 import root/精确 dependency closure 中恰好一个 publisher 的 exported
  unsigned scalar，并把 imported leaf 纳入 node/depth 预算；dynamic field 仅允许 big-endian
  scalar bits，runtime width 严格为 1..64，checked arithmetic、截断事务和 mapped source
  spans 沿用既有 VM 语义。`RuleExecutionSession` 为每次 run 建立 per-run exact-generation
  closure cache，VM 与最终 imported result 复用同一 root-first dependency-order closure，
  missing/future/stale generation 不回退；被引用 publisher 的 context descriptor 也在 source
  read 前完整预验证。英中 DSL 规范、ADR-0046 伴随说明与 malformed IR/session 回归已同步。
  实现提交为 `4ff23ad`；本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 32/32；
  hosted run `30850983789` 在 Windows 2022 / Qt 6.10.1、macOS 15 / Qt 6.11.1 与 Ubuntu
  24.04 / Qt 6.11.1 的 Configure、Build、Test、Install、Upload 均成功。完整 slice-header 项
  仍未完成；下一步定义 bounded sentinel loop，之后再实现 compressed remaining-bit payload。
- 2026-08-04：完成 bounded post-tested sentinel repeat。ADR-0047 与英中 DSL reference 固化
  `repeat (maximum) { ... } until (field == integer);`：maximum 限制为 1..64，sentinel 只能是
  body 直接声明的无条件、顶层、非数组 fixed `bits`、enum 或 `ue` source scalar；body 至少
  执行一次并保留终止项。compiler 以 guarded field projection 静态展开全部 iteration，typed IR
  记录每轮起点/sentinel、enclosing guards 与 assertion position，并在共享边界稳定排序 nested
  assertion；VM 在 source read 前验证 descriptor、完整 guard prefix、`ue` domain 和 64 项上限，
  命中后后续 projection 只计 instruction/cancellation 而不读源或建 node，未命中则在最终
  sentinel 上返回 `invalid-syntax` 并保留有界 prefix。parser/compiler/VM 回归覆盖第一/中间/
  最后一轮终止、未终止、截断事务、外层 false guard、nested assertion 顺序、instruction/node/
  cancellation 预算和 malformed typed IR；equality condition/switch 同步接受已解码 scalar `ue`。
  实现提交为 `021eeb9`，文档提交为 `644651e`；本机 `dev`、`ci`、`sanitize` 重新配置、完整
  构建与 CTest 均为 32/32；hosted run `30857732349` 在 Windows 2022 / Qt 6.10.1、macOS 15 /
  Qt 6.11.1 与 Ubuntu 24.04 / Qt 6.11.1 的 Configure、Build、Test、Install、Upload 均成功。
  bundled H.264 rule 尚未使用新语法，package version 保持不变；完整 slice-header 项仍未完成，
  下一步定义 compressed remaining-bit payload terminal。
- 2026-08-04：完成 compressed remaining-bit payload terminal。ADR-0048 与英中 DSL reference
  固化命名终结项 `compressed_payload name;`：它只能无条件位于结构顶层最终位置，与
  `rbsp_trailing_bits` 互斥，不接受数组、前置 annotation、表达式、constraint、enum 或
  context metadata；typed IR 降为一个 `CompressedPayload` field 与一条
  `register-compressed-payload` instruction。VM 在读取 source 前拒绝 malformed field/opcode，
  成功执行把当前 bounded reader 的全部剩余 bit 映射为一个 `Materialized`
  `CompressedPayload` node，保留非 byte 对齐与 multi-span location，允许零长度，并在不读取
  payload source 的情况下 seek 到逻辑末尾。实现提交为 `9c832ba`，文档提交为 `878aa5a`；本机
  `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为 32/32；hosted run `30861138810`
  在 Windows 2022 / Qt 6.10.1、macOS 15 / Qt 6.11.1 与 Ubuntu 24.04 / Qt 6.11.1 的
  Configure、Build、Test、Install、Upload 均成功。官方 H.264 package 尚未接入该 terminal，
  package version 保持 `0.1.7`；下一步定义首个有界 H.264 slice-header structure 并接入
  VCL NAL dispatch。
- 2026-08-04：完成首个有界 H.264 progressive IDR all-I slice-header 增量。ADR-0049 与英中
  bundled-profile 文档提交 `7d44b2a` 固化 type `5`、`slice_type == 2`、progressive frame、
  POC type 0、无 bottom-field POC/redundant-picture/deblocking-control 的窄支持边界，并明确
  `compressed_payload slice_data` 表示包含 slice trailing bits 的完整 opaque RBSP suffix。实现
  提交 `3330cc7` 新增 `IdrSliceLayerWithoutPartitioningRbsp`，通过精确 PPS import 及其绑定
  SPS dependency 解码 `first_mb_in_slice`、`slice_type`、`pic_parameter_set_id`、dynamic-width
  `frame_num`、`idr_pic_id`、dynamic-width `pic_order_cnt_lsb`、IDR marking flags 与
  `slice_qp_delta`，再把剩余 bit 发布为非 byte 对齐的 materialized compressed payload。
  layout prerequisite 在受影响字段读取前通过 checked dynamic-width expression 失败；
  missing/future/stale parameter set 仍为 source-located `dependency-unavailable`，当前 NAL
  失败后继续后续 NAL。官方 package 更新为 `0.1.8`、coverage depth 更新为
  `idr-slice-header`；analyzer 正反例覆盖精确 context chain、4-bit imported width、7-bit
  opaque span、missing PPS 和 deblocking prerequisite，GUI/CLI/mapper fixtures 同步改用未派发
  type `1`。H.264 analyzer 为 53/53；本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与
  CTest 均为 32/32；hosted run `30863800309` 对
  `3330cc7073d92a663b568eac9b71d3e527e058c4` 在 Windows 2022 / Qt 6.10.1、macOS 15 /
  Qt 6.11.1 与 Ubuntu 24.04 / Qt 6.11.1 的 Configure、Build、Test、Install、Upload 均成功。
  完整 Baseline/Main/High slice-header 项仍未完成；下一步接受等价 `slice_type == 7`，随后
  声明 imported-context conditional branch，以实现 bottom-field POC、redundant-picture 与
  deblocking-control syntax，再扩展 non-IDR/P/B reference-list 和 weighted-prediction 分支。
- 2026-08-04：完成等价 progressive IDR all-I `slice_type == 7` 增量。ADR-0051 与英中文规则
  文档记录 `IdrAllISliceType { i = 2; all_i = 7; }`，官方 H.264 package 更新为 `0.1.9`；
  `slice_type` 改为通用 `ue @enum`，其余 header layout 与 `slice_data` opaque boundary 保持不变。
  analyzer 新增合法 type-7 fixture（opaque suffix 从 absolute bit 197 起、长度 11）和非法
  type-3 fixture（`slice_type` diagnostic 位于 absolute bit 33、长度 5，后续 AUD 继续 materialize）。
  文档提交为 `c111429`，实现提交为 `cab1dfa`；H.264 analyzer 定向测试为 55/55，本机 `dev`、
  `ci`、`sanitize` 重新配置、完整构建与 CTest 均为 32/32；hosted run `30866500351` 在
  Windows 2022 / Qt 6.10.1、macOS 15 / Qt 6.11.1 与 Ubuntu 24.04 / Qt 6.11.1 的
  Configure、Build、Test、Install、Upload 均成功。下一步声明 imported-context conditional
  branches，先处理 bottom-field POC、redundant-picture 与 deblocking-control syntax。
- 2026-08-06：完成由 PPS 控制的剩余有界 progressive IDR all-I slice-header 分支。
  ADR-0053 与英中文 bundled-profile 文档提交 `efc55bc` 决定以 ADR-0052 的 exact imported
  equality guard 取代三个 dynamic-width division 拒绝占位：按 clause 7.3.3 顺序声明
  `delta_pic_order_cnt_bottom`、带非致命 `@range(0, 127)` 的 `redundant_pic_cnt`，以及
  deblocking-filter control。实现提交 `892bef6` 新增
  `DisableDeblockingFilterIdc { enabled = 0; disabled = 1; enabled_within_slice = 2; }`；值 1
  省略两个 offset，值 0/2 解码 signed alpha/beta offset，reserved 值在完整 controller
  码字处致命失败。false imported guard 不读 source、不建节点，`slice_data` 继续从最后一个
  selected field 后精确覆盖剩余 RBSP bit。官方 H.264 package 更新为 `0.1.10`，coverage
  depth 保持 `idr-slice-header`。analyzer 回归覆盖三个 PPS presence flag、全部合法 deblocking
  mode、reserved mode 后继续扫描、`redundant_pic_cnt == 128` warning、字段缺席以及逐 bit
  source/payload boundary，定向套件为 59/59；`svtool rule check` 通过。本机 `dev`、`ci`、
  `sanitize` 重新配置、完整构建与 CTest 均为 32/32；hosted run `31113845900` 对
  `892bef62f186328ca9d77c654c6491b3545791b3` 在 Windows 2022 / Qt 6.10.1、macOS 15 /
  Qt 6.11.1 与 Ubuntu 24.04 / Qt 6.11.1 的 Configure、Build、Test、Install、Upload 均成功，
  三份 package artifact 均已生成。完整 Baseline/Main/High slice-header 项仍未完成。独立审计
  还确认现有 type-5 dispatch 只按 `nal_unit_type` 选择 structure，尚不能由 rule 在无额外
  presentation field 的情况下强制 `nal_ref_idc != 0`；下一步先定义这一 rule-owned conformance
  prerequisite，再扩展 non-IDR/P/B reference-list 与 weighted-prediction 分支。
- 2026-08-07：完成 source-anchored DSL assertion statement 与 H.264 type-5 reference-priority
  prerequisite。ADR-0054 及英中 DSL 参考定义 `assert(boolean_expression) at source_field;`：
  assertion 只能是无 annotation 的 unconditional top-level item，condition 只引用此前可用的
  unsigned/computed scalar，anchor 为此前 source-backed scalar，单 structure 上限为 1,024。
  typed IR 保存 `DslTypedAssertion` 与 statement position，compiler 按 sentinel → expression →
  repeat 顺序发射 `AssertExpression`；VM 在 source read 前严格验证 descriptor/opcode pairing，
  assertion 不读取 source、不移动 cursor、不创建 node，false 返回 fatal `invalid-syntax` 并以
  anchor 的完整 mapped range 定位。官方规则加入
  `assert(nal_unit_type != 5 || nal_ref_idc != 0) at nal_ref_idc;`，package 更新到 `0.1.11`；
  type-5 `nal_ref_idc == 0` 在 payload mapping 前失败，后续合法 NAL 继续 materialize。
  实现提交为 `e394604`，H.264 规则提交为 `236b130`，双语文档与 ADR 提交为 `9d330e9`。
  DSL executor 定向测试为 116/116，H.264 analyzer 定向测试为 60/60；本机 `dev`、`ci`、
  `sanitize` 完整配置、构建与 CTest 均为 32/32，`svtool rule check` 通过。Hosted runs
  `31177484935` 与 `31177900565` 在 Windows 2022、macOS 15、Ubuntu 24.04 的 Build、Test、
  Install、Upload 均成功。完整 Baseline/Main/High slice-header 项仍未完成；下一步定义并实现
  progressive non-IDR all-I branch，再隔离首个 P-slice reference-list prerequisite。
- 2026-08-07：完成有界 progressive non-IDR all-I slice-header 增量。新增 ADR-0055
  及英中 format-language 参考，加入 `nal_unit_type == 1` 的 source-anchored
  `nal_ref_idc == 0` prerequisite，并将 type 1 派发到
  `NonIdrAllISliceLayerWithoutPartitioningRbsp`。该结构支持 `slice_type` 2/7、progressive
  frame、POC type 0、精确 PPS/SPS generation、PPS-controlled bottom-field POC、redundant-picture
  与 deblocking 分支，以及 opaque `slice_data`；IDR-only fields 和
  `dec_ref_pic_marking` 保持排除。package 更新为 `0.1.12`、coverage depth 为
  `all-i-slice-header`；旧 opaque fixtures 迁移到 type 12，回归覆盖合法字段/跨度、非零
  reference priority 的 header-boundary failure 与后续 NAL 继续扫描。实现与测试提交为
  `170d516`，双语文档提交为 `16ae093`。本机 `cmake --preset dev/ci/sanitize`、三套
  build 与 `ctest` 均为 32/32，H.264 analyzer 为 62/62，`svtool rule check` 通过；
  hosted run `31181671443` 的 Windows 2022、macOS 15、Ubuntu 24.04 Configure、Build、Test、
  Install、Upload 全部成功。下一步定义并实现首个 bounded progressive non-IDR P-slice
  reference-list prerequisite。
- 2026-08-07：完成 source-anchored assertion 引用 exact imported context value 的前置切片。
  ADR-0056 与英中 format-language 参考提交 `613a53a` 把
  `context_value(import_key, context_kind, exported_field)` 作为 `u64` leaf 开放给 assertion
  condition，同时保持 pure-function body、computed field、lazy byte count 与其他一般 expression
  position 的既有拒绝。实现提交 `5c98d84` 只在 parser assertion validation、compiler assertion
  lowering 与 VM assertion typed-IR preflight 三处启用已有 imported-context 合同；descriptor、
  bytecode 与 session resolver 均未新增类型。Boolean short-circuit 不解析未选 operand；condition
  为 false 时 diagnostic 仍锚定 `at` 字段，missing/future/stale generation 则保留
  `dependency-unavailable` 并定位 import key。parser、IR、executor 与 execution-session 定向套件
  全部通过；本机 `cmake --preset dev/ci/sanitize`、三套完整 build 与 CTest 均为 32/32，
  `svtool rule check` 通过。hosted run `31186610199` 的 Windows 2022、macOS 15、Ubuntu 24.04
  Configure、Build、Test、Install、Upload 全部成功。下一步使用该能力界定并实现首个 bounded
  progressive non-IDR P-slice reference-list prerequisite。
- 2026-08-07：完成首个有界 progressive non-IDR P-slice header 增量。ADR-0057 与英中文
  bundled-profile 参考提交 `362ac2a` 固化 type-1 non-reference P 值 0/5、两个 mandatory
  reference-list control bit、weighted-prediction/CABAC PPS prerequisite，以及通用
  `NonIdrSliceType` / `NonIdrSliceLayerWithoutPartitioningRbsp` presentation 名。实现提交
  `8a37db1` 新增可见 `is_p_slice` computed node；P 路径实际读取并要求
  `num_ref_idx_active_override_flag == 0` 与 `ref_pic_list_modification_flag_l0 == 0`，再以
  imported source-anchored assertion 要求精确 PPS 的 `weighted_pred_flag == 0` 和
  `entropy_coding_mode_flag == 0`。all-I 值 2/7 会短路 P-only prerequisite；type-1 direct
  header 继续要求 `nal_ref_idc == 0`，因此没有 `dec_ref_pic_marking`。package 更新为
  `0.1.13`、coverage depth 为 `i-p-slice-header`；回归覆盖 P type 0/5、all-I child/order、
  两个非零 control flag、weighted/CABAC assertion 的精确 source span，以及失败后继续扫描，
  H.264 analyzer 定向套件为 68/68。`svtool rule check` 通过；本机 `dev`、`ci`、`sanitize`
  重新配置、完整构建与 CTest 均为 32/32。hosted run `31189395918` 的 Windows 2022、macOS 15、
  Ubuntu 24.04 Configure、Build、Test、Install、Upload 全部成功。完整 Baseline/Main/High
  slice-header 项仍未完成；下一步定义并实现 bounded P-slice reference-index override 分支。
- 2026-08-07：完成有界 P-slice reference-index override 增量。ADR-0058 与英中文
  bundled-profile 参考提交 `7ded7ba` 固化 optional list-0 override：
  `num_ref_idx_active_override_flag == 1` 时按 clause 顺序读取带非致命
  `@range(0, 31)` 的 `num_ref_idx_l0_active_minus1`；flag 为零时该字段缺席，后续
  `ref_pic_list_modification_flag_l0`、QP 与 opaque `slice_data` 边界保持不变。实现提交
  `9a9e81d` 把官方 package 更新为 `0.1.14`，coverage depth 保持
  `i-p-slice-header`；回归覆盖 P type 0/5 的非默认 override、zero-flag 字段缺席、
  count 32 warning 后 payload 不错位、截断 count、override 后仍不支持的 list
  modification，以及精确 source span 与后续 NAL 恢复。H.264 analyzer 定向套件为
  72/72，`svtool rule check` 通过；本机 `dev`、`ci`、`sanitize` 重新配置、完整构建
  与 CTest 均为 32/32。hosted run `31192254742` 的 Windows 2022、macOS 15、Ubuntu 24.04
  Configure、Build、Test、Install、Upload 全部成功。完整 Baseline/Main/High
  slice-header 项仍未完成；下一步定义并实现 bounded P-slice reference-list
  modification loop。
- 2026-08-07：完成有界 P-slice reference-list modification loop 增量。ADR-0059 与英中文
  bundled-profile 参考提交 `b635cd7` 固化 list 0 的 post-tested bounded syntax：flag 为零时
  modification operation 缺席；flag 为一时最多执行 64 次，按 `modification_of_pic_nums_idc`
  值 0/1 读取 `abs_diff_pic_num_minus1`、值 2 读取 `long_term_pic_num`，值 3 作为保留在树中的
  terminator。闭集 enum 使 reserved idc 在完整 Exp-Golomb 码字处致命失败；64 是 bundled
  profile 的资源边界，不宣称为 H.264 conformance limit。实现提交 `d16e932` 把官方 package
  更新为 `0.1.15`，coverage depth 保持 `i-p-slice-header`；weighted-prediction 与 CABAC
  prerequisite 仍位于 loop 之后、`slice_qp_delta` 之前，`slice_data` 继续作为 opaque payload。
  回归覆盖零 flag、首项终止、idc 0/1/2/3 全路径、type-5 P alias、reserved idc、截断 operation、
  截断 operand、64 次未终止以及精确 source span 与后续 NAL 恢复，H.264 analyzer 定向套件为
  77/77。`svtool rule check` 通过；本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest
  均为 32/32。hosted run `31195600795` 对 `d16e9320ca331e7b5b6549128563d35492f60632`
  的 Windows 2022、macOS 15、Ubuntu 24.04 Configure、Build、Test、Install、Upload 全部成功。
  完整 Baseline/Main/High slice-header 项仍未完成；下一步定义并实现 bounded non-reference
  P-slice CABAC initialization branch。
- 2026-08-07：完成有界 non-reference P-slice CABAC initialization branch 增量。ADR-0060
  与英中文 bundled-profile 参考提交 `c39677c` 固化 clause 7.3.3 presence 条件：保留
  weighted-prediction prerequisite，只在 coded P 值 0/5 且所选 PPS 的
  `entropy_coding_mode_flag == 1` 时，于 `slice_qp_delta` 之前读取 `cabac_init_idc`；
  entropy-enabled all-I 路径不消费该字段。实现提交 `75c04ae` 以 nested local/imported guard
  取代旧 entropy assertion，并用 `ue @range(0, 2)` 报告非 layout-critical value-domain
  warning；值 3 保留完整字段并继续 QP、deblocking 与 opaque payload，截断码字仍致命失败。
  type-1 direct header 继续要求 `nal_ref_idc == 0`，因此没有 reference-picture marking。
  package 更新为 `0.1.16`，coverage depth 保持 `i-p-slice-header`。回归覆盖 entropy-disabled
  P 与 entropy-enabled all-I 字段缺席、coded P 值 0/5、合法 idc 0/1/2、值 3 warning 后的
  deblocking/QP/payload 精确 boundary、截断码字及后续 NAL 恢复；H.264 analyzer 定向套件为
  79/79。`svtool rule check` 通过；本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest
  均为 32/32。hosted run `31198302934` 对 `75c04aedb7ca4b497580eaa6dad4febf9c8b1e1b`
  的 Windows 2022、macOS 15、Ubuntu 24.04 Configure、Build、Test、Install、Upload 全部成功。
  完整 Baseline/Main/High slice-header 项仍未完成；下一步定义并实现 bounded progressive
  non-reference B-slice prerequisite。
- 2026-08-08：完成有界 non-reference B-slice header 增量。ADR-0061 与英中文 bundled-profile
  参考提交 `4abf385` 固化边界：`NonIdrSliceType` 增加 `b = 1` 与 `all_b = 6`，并把既有
  P-slice 分支拓宽为共享 reference-list 分支，而不是新增平行的 B-only 副本。该形状由
  语言约束决定而非偏好：structure 的 field name 共享一个扁平命名空间，独立 B-only 分支会
  重复声明 `num_ref_idx_active_override_flag` 与 `ref_pic_list_modification_flag_l0`；
  `if` condition 只接受 computed<bool>、field equality 或 imported `context_value` equality，
  无法内联写出 `is_p_slice || is_b_slice`，因此需要 `uses_reference_lists` computed Boolean。
  实现提交 `73a5156` 让 B slice 在 reference-count override 之前读取
  `direct_spatial_mv_pred_flag`，在 override 分支内追加 `num_ref_idx_l1_active_minus1`，
  复用既有 list 0 modification loop，随后读取由 `@equals(0)` 约束的
  `ref_pic_list_modification_flag_l1`——list 1 loop 需要第二套投射名，故暂以致命约束封边。
  新增 source-anchored assertion 要求 `weighted_bipred_idc != 1`，因此 default(0) 与
  implicit(2) biprediction 受支持，explicit(1) 在 `pic_parameter_set_id` 处失败；
  `cabac_init_idc` 的 guard 从 `is_p_slice` 改为 `uses_reference_lists`。type-1 direct header
  继续要求 `nal_ref_idc == 0`，因此没有 reference-picture marking。structure 现在发布三个
  top-level computed Boolean 而非一个，type-1 的 child index 整体后移两位，18 个既有测试
  已同步更新；这是可见的呈现变化，不影响任何字段的 source span 与取值。package 更新为
  `0.1.17`，coverage depth 更新为 `i-p-b-slice-header`。新增九条回归覆盖 B 值 1/6、
  direct flag 两种状态、l0/l1 override count、复用的 modification loop、非零 list 1 flag 的
  致命失败、explicit/implicit biprediction，以及 entropy-coded B 的 `cabac_init_idc`；
  九条在旧规则下全部失败、在新规则下全部通过，H.264 analyzer 定向套件为 88/88。
  `svtool rule check` 通过；本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与 CTest 均为
  32/32。hosted run `31241123551` 对 `73a515677a660d34450aa6e8403d187db29cf42c`
  的 Windows 2022、macOS 15、Ubuntu 24.04 Configure、Build、Test、Install、Upload 全部成功。
  完整 Baseline/Main/High slice-header 项仍未完成；下一步定义并实现有界 list 1
  reference-list modification loop，这需要为第二个 loop 取一套不同的投射名。
- 2026-08-08：完成有界 list 1 reference-list modification loop 增量。ADR-0062 与英中文
  bundled-profile 参考提交 `91faab6` 固化决策：用真正的 loop 取代
  `ref_pic_list_modification_flag_l1` 上的 `@equals(0)` 约束，形状镜像 list 0。clause
  7.3.3.1 的两个 loop 结构完全相同且复用同一批 syntax element 名字，因为每个 loop 自带
  作用域；而本语言的 structure 只有一个扁平字段命名空间，因此 list 1 的投射名带 `_l1`
  后缀。该后缀只用于呈现层消歧，不表示另一个 syntax element，将来若引入 scope 构造可在
  不改变解码 bit 布局的前提下移除。实现提交 `c69e02e` 让 flag 为零时不发布任何 loop
  字段，为一时进入 64 次 sentinel repeat；复用闭集 `ModificationOfPicNumsIdc` enum，
  因此保留值仍在完整 Exp-Golomb 码字处致命失败，终止值 3 保留在树中。64 是 sentinel
  repeat 的语言上界而非 profile 选择，两个 loop 各自独立受界，因此一个 B slice 最多可
  投射 128 个 operation。package 更新为 `0.1.18`，coverage depth 保持
  `i-p-b-slice-header`。四条新回归取代了已过时的 `@equals` 断言，覆盖首轮即终止、单个
  list 1 loop 中的 operation 0/1/2/3 全集、同一 slice 中两个独立 loop，以及保留
  operation code；四条在旧规则下全部失败、在新规则下全部通过，H.264 analyzer 定向套件
  为 91/91。`svtool rule check` 通过；本机 `dev`、`ci`、`sanitize` 重新配置、完整构建与
  CTest 均为 32/32。hosted run `31242093716` 对 `c69e02e` 的 Windows 2022、macOS 15、
  Ubuntu 24.04 Configure、Build、Test、Install、Upload 全部成功。完整 Baseline/Main/High
  slice-header 项仍未完成；下一步定义并实现 nonzero-reference type-1 slice header 的
  有界 `dec_ref_pic_marking()`，这将首次解除 type-1 的 `nal_ref_idc == 0` 前置约束。
- 2026-08-08：完成 `header_value` sequence-element expression leaf 这一使能能力增量。
  开始 `dec_ref_pic_marking()` 前的独立核验确认它当前**不可表达**：该语法的 presence
  取决于 `NalUnitHeader` 的 `nal_ref_idc`，而 payload request 不携带 header 值（VM 按
  正在执行的 structure 单独建立空环境）；context 机制按 source position 解析，payload 的
  查询位置是 NAL 起点而 definition 只在其 exclusive end 处可选，因此 slice 会导入**上一个**
  NAL 的 header；context kind 也是四个 parameter-set kind 的闭集；dispatch controller 同样
  从不进入 payload 环境。因此本增量先补语言能力，与 ADR-0054、ADR-0056 的做法一致。
  ADR-0063 与英中 format-language 参考提交 `29556cf` 固化 `header_value(element_field)`：
  它镜像既有 `context_value(...)` leaf，接受且只接受一个 identifier 实参，在编译期针对
  程序唯一的 sequence element structure 解析，要求无条件、顶层、非数组的 unsigned scalar，
  并拒绝缺少 sequence、未知或不合格的 element 字段，以及写在 element structure 自身内部的
  调用。由于它是 call 而不是 identifier，element 与 payload 的命名空间保持分离——这正是
  ADR-0037 拒绝把 dispatch 放进 header 时所要求的。实现提交 `3f64b27` 覆盖 parser、
  typed IR（新增 `SequenceElementReference` kind，携带已解析 element field index，不新增
  opcode）、compiler 静态校验与 VM 预验证；runner 在物化 NAL header 时顺带捕获 element
  字段值（它本来就要从中读出 dispatch controller），并随 execution request 提供。index
  越界、值缺失或值向量缺席都作为 invalid definition 失败，不用猜测值解码。bundled rule
  本次不改动，package 版本保持 `0.1.18`。回归覆盖 parser arity/identifier 规则、五类静态
  拒绝、typed IR 降低、以及 session 层依据 element 值分支与缺少值向量时的失败；通过临时
  篡改求值路径确认新测试确为有效信号。DSL parser 59/59、IR 67/67、executor 118/118、
  session 35/35、H.264 analyzer 91/91；`svtool rule check` 通过；本机 `dev`、`ci`、
  `sanitize` 重新配置、完整构建与 CTest 均为 32/32。hosted run `31250687630` 对
  `3f64b27` 的 Windows 2022、macOS 15、Ubuntu 24.04 Configure、Build、Test、Install、
  Upload 全部成功。下一步用该能力实现有界 `dec_ref_pic_marking()`，解除 type-1 的
  `nal_ref_idc == 0` 前置约束。
- 2026-08-08：完成有界 reference-picture marking 增量，首次解除 type-1 的
  `nal_ref_idc == 0` 前置约束。ADR-0064 与英中文 bundled-profile 参考提交 `db48349`
  固化边界：移除 ADR-0054 引入的该前置 assertion，改用 ADR-0063 的 `header_value` leaf
  表达 clause 7.3.3 的 presence 条件，因此 profile 现在接受 reference P/B slice，而不再
  只支持 non-reference。实现提交 `698092a` 让 `adaptive_ref_pic_marking_mode_flag` 为零时
  选择 sliding-window marking 且不再发布字段；为一时进入 64 次有界 loop，operation 1/3 读
  `difference_of_pic_nums_minus1`、operation 2 读 `long_term_pic_num_mmco`、operation 3/6
  读 `long_term_frame_idx`、operation 4 读 `max_long_term_frame_idx_plus1`，operation 0/5
  不读 operand，terminator 保留在树中。`MemoryManagementControlOperation` 为覆盖 `0..6` 的
  闭集 enum：保留值不对应任何 operand 组合，属 layout-critical，故在完整码字处致命失败。
  扁平命名空间中只有 clause 7.3.3.3 的 `long_term_pic_num` 与 list 0 loop 冲突，因此仅它
  改名为 `long_term_pic_num_mmco`，其余四个 operation 字段保留 clause 名——与 ADR-0062 的
  统一 `_l1` 后缀不同，因为那里有四个名字冲突。guard 反转为空 `then` 分支，因为 `if`
  condition 只接受 equality，无法写 `header_value(...) != 0`。package 更新为 `0.1.19`，
  coverage depth 更新为 `i-p-b-reference-slice-header`（本增量拓宽的是所接受的 slice 范围）。
  测试影响：`loadsBundledRule` 的 `NalUnitHeader` item 数减一；
  `rejectsNonIdrReferenceNalBeforePayloadMapping` 断言的正是本次移除的行为，故整体替换而非
  修补。四条新回归覆盖 sliding-window marking、non-reference slice 的 marking 缺席、四个
  operand 组合的精确 source span，以及保留 operation 的致命失败；其中三条在旧规则下失败、
  在新规则下通过（marking 缺席那条在两版下都通过，它守的是边界而非新代码）。写 ADR 前先在
  scratch 副本上用 `svtool analyze` 实际解码五个 fixture 核对字段 presence 与 span，落地时
  规则文本与该已验证副本逐字节相同。H.264 analyzer 定向套件为 94/94，`svtool rule check`
  通过；本机 `dev`、`ci`、`sanitize` 完整构建与 CTest 均为 32/32。hosted run `31253798200`
  对 `698092a` 的 Windows 2022、macOS 15、Ubuntu 24.04 Configure、Build、Test、Install、
  Upload 全部成功。至此 type-1 slice-header 覆盖 reference 与 non-reference 两种形态下的
  I、P、B slice；完整 Baseline/Main/High slice-header 项仍未完成，下一步定义并实现有界
  `pred_weight_table()`，以解除 `weighted_pred_flag == 0` 与 `weighted_bipred_idc != 1`
  这两条 imported assertion。
- 2026-08-08：完成「computed initializer 接受保留 external leaf」这一使能能力增量。
  上一轮批准的 ADR-0065 草案在写代码前先被 scratch 探测**推翻**：`pred_weight_table()`
  需要有效的 entry count，而该 count 要么来自本地 override 字段、要么来自 imported PPS
  默认值，语言没有在二者之间选择的表达式；草案原本打算用一条 narrowing assertion 绕开这个
  选择，但那条 assertion **没有合法位置**——写在顶层时 `num_ref_idx_active_override_flag`
  报「dependency is not guaranteed on the current branch」，写进该 flag 可用的
  `if (uses_reference_lists)` 内部则报「Assertions must be unconditional top-level items」。
  探测还额外确认 `repeat (integer)` 永远是 sentinel 形式，因此固定 2 次的 Cb/Cr 循环不可
  表达（这独立证明草案里的 `_cb`/`_cr` 命名是必需而非风格选择），以及 `computed<u64>`
  确实可以驱动 count repeat、嵌在 conditional 里的 `computed<bool>` 确实可以 guard 后续
  `if`。于是本增量改为只补语言能力，与 ADR-0054、ADR-0056、ADR-0063 的做法一致。
  ADR-0065 与英中 format-language 参考提交 `8d54240` 固化：`computed<bool>` 与
  `computed<u64>` 的 initializer 按**完整 expression 语法**接受 `context_value(...)` 与
  `header_value(...)`，不是固定比较形状；两个 leaf 在别处携带的每一项约束继续适用，包括
  structure 未声明匹配 `@context_import` 时的拒绝。两个 leaf 一起放开，因为它们是同一套
  机制、在同样三处开关。实现提交 `3cfef2b` 只打开三道前端闸门：parser 的
  `validateExpression` 调用点、IR lowering 的 resolver 赋值、typed-expression validation
  state。**求值路径无需改动**——VM 在 computed 位置本来就同时传入 imported-context
  resolver 与 sequence-element value vector，与 assertion 位置逐字相同；因此不新增语法、
  expression kind 或 opcode。三条既有负向测试断言的正是本次解除的限制：parser 那条改为
  断言接受，IR index 8 那条移除 `@context_import` 以保持因合法理由无效，VM 那条
  malformed-program 直接删除（该程序现已合法）。端到端回归用 `header_value(nal_ref_idc)`
  同时驱动一个 `computed<bool>` guard 与一个 `computed<u64>` repeat controller，三组
  scenario 的 bit 消费量与预测完全一致。bundled H.264 规则**故意不改**：清理版本已在
  probe 上验证通过且与目标逐字节相同，但计算字段总是物化为可见节点（无隐藏机制），落地会
  在 non-IDR slice structure 尾部插入 2 个节点，导致 24 个 analyzer 测试的 106 处硬编码
  child index 偏移，而解码行为零变化——沿用 ADR-0063 的 capability-only 先例，package
  版本与 `rule.toml` 均不变。H.264 analyzer 定向套件 94/94，`svtool rule check` 通过；
  本机 `dev`、`ci`、`sanitize` 完整构建与 CTest 均为 32/32。hosted run `31258216794` 对
  `90910e6` 的 Windows 2022、macOS 15、Ubuntu 24.04 Configure、Build、Test、Install、
  Upload 全部成功。下一步先决定
  `pred_weight_table()` 的 count 选择方案：一是在 override 与默认两个分支下复制表体并加
  区分后缀（今天即可表达，代价是四份近似副本、字段名随一个无关 flag 变化），二是新增带
  flow-sensitive 依赖分析的 defaulting/conditional expression（保住 spec 命名，代价是再
  一个能力 ADR）；建议取二，因为复制方案会在后续每张表上复利式膨胀。
- 2026-08-08：新增 `optional_value(field, fallback)` expression leaf（承接上一条的方案二）。
  决策前先用探测把问题钉死，而不是照图纸设计：最小 blocker 精确复现
  `error: Computed field dependency is not guaranteed on the current branch`，复制方案
  也真的编译通过（`Rule OK`），因此 ADR 里的对比是实测而非推断——代价确认为字段名被迫
  变成 `weight_flag_override` / `weight_flag_default`。随后在源码里核实三条运行期事实：
  VM 的 `fieldValues` 本来就是 `std::vector<std::optional<quint64>>`；空槽今天之所以是硬
  失败，只是因为静态规则承诺它不会发生；repeat body 每次迭代都 `resize(scopeStart)`，所以
  index 不可能与后续 projection 混淆。结论是**这条限制纯属静态，运行期早已知情**。
  ADR-0066 与英中 format-language 参考提交 `480f661`：leaf 在实际执行路径物化了该字段时
  给出其值，否则给出 fallback；**只有第一个实参**豁免 branch-guarantee，其余每条 dependency
  规则照旧。有意不要求第一个实参必须 branch-local——否则后续一个无关 guard 会反过来让一处
  正确用法失效。Boolean 形式与通用三元表达式明确列为 non-goal，也不暴露 `has_value`。
  实现提交 `bb4e223` 在**三层各自独立检查 dependency 的地方**都设闸：parser 用专用
  resolver（只省掉 availability 检查）解析被命名字段，在拒绝该形式的位置传空 resolver；
  IR 降低为新的 `OptionalFieldReference` kind，携带解析出的 field index 与编译后的
  fallback 作为唯一 operand；VM 验证该 descriptor 并在字段已物化时读取记录值，否则求值
  fallback。**不新增 opcode、不新增结构体成员。** 探测确认四个位置（dynamic width、
  assertion condition、computed initializer、lazy byte count）全部可用；lazy 位置额外
  受既有的 byte-boundary 约束，与本 leaf 无关。端到端 session 测试用同一程序跑两组
  scenario：override 分支取本地值（effective 3、16 bit），未取时 fallback 落到 imported
  PPS 默认（effective 5、15 bit）——若 fallback 恒胜第一组会读到 5，若空槽仍是硬失败第二组
  会直接失败，两组因此互为判别。parser 61/61、IR 70/70、session 37/37；本机 `dev`、`ci`、
  `sanitize` 均 32/32；H.264 analyzer 94/94，`svtool rule check` 通过。本增量**仍然只补
  语言能力**，不动 bundled 规则，与 ADR-0063、ADR-0065 先例一致；消费它的 bounded
  `pred_weight_table()` 连同其 analyzer child index 偏移作为下一个增量。hosted run
  `31260642168` 对 `9d867de` 的 Windows 2022、macOS 15、Ubuntu 24.04 Configure、Build、
  Test、Install、Upload 全部成功。
- 2026-08-09：解除 `weighted_pred_flag == 0` 与 `weighted_bipred_idc != 1` 两条 imported
  assertion，改为解码有界的 clause 7.3.3.2 `pred_weight_table()`（消费 ADR-0066 的
  `optional_value`）。**表体从来不是障碍，count 才是**——这也是前两个增量分别铺路的原因。
  写文档前先用探测钉死上一轮交接时仍未定的设计问题：把 `if (is_p_slice)` 与 `if (is_b_slice)`
  各自嵌套一层 imported 相等判断，会被 `error: Duplicate field name` 拒绝，因为 structure
  只有一个扁平字段命名空间、互斥分支不能重名；嵌套因此会迫使整张表复制两份，正是 ADR-0066
  要避免的代价。于是 presence 条件由**单个** `computed<bool> uses_explicit_weighting` 承载。
  另外两点也是实测而非推断：完整表体（l0/l1 repeat、四个 Cb/Cr 字段、两处 `optional_value`）
  在真实 bundled 规则上 `Rule OK`，`optional_value` 确实是唯一缺失能力；色度字段**无条件
  存在**，因为 `chroma_format_idc` 只在 `profile_idc == 100` 下声明且被 `@equals(1)` 钉死、
  其余受支持 profile 根本不声明它，故受支持子集内 ChromaArrayType 恒为 1，不需要新增 SPS
  export 或改 context 契约。`weighted_bipred_idc == 3` 也**不需要**新 assertion：`@enum` 本
  就提供致命校验而 `WeightedBipredIdc` 只声明 0/1/2，保留值早已在读取它的 PPS 处
  `invalid-syntax`——那才是正确的拦截点。ADR-0067 与英中 format-language 参考提交 `4a83d44`；
  实现提交 `e5ff808` 中两个 loop 都以 32 项为界，与 count 字段已有的 `@range(0, 31)` 一致；
  `_cb`/`_cr` 后缀是必需而非风格，因为单整数 `repeat` 永远是 sentinel 形式、固定两次的
  chroma 循环不可表达。测试影响先**实测**（应用改动后跑套件）再处理：24 个失败中 22 个是纯
  索引平移，child count 一律 +1——计算字段总会物化为可见节点，于是每个 non-IDR slice 都多一
  个节点，与它是否真的带表无关；另 2 个断言的正是本增量解除的限制，因此整体重写。这 2 个重
  写扩成 5 个测试，覆盖两个 `optional_value` 叶子各自的两种状态：P/B slice 的 count 分别取
  imported PPS 默认与 slice header 声明的 override，外加一个被码流末尾截断的表（终止在
  `luma_weight_l0[0]` 与 `luma_offset_l0[0]` 之间，确认部分前缀仍物化、诊断锚定在未读到的
  字段上）。每个测试断言**完整有序的 child 名字列表**而非位置索引，后续增量插入字段时会以名
  字不匹配的形式失败，而不是变成一次被静默平移的比较。所有码流由生成脚本装配、解码结果经
  `svtool analyze` 回读，因此每个被断言的值都是实测而非手算——首轮就靠这条发现 B slice 的
  `direct_spatial_mv_pred_flag` 会多吃一个 bit、使原本的取值全部错位。package 更新为
  `0.1.20`，是四个增量以来**第一个真正改变解码输出**的 bundled 规则变更（前三个为纯能力增量
  并有意不升版本）。H.264 analyzer 97/97，`svtool rule check` 通过；本机 `dev`、`ci`、
  `sanitize` 完整构建与 CTest 均为 32/32，并额外跑通 CI 也会执行的 Release install。落地后
  补做了一次反向核验：把 svfmt 单独回退到 `a324e39` 重建，5 个新测试全部 red，恢复后 97/97
  green，因此它们确实判别的是本增量的行为而不是恒真。hosted run `31293182690` 对 `7f093d8`
  的 Windows 2022、macOS 15、Ubuntu 24.04 Configure、Build、Test、Install、Upload 全部成功。
- 2026-08-09：解码场图像 slice header（`field_pic_flag` / `bottom_field_flag`）。`frame_num` 上那个
  除以 `frame_mbs_only_flag` 的 ADR-0049 guard 并不属于 clause 7.4.3 规定的字段宽度，它只是用来拒绝
  隔行码流，因此移除；两个 slice struct 改为在 imported equality 下读 `field_pic_flag`，并在其内层嵌
  `bottom_field_flag`。`delta_pic_order_cnt_bottom` 的条件是复合的，而 imported-condition 文法只接受
  `context_value(a, b, c) == <整数>` 这一确切形式，因此折进一个 `computed<bool>`，用
  `optional_value(field_pic_flag, 0)` 承接那个自身就是条件性的字段——靠的正是 ADR-0066 只豁免第一个
  实参的规则。fallback 取 0 是在重述 clause 7.4.3 的推断，而不是挑一个方便的默认值。同时接受了 MBAFF
  帧：它 header 布局与场图像相同，宏块自适应只改变不透明 `slice_data` 的解释，因此不需要额外语法。
  34 个既有测试失败，全部是 +1 child 平移，**没有一个是语义变更**。两种失败形态安全性不对等：断言有序
  名字列表的以名字不匹配失败，而按位置索引的**直接 SIGSEGV**——`at(8)` 仍返回有效节点，但那已是计算
  guard，其 `location()` 为空。插入位置由 `svtool analyze` 实测（non-IDR 索引 8、IDR 索引 7），不靠读
  规则推导。新增 5 个测试：底场（其 PPS 启用 bottom-field POC，证明的是场图像**抑制**该字段）、MBAFF
  帧带 delta、IDR 布局、两个 flag 之间的截断（12 bit `frame_num` 把 `field_pic_flag` 顶到 payload 末
  位）、以及 progressive 回归。fixture 由生成脚本装配，该脚本先逐字节复现两个**已提交**的既有 fixture
  做自检。package 升到 `0.1.21`，coverage depth 改 `field-picture-slice-header`。H.264 analyzer
  102/102，`svtool rule check` 通过；本机 `dev`、`ci`、`sanitize` 三套 CTest 均 32/32。另修正计划头部
  `Next Action`：SP/SI 属 Extended profile，不在 Baseline/Main/High slice-header 里程碑的路径上。
  hosted run `31320789048` 对 `685b615` 的 Windows 2022、macOS 15、Ubuntu 24.04 Configure、Build、
  Test、Install deployable tree、Upload package tree 全部成功。
- 2026-08-10：完成有界 repeat-local assertion 与 H.264 marking 关系切片。ADR-0069 允许
  `assert(condition) at anchor;` 出现在 bounded/sentinel repeat 及其 conditional/switch body；compiler
  按静态 projection iteration 展开 assertion，descriptor 保存 active conditions，VM 在读取 anchor
  range 前检查 branch conditions。官方规则将 SPS `max_num_ref_frames` 导出，并为 operation 2 的
  `long_term_pic_num_mmco`、operation 3/6 的 `long_term_frame_idx`、operation 4 的
  `max_long_term_frame_idx_plus1` 增加 source-anchored bound assertion；超限 slice 保留前缀并继续
  分析后续 NAL。package 升到 `0.1.22`，coverage depth 为 `relational-marking-slice-header`。
  `streamview_dsl_tests` 62/62、`streamview_dsl_ir_tests` 71/71、`streamview_dsl_executor_tests`
  120/120、`streamview_h264_annex_b_analyzer_tests` 103/103，`svtool rule check` 通过；本机
  dev/ci/sanitize 完整构建与 CTest 均为 32/32。hosted run `31391608560` 的 Ubuntu 24.04 /
  Qt 6.11.1、Windows 2022 / Qt 6.10.1、macOS 15 / Qt 6.11.1 jobs 全部成功。
  `difference_of_pic_nums_minus1` 的
  MaxPicNum relation、DPB/order/duplicate semantics 明确保留为后续工作。
- 2026-08-10：完成 marking operation 1/3 的 MaxPicNum relation。ADR-0070 新增受限
  `power_of_two(u64)` expression leaf：exponent `0..63` 产生 `1 << exponent`，更大值在外层
  expression 的既有 instruction 内返回致命 `invalid-syntax`；parser/compiler 校验 arity/type，
  typed IR 保存单 operand `PowerOfTwo` node，VM preflight 在 shift 前验证 descriptor 与 exponent。
  官方规则把 SPS `log2_max_frame_num_minus4 + 4` 转为 `MaxFrameNum`，再乘以
  `optional_value(field_pic_flag, 0) + 1` 得到 frame/field `MaxPicNum`，以 repeat-local assertion
  约束 operation 1/3 的 `difference_of_pic_nums_minus1`。新增 frame fixture 在最小
  `MaxPicNum == 16` 时提交 operand 16，精确锚定 9-bit codeword 并验证后续 NAL 继续分析。
  package 升到 `0.1.23`，coverage depth 为 `max-pic-num-marking-slice-header`。定向 DSL/H.264
  套件为 DSL 64/64、IR 72/72、executor 121/121、H.264 analyzer 104/104，
  `svtool rule check` 通过；回归同时覆盖 exponent 0/63/64+、operation 1/3 与
  frame/field MaxPicNum 边界。完整本机 dev/ci/sanitize 构建与 CTest 均为 32/32，
  实现拆分为 `c6fb05f` 与 `116271c`。hosted run `31396375794` 的 Ubuntu 24.04 /
  Qt 6.11.1 job `93480182939`、Windows 2022 / Qt 6.10.1 job `93480182983`、macOS 15 /
  Qt 6.11.1 job `93480183106` 全部成功。下一步进入
  `pic_order_cnt_type` 1/2 的 SPS 与 slice-header 分支；DPB/order/duplicate semantics 继续延期。
- 2026-08-11：完成 `pic_order_cnt_type` 1/2 的 SPS 与 IDR/non-IDR slice-header 语法分支。
  ADR-0071 将 POC type 建模为 0/1/2 闭集 enum；type 0 保持既有动态宽度字段顺序，type 1
  解码 `delta_pic_order_always_zero_flag`、两个 SPS signed offset、最多 255 项的 reference
  offset cycle，并按帧/场条件发布零个、一个或两个 `delta_pic_order_cnt[index]`，type 2 不读取
  额外 POC syntax。两个无 source location 的 computed export 会把条件 SPS 字段归一化后交给
  slice context；cycle count 同时受非致命 `@range(0, 255)` 与 layout-critical
  `repeat(..., 255)` 约束。package 升到 `0.1.24`，coverage depth 为
  `picture-order-count-slice-header`。新增回归覆盖 type-1 SPS/source span、IDR 两个 delta、
  non-IDR 一个 delta、always-zero 缺席、场图像抑制第二个 delta、type-2 IDR/non-IDR 缺席、reserved
  type、cycle count 0/256 的边界，以及 cycle entry 截断后继续分析下一 NAL。`svtool rule check`
  与 H.264 analyzer 112/112 已通过；本机 dev/ci/sanitize 三套 CTest 均为 32/32。实现与文档
  分别提交为 `e1db76a` 与 `0b50428`。hosted run `31457193468` 的 Ubuntu 24.04 /
  Qt 6.11.1 job `93673379241`、Windows 2022 / Qt 6.10.1 job `93673379325`、macOS 15 /
  Qt 6.11.1 job `93673379309` 全部成功。
  下一步审计 Baseline/Main/High 8-bit 4:2:0 slice-header 的剩余缺口，优先确认 High-profile PPS
  extension prerequisite；实际 POC、field order、wrap/MMCO-5、DPB 与 output order 继续延期。
- 2026-08-11：完成 High-profile PPS extension 所需的 `more_rbsp_data()` DSL source-state
  prerequisite。ADR-0072 拒绝用 profile 猜测 extension presence，也不引入通用 EOF、remaining
  count 或任意 lookahead；零参数 leaf 只在 structure 执行期 expression 中返回 `bool`，pure
  function body 与同名 pure declaration 均被拒绝。parser/compiler 独立检查作用域与 arity，
  typed IR 新增零 operand `MoreRbspData` kind，VM 在外层 expression instruction 内探测当前
  `BitReader` 的副本：剩余零 bit 返回 false，多于八 bit 返回 true，一至八 bit 仅在完整
  remainder 为 `1` 后全零时返回 false。成功、`EndOfSource` 与 source error 都不移动执行
  cursor；错误沿用 truncated/source-error 状态。本增量不新增 opcode、不修改 bundled H.264
  规则，因此 package 版本保持 `0.1.24`。测试覆盖 parser 正反例、typed lowering、trailing-only、
  一至八 bit 的 non-trailing、超过八 bit、跨 mapping span、零剩余、truncated/source error、
  malformed descriptor 与 reader 非消费。DSL parser 66/66、IR 73/73、executor 122/122，
  `svtool rule check` 与 H.264 analyzer 112/112 通过；本机 dev/ci/sanitize 完整构建与 CTest
  均为 32/32。设计与实现分别提交为 `b31144b` 与 `04a49e9`。hosted run
  `31493517456` 的 Ubuntu 24.04 / Qt 6.11.1 job `93785464149`、Windows 2022 /
  Qt 6.10.1 job `93785464275`、macOS 15 / Qt 6.11.1 job `93785464154` 全部成功。
  下一步消费该 leaf 解码 High-profile PPS 的 `transform_8x8_mode_flag`、受限为零的
  `pic_scaling_matrix_present_flag` 与 `second_chroma_qp_index_offset`，同时保持合法的
  High-profile base-only PPS 可 materialize。
- 2026-08-11：完成有界 High-profile PPS extension。ADR-0073 要求 SPS 导出无条件
  `profile_idc`，PPS 在保留 SPS dependency 的同时导入同一 generation；base 字段后的
  `has_pps_extension = more_rbsp_data()` 区分合法 base-only PPS 与实际 extension。只有
  `profile_idc == 100` 可进入 extension，并按顺序解码 `transform_8x8_mode_flag`、受限为零的
  `pic_scaling_matrix_present_flag` 与 signed `second_chroma_qp_index_offset`；scaling-list 分支
  继续作为 layout-critical unsupported，terminal `rbsp_trailing_bits` 保持最终无条件 item。
  package 升到 `0.1.25`，coverage depth 保持 `picture-order-count-slice-header`。回归覆盖 High
  base-only、transform flag 两种值、正负 second offset 与精确 source span、scaling-matrix
  拒绝、Baseline/Main/Extended profile gate、offset 截断、missing/future/stale SPS、失败 SPS
  重定义恢复，以及失败 NAL 后继续扫描。`svtool rule check` 与 H.264 analyzer 121/121 已通过；
  本机 dev/ci/sanitize 完整构建与 CTest 均为 32/32，且无 sanitizer 报告。hosted run
  `31496827386` 的 Ubuntu 24.04 / Qt 6.11.1 job `93796543197`、Windows 2022 /
  Qt 6.10.1 job `93796543158`、macOS 15 / Qt 6.11.1 job `93796543359` 全部成功。设计提交为
  `b2b41a8`，实现提交为 `9187cac`，双语覆盖记录为 `78377e5`。后续审计确认当前单
  slice-group、无 scaling-list 的 I/P/B 范围内，clause 7.3.3 位消费分支已经完整；下一步为
  IDR `idr_pic_id` 增加 `0..65535` 非致命范围校验。实际 POC、field order、wrap/MMCO-5、
  DPB 与 output order 继续延期。
- 2026-08-11：完成 IDR `idr_pic_id` 的 `0..65535` 非致命值域校验。ADR-0074 将该约束
  定义为不改变布局的 semantic value domain：完整 Exp-Golomb 码字仍决定后续字段起点，
  越界时只在字段节点附加 source-located `invalid-syntax` warning，slice 保持 materialized。
  官方规则增加 `@range(0, 65535)` 与 clause 7.3.3/7.4.3 引用，package 升到 `0.1.26`，
  coverage depth 保持 `picture-order-count-slice-header`。回归覆盖上界 `65535` 与首个非法值
  `65536`；两者均为 33-bit 码字，并精确验证后续 POC、IDR marking、QP、opaque payload
  位置及下一 NAL 未移动。设计提交为 `60a85b2`，实现提交为 `d3b6587`；英中 bundled-profile
  字段说明与 package 记录已同步。`svtool rule check` 与 H.264 analyzer 122/122 通过；本机
  dev/ci/sanitize 重新配置、完整构建与 CTest 均为 32/32，且无 sanitizer 报告。hosted matrix
  验证待 push 后记录。下一步把既有非致命 `@range` 合同扩展到 unsigned fixed/dynamic
  `bits`，再约束 IDR `frame_num == 0`；实际 POC、field order、wrap/MMCO-5、DPB 与
  output order 继续延期。
- 2026-08-11：确认 IDR picture identifier 增量的 hosted run `31499322192` 成功：Ubuntu
  24.04 / Qt 6.11.1 job `93804895854`、Windows 2022 / Qt 6.10.1 job `93804895734`、
  macOS 15 / Qt 6.11.1 job `93804895870` 的 Configure、Build、Test、Install 与 Upload
  全部通过。
- 2026-08-11：完成 unsigned fixed/dynamic `bits` 的非致命 `@range` 与 IDR
  `frame_num == 0` 值域校验。ADR-0075 把既有 range contract 扩展到 fixed `bits<N>` 与
  dynamic `bits<expression>`：前者的 maximum 必须适配静态 width，后者接受完整 `u64`
  annotation domain；`ue` 仍以 `2^64 - 2` 为上界，`se` 暂时拒绝 `@range`。enum-backed
  fixed bits 可同时执行 fatal enum/equality 与 non-fatal range，dynamic bits 只接受 range。
  VM 在读取 source 前预检 range descriptor、两条相邻 opcode、operand、immediate、顺序与
  数量，malformed typed IR 不消耗 bit、不移动 reader 且不创建节点。官方 H.264 规则仅在
  IDR branch 给 dynamic-width `frame_num` 添加 `@range(0, 0)`；非 IDR branch 保留该 width
  的完整表示域，违规 IDR 仍完整物化后续 POC、marking、QP 与 opaque payload。package 升到
  `0.1.27`，coverage depth 保持 `picture-order-count-slice-header`。设计提交为 `6f2f4ee`，
  实现提交为 `282ecab`。parser 67/67、IR 74/74、executor 124/124、H.264 analyzer 123/123
  与 `svtool rule check` 通过；本机 dev/ci/sanitize 重新配置、完整构建与 CTest 均为 32/32，
  且无 sanitizer 报告。hosted matrix 验证待 push 后记录。下一步扩展静态非致命 range 到
  signed `se`，并约束 `slice_alpha_c0_offset_div2` 与 `slice_beta_offset_div2` 为 `-6..6`；
  SPS/PPS-dependent `slice_qp_delta`、实际 POC、field order、wrap/MMCO-5、DPB 与 output
  order 继续延期。
- 2026-08-14：完成 signed `se` 的非致命 `@range` 与两个 slice deblocking offset 的
  `-6..6` 值域校验。ADR-0076 把既有 range contract 扩展到 `se`：annotation 参数允许前导
  `-`，AST 保留无符号 magnitude 加 negative 标志，因此 unsigned `@range` 仍可覆盖到
  `2^64 - 2`；signed 静态值域取 `readExpGolomb` 的 `magnitude = (codeNumber + 1) / 2`
  推导出的对称区间 `-(2^63 - 1)..2^63 - 1`，`-0` 归一化为零。typed IR 新增独立的
  `signedRangeConstraint` descriptor，但不新增 opcode：仍复用 `assert-range-minimum` 与
  `assert-range-maximum`，bound 以补码存入 immediate；VM 按字段编码选择有符号或无符号
  比较，并在预检阶段拒绝 descriptor 与 encoding 不匹配的配对（无符号字段带 signed
  bound、`se` 字段带 unsigned bound、两种 constraint 同时存在或缺失）。`se` 仍拒绝
  `@equals` 与 `@enum`。官方 H.264 规则给 IDR 与非 IDR slice header 的
  `slice_alpha_c0_offset_div2` 与 `slice_beta_offset_div2` 各加 clause 7.4.3 的
  `@range(-6, 6)` 与 7.3.3/7.4.3 引用，package 升到 `0.1.28`，coverage depth 保持
  `picture-order-count-slice-header`。回归覆盖负数 annotation 字面量、合法极值 `-6`/`6`、
  首个非法值 `-7`/`7`、两个 offset 各自的违规、每种被拒 descriptor 配对的 malformed
  typed IR，并精确验证兄弟 offset、opaque payload 与下一 NAL 未移动：合法与非法取值都用
  7-bit 码字配对，因此“后续字段未移动”是合同保证而非编码宽度巧合。设计提交为 `81fd0ee`，
  实现提交为 `799b4a5`。
  `svtool rule check`、parser 70/70、IR 76/76、executor 127/127、H.264 analyzer 124/124
  通过；本机 dev/ci/sanitize 重新配置、完整构建与 CTest 均为 32/32，且无 sanitizer 报告。
  hosted matrix 验证待 push 后记录。下一步把静态 signed `@range` 应用到 clause 7.4.2.2
  给出字面量值域的 PPS QP offset：`pic_init_qs_minus26` 取 `-26..25`，两个
  `chroma_qp_index_offset` 取 `-12..12`。`pic_init_qp_minus26` 与 `slice_qp_delta` 继续
  延期，因为它们的值域依赖 SPS 派生的 `QpBdOffsetY`，而 `@range` 只接受整数字面量，无法
  表达关系型 bound；实际 POC、field order、wrap/MMCO-5、DPB 与 output order 也继续延期。
- 2026-08-14：确认 unsigned bit range 与 IDR frame number 增量的 hosted run `31502042652`
  成功：Ubuntu 24.04 / Qt 6.11.1 job `93814256105`、Windows 2022 / Qt 6.10.1 job
  `93814255936`、macOS 15 / Qt 6.11.1 job `93814255922` 的 Configure、Build、Test、Install
  与 Upload 全部通过。
- 2026-08-14：确认 signed range 与 deblocking offset 增量的 hosted run `31782320789`
  成功：Ubuntu 24.04 / Qt 6.11.1 job `94710577209`、Windows 2022 / Qt 6.10.1 job
  `94710577185`、macOS 15 / Qt 6.11.1 job `94710577220` 的 Configure、Build、Test、Install
  与 Upload 全部通过。
- 2026-08-14：完成 PPS QP offset 的 static signed `@range` 校验。新增双语
  ADR-0077（`docs/adr/0077-bound-pps-quantization-parameter-offsets.md` 与
  `docs/zh-CN/adr/0077-bound-pps-quantization-parameter-offsets.md`），补充
  ADR-0041/0073/0076 后续链接。官方规则为 `pic_init_qs_minus26` 增加
  `@range(-26, 25)` 与 clause 7.4.2.2 引用，为 `chroma_qp_index_offset` 及
  `second_chroma_qp_index_offset` 增加 `@range(-12, 12)` 与 clause 7.4.2.2 引用；
  package 升到 `0.1.29`，coverage depth 保持 `picture-order-count-slice-header`。
  回归覆盖三个字段各自的合法极值（-26/25、-12/12）与首个非法值（-27/26、-13/13），
  合法与非法取值均配对使用相同码字宽度以验证兄弟字段、后续
  deblocking flag/extension/trailing bits 与下一 NAL 位置未移动；越界时只附加
  warning `invalid-syntax` 诊断，PPS 仍保持 materialized。`svtool rule check`、
  parser 70/70、IR 76/76、executor 127/127、H.264 analyzer 125/125 通过；本机
  dev/ci/sanitize 重新配置、完整构建与 CTest 均为 32/32，且无 sanitizer 报告。
  hosted run `31791302960` 在 Ubuntu 24.04 / Qt 6.11.1（job `94738594523`）、
  Windows 2022 / Qt 6.10.1（job `94738594486`）、macOS 15 / Qt 6.11.1
  （job `94738594522`）全部成功。
- 2026-08-14：完成同 ID SPS/PPS 中途重定义与按位置选择的格式级验收（阶段 3 第 5 项）。新增双语
  ADR-0078（`docs/adr/0078-bind-redefined-parameter-sets-by-stream-position.md` 与
  `docs/zh-CN/adr/0078-bind-redefined-parameter-sets-by-stream-position.md`），并在双语格式语言说明中补充重定义 generation 激活语义。
  在 H.264 analyzer 测试套件中增加正反例回归：正向用例验证 SPS id 0（`log2_max_frame_num_minus4=0`）经 PPS id 0 激活 Slice A（`frame_num` 为 4 bit），随后 SPS id 0 重定义（`log2_max_frame_num_minus4=2`）并重绑 PPS id 0 动态激活 Slice B（`frame_num` 为 6 bit），两个 slice 的完整有序子节点列表与物化状态均保持正确且无漂移；负向用例验证非法 SPS id 1（保留 profile 99）进入 invalid 状态且不污染 generation 表，依赖该 SPS 的 PPS id 1 与 Slice B 报告 `dependency-unavailable` 诊断，而先前的 Slice A 仍保持完整物化。`svtool rule check`、parser 70/70、IR 76/76、executor 127/127、H.264 analyzer 127/127 通过；本机 dev/ci/sanitize 均为 32/32 且无 sanitizer 报告。hosted run `31792636283` 在 Ubuntu 24.04 / Qt 6.11.1（job `94742760146`）、Windows 2022 / Qt 6.10.1（job `94742760226`）、macOS 15 / Qt 6.11.1（job `94742760849`）全部成功。
- 2026-08-14：只读审计 Baseline/Main/High 8-bit 4:2:0 单 slice group slice header 的语法覆盖（阶段 3 第 2 项）。
  对照 ITU-T H.264 标准：
  1. clause 7.3.3 主干语法：`first_mb_in_slice`（svfmt 318/414）、`slice_type`（320/416）、`pic_parameter_set_id`（322/421）、`frame_num`（324/423）、`field_pic_flag`/`bottom_field_flag`（332/430）、`idr_pic_id`（341）、`pic_order_cnt_lsb`/`delta_pic_order_cnt_bottom`/`delta_pic_order_cnt[0..1]`（344-375/439-470）、`redundant_pic_cnt`（379/474）、`direct_spatial_mv_pred_flag`（479）、`num_ref_idx_active_override_flag` 及 override counts（484-496）、`cabac_init_idc`（694）、`slice_qp_delta`（387/699）、`disable_deblocking_filter_idc` 及 offsets（392-403/704-715）均已在 `h264_annex_b.svfmt` 中完整实现；
  2. clause 7.3.3.1 参考图像列表修改：L0 与 L1（B-slice）循环及 0/1/2/3 操作（501-544）已完整实现；
  3. clause 7.3.3.2 预测权重表：luma/chroma 分母与 L0/L1 4:2:0 权重/偏移循环（552-628）已完整实现；
  4. clause 7.3.3.3 参考图像标记：IDR 5-bit flags（383-386）与非 IDR MMCO 0..6 循环及动态 bounds（631-689）已完整实现；
  5. `colour_plane_id`（4:4:4 profile 专属）、`sp_for_switch_flag`/`slice_qs_delta`（SP/SI profile 专属，ADR-0050）及 `slice_group_change_cycle`（多 slice group 专属，ADR-0041）按既定边界显式延期；
  6. `slice_data`（405/717）整体标记为不透明压缩载荷，符合里程碑合同。
  在双语格式语言说明中将 package `0.1.29` 的 coverage depth 正式升级为 `baseline-main-high-slice-header`，并勾选阶段 3 第 2 项。下一步进入 SEI 载荷长度编码 DSL 探测与 ADR 设计（阶段 3 第 3 项 & T4）。
- 2026-08-14：完成参数集重定义负例覆盖与 ADR-0078 表述精度修复（任务 R1）。
  1. 在 `tests/rules/h264_annex_b_analyzer_test.cpp` 中新增真正的同 ID SPS 失败重定义回归用例 `failedSpsRedefinitionPreservesPriorGenerationForSubsequentSlices`：验证 SPS 0（`log2_max_frame_num_minus4=0`）经 PPS 0 激活 Slice A（4-bit `frame_num`），随后的 malformed SPS 0（保留 profile 99，试图将 `log2` 改为 2）进入 `Invalid` 且不发布新代、不污染 generation 0，重发的 PPS 0 与随后的 Slice B 仍绑定至 generation 0 并以 4-bit `frame_num` 正常物化且零诊断，AUD 正常；
  2. 将原全新非法 ID 1 依赖缺失测试重命名为如实的 `invalidParameterSetDefinitionDoesNotPublishOrFallBack`；
  3. 修订双语 ADR-0078（`docs/adr/0078-*.md` 与 `docs/zh-CN/adr/0078-*.md`）决策第 3 条与 Consequences，如实区分同 ID 失败重定义与全新非法 ID 依赖缺失两条负向流，并引用既有 PPS 扩展门控用例 `failedSpsRedefinitionDoesNotHideHighProfileForPpsExtension`。
  H.264 analyzer 测试套件增至 128/128；`svtool rule check` 通过；本机 dev/ci/sanitize 均为 32/32 且无 sanitizer 报告。hosted run `31797084545` 在 macOS 15 / Qt 6.11.1（job `94756424229`）、Windows 2022 / Qt 6.10.1（job `94756424256`）、Ubuntu 24.04 / Qt 6.11.1（job `94756424261`）全部成功。
- 2026-08-14：完成 coverage depth 清单与文档对齐及版本升级（任务 R2）。
  在 `src/rules/official/org.streamview.h264/rule.toml` 中将 `depth` 正式升级为
  `"baseline-main-high-slice-header"`，并将包版本升级为 `0.1.30`；在双语格式语言参考中
  同步更新 package `0.1.30` 版本声明。`svtool rule check` 通过；parser 70/70、IR 76/76、
  executor 127/127、H.264 analyzer 128/128 通过；本机 dev/ci/sanitize 均为 32/32 且无 sanitizer
  报告。hosted run `31797754251` 在 Ubuntu 24.04 / Qt 6.11.1（job `94758488325`）、
  macOS 15 / Qt 6.11.1（job `94758488439`）、Windows 2022 / Qt 6.10.1（job `94758488442`）全部成功。
- 2026-08-14：完成 SEI 载荷与循环 DSL 能力探测与双语 ADR 规范设计（任务 T4 / 阶段 3 第 3 项）。
  1. 实测探测证据：
     - 探测 1（`payloadType`/`payloadSize` 0xFF 累加）：尝试 `until (ff_byte != 255)` 触发 `Expected '==' after sentinel field name`；尝试 `computed<u64> total = sum(ff_byte)` 触发 `Computed field dependency must be declared earlier` 与 `Pure function is not declared before this call`。确认现有语法不支持非等值哨兵或跨投影数组规约。
     - 探测 2（`sei_rbsp` 外层循环）：尝试 `until (more_rbsp_data() == false)` 或 `until (computed_done == true)` 触发 `Sentinel field must be declared directly in the repeat body`。确认现有哨兵仅支持结构体内 source-backed 标量字段。
  2. 制定并提交双语 ADR-0079 与 ADR-0080：
     - ADR-0079（`docs/adr/0079-*.md` / `docs/zh-CN/adr/0079-*.md`）：引入 `ff_coded<max_bytes>` 变长字节累加标量编码（产出单个 `u64` AST 标量节点，mapped source span，`max_bytes` 上限越界报错）；
     - ADR-0080（`docs/adr/0080-*.md` / `docs/zh-CN/adr/0080-*.md`）：引入 `repeat (max_iterations) while (more_rbsp_data()) { ... }` 由码流剩余数据状态驱动的有界重复循环；
     - 在双语格式语言参考中同步更新对应语法说明。
  Next Action 指向 T5a：实现 `ff_coded<max_bytes>` 语言能力（DSL parser/IR/VM 及定向测试）。
- 2026-08-14：完成 ff_coded<max_bytes> 变长字节累加标量编码语言能力实现（任务 T5a / ADR-0079）。
  1. DSL 语法与语义（src/rules/dsl.h / dsl.cpp）：
     - 新增 DslFieldEncoding::FfCoded 与 quint64 maxBytes 字段声明；
     - 实现 ff_coded<max_bytes> 词法与语法解析，严格校验 1 <= max_bytes <= 64（越界报错 InvalidBitWidth）；
     - 允许 ff_coded 作为控制字段参与条件、循环界限控制，并支持 @range 与 @equals 约束声明。
  2. 静态类型 IR（src/rules/dsl_ir.h / dsl_ir.cpp）：
     - 新增 DslValueTypeKind::FfCoded 与 DslOpcode::ReadFfCoded；
     - 将 FfCoded AST 节点 lower 为类型化字段（metadata.typeName = "ff_coded<N>"），保留 context export 资格，并生成 ReadFfCoded 字节码指令。
  3. VM 运行时（src/rules/dsl_vm.cpp）：
     - 实现 readFfCoded(core::BitReader&, quint64 maxBytes) 执行引擎：按 8-bit 读取字节，若为 0xFF 则累加 255 并继续循环，直至遇到 < 0xFF 的终止字节；
     - 若累计字节数达到 maxBytes 仍未遇到终止字节，报错 InvalidSyntax（"ff_coded field exceeded maximum byte limit"）；
     - 截断保护与事务回滚：若码流在非完整字节处截断，回滚 reader 位置并返回 TruncatedSource；
     - 字段物化为 SyntaxField、u64 标量值，source span 覆盖所有消耗字节。
  4. 自动化测试套件：
     - tests/rules/dsl_test.cpp：覆盖合法解析（ff_coded<8>、ff_coded<64>）、非法界限（ff_coded<0>、ff_coded<65> 报 InvalidBitWidth）与残缺语法报错；
     - tests/rules/dsl_ir_test.cpp：覆盖类型推导、ReadFfCoded 指令生成及 @range/@equals 约束；
     - tests/rules/dsl_executor_test.cpp：覆盖单字节直接解码（0x04 -> 4）、多字节 0xFF 累加解码（0xFF 0xFF 0x03 -> 513）、超限拒绝与截断回滚。
  svtool rule check 通过；本机 dev/ci/sanitize 均为 32/32 且无 sanitizer 报告。hosted run 31802120848 在 macOS 15 / Qt 6.11.1（job 94772297778）、Windows 2022 / Qt 6.10.1（job 94772297785）、Ubuntu 24.04 / Qt 6.11.1（job 94772297818）全部成功。
- 2026-08-14：完成 repeat (maximum) while (more_rbsp_data()) 有界循环语言能力实现（任务 T5b / ADR-0080）。
  1. DSL 语法与语义（src/rules/dsl.h / dsl.cpp）：
     - 在 DslStructItemKind 中新增 WhileRepeat，定义 maximumWhileRepeatIterations() = 1024；
     - 扩展 parseRepeat 解析 repeat (maximum) while (more_rbsp_data()) { ... }；严格校验 1 <= maximum <= 1024（越界报 InvalidArrayLength）与循环谓词只能为 more_rbsp_data()（非法报 InvalidCondition）；
     - 校验 repeat body 非空且包含至少一个字段。
  2. 静态类型 IR（src/rules/dsl_ir.h / dsl_ir.cpp）：
     - 新增 DslTypedWhileRepeat 与 DslOpcode::AssertWhileRepeatTerminated；
     - 将 WhileRepeat 展开为 maximum 次迭代字段投影，每个投影附加 MoreRbspData == true 条件约束，在末尾发射 AssertWhileRepeatTerminated 字节码指令。
  3. VM 运行时（src/rules/dsl_vm.cpp）：
     - 在结构体预检中校验 whileRepeats 的有序性、投影 guard 一致性；
     - conditionsPresent 扩展支持 Bool 类型的表达式求值（MoreRbspData）；
     - 在 AssertWhileRepeatTerminated 执行时探测 reader.moreRbspData()：若 maximum 次迭代执行完毕后仍有 RBSP 数据，报错 InvalidSyntax（"While repeat did not terminate within its declared maximum"）；若无剩余数据则正常退出循环。
  4. 自动化测试套件：
     - tests/rules/dsl_test.cpp：覆盖合法语法（repeat (64) while (more_rbsp_data())）、非法界限（0, 1025）、非法条件函数与空循环体报错；
     - tests/rules/dsl_ir_test.cpp：覆盖 DslTypedWhileRepeat 生成、MoreRbspData 迭代条件以及 AssertWhileRepeatTerminated 指令生成；
     - tests/rules/dsl_executor_test.cpp：覆盖 0 次迭代退出（直接遇到 rbsp_trailing_bits）、多迭代正常解码以及超过最大迭代次数时的 InvalidSyntax 报错。
  svtool rule check 通过；本机 dev/ci/sanitize 均为 32/32 且无 sanitizer 报告。hosted run 31821440613 在 macOS 15 / Qt 6.11.1（job 94835366232）、Windows 2022 / Qt 6.10.1（job 94835366114）、Ubuntu 24.04 / Qt 6.11.1（job 94835366166）全部成功。
- 2026-08-15：完成 SEI 容器结构与派发实现（任务 T6 / 阶段 3 第 3 项 / ADR-0080）。
  1. 官方规则包（`src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt`）：
     - 新增 `SeiRbsp` 结构体，包含 `repeat (64) while (more_rbsp_data())` 循环，每个 message 解析 `ff_coded<8> payload_type`、`ff_coded<64> payload_size`、`@lazy(payload_size) bytes payload_data`，以 `rbsp_trailing_bits;` 结尾；
     - 在 `payload<rbsp> nal_units` 派发表中加入 `case 6: SeiRbsp;`；
     - `rule.toml` 包版本升级至 `0.1.31`。
  2. VM 与编译器增强：
     - `src/rules/dsl_vm.cpp`：在 `executeStructure` 中增加 `WhileRepeatRuntimeState`，动态在每次迭代起始点探测 `more_rbsp_data()` 并跨该迭代内的所有字段保持 active 状态；
     - `src/rules/dsl_ir.cpp` 与 `dsl.cpp`：完善 `OffsetTracker` 对 while repeat 和 lazy byte region 的字节对齐检查与传播。
  3. 自动化测试套件（`tests/rules/h264_annex_b_analyzer_test.cpp`）：
     - 新增 5 个 SEI 针对性测试用例：单 SEI 消息、同一 NAL 内多 SEI 消息、跨字节 `ff_coded` payload type 与 size（256/257）、0 长度 SEI payload、截断 SEI 码流安全回滚；
     - 验证所有节点均具有完整有序子节点列表与准确 source spans。
  4. 测试与验证：
     - `svtool rule check` 通过；
     - H.264 analyzer 套件为 131 测试用例（132/132 包含 fixture setup）；DSL parser 77/77、DSL IR 77/77、executor 133/133 全部通过；
     - 本机 dev/ci/sanitize 均为 32/32 且无 sanitizer 报告；
     - hosted run `31824494864` 在 Ubuntu 24.04 / Qt 6.11.1（job `94845285948`）、macOS 15 / Qt 6.11.1（job `94845285989`）、Windows 2022 / Qt 6.10.1（job `94845286130`）全部成功。
  勾选阶段 3 第 3 项（所有 SEI 解析 payloadType/payloadSize）。Next Action 指向 Task T7。
- 2026-08-15：完成恢复点 SEI 消息（Recovery Point SEI, payload_type == 6）解码与条件 RBSP 对齐支持（任务 T7 / ADR-0081 / 包版本 0.1.32）。
  1. DSL 语法、编译器与 VM 增强：
     - `src/rules/dsl.cpp`：`validateTerminals` 支持 `rbsp_trailing_bits;` 作为 `if` 分支或 `switch` 分支内的末尾项，同时严格禁止直接出现在 repeat 循环体顶层；
     - `src/rules/dsl_ir.cpp`：`RbspTrailingBits` 编译支持携带分支条件约束（`conditions`）与迭代索引（`repeatIndices`），生成 `rbsp_stop_one_bit[repeatIndex]` 和 `rbsp_alignment_zero_bit[zeroIndex][repeatIndex]` 命名；
     - `src/rules/dsl_vm.cpp`：`ReadRbspTrailingBits` 操作码支持条件执行求值，当条件未命中时安全跳过保留字段并递进字段索引；顶层无条件 `rbsp_trailing_bits` 仍强制要求为结构体终结指令。
  2. 官方规则包（`src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt`）：
     - 在 `SeiRbsp` 的 `while (more_rbsp_data())` 循环中引入 `if (payload_type == 6)` 分支，解码 `ue recovery_frame_cnt`、`bits<1> exact_match_flag`、`bits<1> broken_link_flag`、`bits<2> changing_slice_group_idc @range(0, 2)` 以及条件 `rbsp_trailing_bits;` 对齐字节边界；其他 payload 类型保持为 `@lazy(payload_size) bytes payload_data`；
     - `rule.toml` 包版本升级至 `0.1.32`。
  3. 自动化测试套件：
     - `tests/rules/dsl_test.cpp`：覆盖条件分支内 `rbsp_trailing_bits` 的合法解析及非末尾位置报错；
     - `tests/rules/dsl_ir_test.cpp`：覆盖条件分支内 trailing bits 的条件与操作码编译；
     - `tests/rules/dsl_executor_test.cpp`：覆盖条件命中与跳过两种执行路径及字段物化；补充多字段 while-repeat 循环体内在后续字段处抵达 trailing bits 时的每迭代统一执行回归测试；
     - `tests/rules/h264_annex_b_analyzer_test.cpp`：新增 5 个针对性测试用例（单恢复点 SEI 完整有序子节点列表断言、多 bit Exp-Golomb recovery_frame_cnt、changing_slice_group_idc 越界非致命告警、recovery_point + user_data 混合多 SEI NAL、截断 recovery_point 码流安全回滚与后续 NAL 正常解析）。
  4. 测试与验证：
     - `svtool rule check` 通过；
     - H.264 analyzer 套件增至 136 测试用例（137/137 包含 fixture setup）；DSL parser 77/77、DSL IR 77/77、executor 135/135 全部通过；
     - 本机 dev/ci/sanitize 均为 32/32 且无 sanitizer 报告；
     - hosted run `31859117579` 在 Ubuntu 24.04 / Qt 6.11.1（job `94949154063`）、macOS 15 / Qt 6.11.1（job `94949153997`）、Windows 2022 / Qt 6.10.1（job `94949154008`）全部成功。
  Next Action 指向 Task T8（user_data_unregistered SEI payload parsing）。
- 2026-08-15：完成未注册用户数据 SEI 消息（User Data Unregistered SEI, payload_type == 5）结构化解码与基于 switch 的 SEI 载荷派发重构（任务 T8 / ADR-0082 / 包版本 0.1.33）。
  1. 官方规则包（`src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt`）：
     - 将 `SeiRbsp` 的载荷派发由 `if/else` 链重构为可扩展的 `switch (payload_type)` 结构；
     - 新增 `case 5` 分支解码未注册用户数据：16 字节 UUID 数组 `bits<8> uuid_iso_iec_11578[16]`，以及动态长度延迟载荷字节 `@lazy(payload_size - 16) bytes user_data_payload_byte`；
     - 当 `payload_size < 16` 时触发算术减法下溢保护生成语法错误并安全回滚事务；
     - `rule.toml` 包版本升级至 `0.1.33`。
  2. 自动化测试套件（`tests/rules/h264_annex_b_analyzer_test.cpp`）：
     - 新增 5 个针对性测试用例：单消息未注册用户数据 16 字节 UUID + 4 字节载荷解码及完整有序子节点列表断言、`payload_size == 16` 时 0 长度载荷区域边界物化、同一 NAL 内 `user_data_unregistered` + `recovery_point` 混合多 SEI 消息按顺序完整解析、`payload_size < 16` 下溢保护报错与安全回滚、截断 payload 码流安全回滚并继续后续 NAL；
     - 适配既有单/多 SEI 消息测试使用 `payload_type` 7/8 验证 default 分支的不透明 lazy payload 保留。
  3. 测试与验证：
     - `svtool rule check` 通过；
     - H.264 analyzer 套件增至 141 测试方法（143/143 包含 fixture setup）；DSL parser 77/77、DSL IR 77/77、executor 135/135 全部通过；
     - 本机 dev/ci/sanitize 均为 32/32 且无 sanitizer 报告；
     - hosted run `31861503655` 在 Ubuntu 24.04 / Qt 6.11.1（job `94955623663`）、macOS 15 / Qt 6.11.1（job `94955623587`）、Windows 2022 / Qt 6.10.1（job `94955623627`）全部成功。
  Next Action 指向 Task T9（user_data_registered_itu_t_t35 SEI payload parsing）。
- 2026-08-15：完成 ITU-T T.35 建议书注册用户数据 SEI 消息（User Data Registered by Recommendation ITU-T T.35 SEI, payload_type == 4）结构化解码（任务 T9 / ADR-0083 / 包版本 0.1.34）。
  1. 官方规则包（`src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt`）：
     - 在 `SeiRbsp` 的 `switch (payload_type)` 中新增 `case 4` 分支解码 ITU-T T.35 注册用户数据；
     - 解析 8 比特国家代码 `bits<8> itu_t_t35_country_code`；
     - 当 `itu_t_t35_country_code == 255` 时，解析扩展国家代码 `bits<8> itu_t_t35_country_code_extension_byte` 与动态延迟载荷字节 `@lazy(payload_size - 2) bytes itu_t_t35_extension_payload_byte`；
     - 当国家代码不为 255 时，解析动态延迟载荷字节 `@lazy(payload_size - 1) bytes itu_t_t35_payload_byte`；
     - 当 payload_size 不足（标准分支 < 1 或扩展分支 < 2）时触发算术减法下溢保护生成语法错误并安全回滚事务；
     - `rule.toml` 包版本升级至 `0.1.34`。
  2. 自动化测试套件（`tests/rules/h264_annex_b_analyzer_test.cpp`）：
     - 新增 6 个针对性测试用例：
       - `decodesUserDataRegisteredItuTT35SeiMessageWithoutExtensionByte`（标准国家代码 0xb5 + 6 字节载荷解析，断言完整有序子节点列表、逐字段值与 source span）；
       - `decodesUserDataRegisteredItuTT35SeiMessageWithExtensionByte`（扩展国家代码 255 + 扩展字节 0x01 + 4 字节载荷解析，断言完整有序子节点列表与逐字段值）；
       - `decodesUserDataRegisteredItuTT35SeiMessageWithZeroLengthPayload`（标准 payload_size == 1 与扩展 payload_size == 2 时 0 长度载荷区域边界物化）；
       - `decodesMultipleSeiMessagesContainingUserDataRegisteredAndRecoveryPoint`（同一 NAL 内 T.35 + recovery_point 混合多 SEI 消息按顺序完整解析与有序子节点列表断言）；
       - `reportsInvalidSyntaxForUserDataRegisteredWithPayloadSizeUnderflow`（payload_size == 0 与 payload_size == 1 配合 country_code == 255 减法下溢安全回滚与报错）；
       - `reportsTruncatedUserDataRegisteredSeiPayloadAndContinues`（截断 T.35 载荷码流安全回滚并继续后续 NAL）。
  3. 测试与验证：
     - `svtool rule check` 通过；
     - H.264 analyzer 套件增至 147 测试方法（149/149 包含 fixture setup）；DSL parser 77/77、DSL IR 77/77、executor 135/135 全部通过；
     - 本机 dev/ci/sanitize 均为 32/32 且无 sanitizer 报告；
     - hosted run `31862532299` 在 Ubuntu 24.04 / Qt 6.11.1（job `94958276145`）、macOS 15 / Qt 6.11.1（job `94958276179`）、Windows 2022 / Qt 6.10.1（job `94958276153`）全部成功。
  Next Action 指向 Task T10（buffering_period SEI payload parsing）。
- 2026-08-15：完成块内局部上下文导入键语言能力规范与引擎实现（任务 T10a/T10b / ADR-0084 / 纯能力切片，包版本保持 0.1.34）。
  1. 规范与文档（`5d416bd`）：
     - 归档双语 ADR-0084（`docs/adr/0084-locally-scoped-context-import-keys.md` + `docs/zh-CN/adr/0084-locally-scoped-context-import-keys.md`），明确放宽 `@context_import` 键的位置约束至块内有支配保证的局部声明；
     - 更新中英文格式语言参考（`docs/format-language/README.md`、`docs/zh-CN/format-language/README.md`）。
  2. 引擎能力实现（`c765eda`）：
     - `src/rules/dsl_ir.cpp`：`resolveContextValue` 支持在嵌套条件与 repeat 展开块内向上查找最近支配的无符号标量键字段，执行 branch-guarantee 分支保证验证，并按用法动态向 `typedStruct.contextImports` 登记导入槽位；
     - `src/rules/include/streamview/rules/dsl_ir.h`：`DslTypedContextImport::maximumImports()` 扩至 1024 以容纳展开导入槽位，新增 `maximumDeclaredImports() == 16`；`DslTypedField` 新增 `importEligible` 标识以精准区隔标量与数组元素；
     - `src/rules/dsl_vm.cpp`：新增 `validImportContextField`，在表达式校验中加入导入键分支支配验证，并在 `Opcode::End` 中优雅处理未物化的条件导入键；
     - `src/rules/rule_execution_session.cpp`：在后处理物化循环中安全跳过未物化的条件导入。
  3. 自动化测试套件：
     - `tests/rules/dsl_ir_test.cpp`：新增 `lowersLocallyScopedContextImportKeysToTypedIr`，覆盖 repeat + switch 局部导入键编译、异分支非支配键拒绝、`se` 键拒绝以及后置声明键拒绝；IR 测试方法增至 78；
     - `tests/rules/rule_execution_session_test.cpp`：新增 `executesLocallyScopedContextImportKeysInsideRepeatAndSwitch`（验证 repeat + switch 块内动态位宽 `bits<(context_value(...) + 1)>`、多迭代逐次重绑定及完整有序子节点列表）与 `handlesLocallyScopedContextImportFailureGracefully`（未知 SPS 键安全回退 DependencyUnavailable）；测试方法增至 38。
  4. 测试与验证：
     - `svtool rule check` 通过；
     - 套件测试：IR 78/78、RuleExecutionSession 38/38、DSL executor 135/135、H.264 analyzer 147/147（149/149 包含 fixture）；
     - 本机 dev/ci/sanitize 均为 32/32 且无 sanitizer 报告；
     - hosted run `31864791158` 在 Ubuntu 24.04 / Qt 6.11.1（job `94964008874`）、macOS 15 / Qt 6.11.1（job `94964008882`）、Windows 2022 / Qt 6.10.1（job `94964008931`）全部成功。
  Next Action 指向 Task T10c（buffering_period SEI message decoding rule consumption slice, package 0.1.35）。
- 2026-08-15：完成缓冲周期 SEI 消息（Buffering Period SEI, payload_type == 0）结构化解码与 SPS HRD 上下文导出（任务 T10c / ADR-0085 / 包版本 0.1.35）。
  1. 官方规则包（`src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt`）：
     - 在 `SequenceParameterSetRbsp` 中导出有效 HRD 参数：`effective_nal_hrd_parameters_present_flag`、`effective_nal_hrd_cpb_count`、`effective_nal_hrd_initial_cpb_removal_delay_length_minus1` 以及对应的 VCL HRD 导出项，通过 `optional_value` 提供健壮默认值；
     - 在 `SeiRbsp` 的 `switch (payload_type)` 中实现 `case 0` 分支，利用 ADR-0084 块内局部导入键机制声明 `ue seq_parameter_set_id @range(0, 31)` 并动态导入 `h264-sps` 上下文；
     - 条件解码 NAL HRD 初始到达延迟循环与 VCL HRD 初始到达延迟循环，使用动态位宽 `bits<nal_delay_length>` / `bits<vcl_delay_length>` 读取 `initial_cpb_removal_delay` 和 `initial_cpb_removal_delay_offset`；
     - 末尾使用条件 `rbsp_trailing_bits;` 对齐字节边界；
     - `rule.toml` 包版本升级至 `0.1.35`。
  2. 自动化测试套件（`tests/rules/h264_annex_b_analyzer_test.cpp`）：
     - 新增 7 个针对性测试用例：
       - `decodesBufferingPeriodSeiMessageWithNalHrdParameters`（SPS 包含 NAL HRD 2 CPBs / 24-bit 延迟，断言完整有序子节点列表 26 项、逐字段值与 source span）；
       - `decodesBufferingPeriodSeiMessageWithVclHrdParameters`（SPS 包含 VCL HRD 1 CPB / 11-bit 延迟，断言完整有序子节点列表 18 项与逐字段值）；
       - `decodesBufferingPeriodSeiMessageWithBothNalAndVclHrdParameters`（SPS 同时包含 NAL 与 VCL HRD 参数，断言完整有序子节点列表 30 项与逐字段值）；
       - `decodesBufferingPeriodSeiMessageWithNoHrdParametersInSps`（Baseline SPS 无 HRD，仅解析 seq_parameter_set_id 与对齐位，断言完整有序子节点列表 20 项）；
       - `decodesMultipleSeiMessagesContainingBufferingPeriodAndRecoveryPoint`（同一 NAL 内 buffering_period + recovery_point 混合多 SEI 消息按顺序完整解析与有序子节点列表断言 35 项）；
       - `handlesBufferingPeriodWithUnavailableSpsGracefullyAndContinues`（缺失 SPS 依赖时安全降级为 Invalid，不崩溃且保持隔离）；
       - `reportsTruncatedBufferingPeriodSeiPayloadAndContinues`（截断 buffering_period 载荷码流安全回滚并继续后续 AUD NAL）。
  3. 测试与验证：
     - `svtool rule check` 通过；
     - H.264 analyzer 套件增至 154 测试方法（156/156 包含 fixture setup）；DSL parser 77/77、DSL IR 78/78、RuleExecutionSession 38/38、executor 135/135 全部通过；
     - 本机 dev/ci/sanitize 均为 32/32 且无 sanitizer 报告；
     - hosted run `31871279202` 在 Ubuntu 24.04 / Qt 6.11.1（job `94980189462`）、macOS 15 / Qt 6.11.1（job `94980189372`）、Windows 2022 / Qt 6.10.1（job `94980189380`）全部成功。
  Next Action 指向 Task T11（pic_timing SEI message decoding probe & specification）。
- 2026-08-15：完成环境上下文导入能力规范与图像定时 SEI 活跃参数集解析设计（任务 T11a / ADR-0086 / commit `7aa9aa9` / 纯规范切片）。
  1. 双语 ADR-0086 归档（`docs/adr/0086-ambient-context-imports.md` + `docs/zh-CN/adr/0086-ambient-context-imports.md`）：
     - 归档 4 项探测证据与已排除死路：
       - 闸门 1：注解参数数量检查（`src/rules/dsl_ir.cpp:1125`）；
       - 闸门 2：解析器表达式三元 arity 检查（`src/rules/dsl.cpp:1892`）与 IR 降低 3 操作数检查（`src/rules/dsl_ir.cpp:1508`）；
       - 死路 A：块内常量键（`assumed_sps_id = 0`，多 SPS 场景静默错解）；
       - 死路 B：跨分支 `optional_value(seq_parameter_set_id, 0)`（槽位未物化恒走回退值 0）；
     - 固化基于位置的 $O(K \cdot \log M)$ 环境上下文检索语义（与 ADR-0078 同构，无键等值过滤）；
     - 固化同结构体有键与环境导入共存规则（三参绑定有键、二参绑定环境、同 kind 重复环境报错）；
     - 固化单消息级失败隔离（`NotFound` / `DependencyUnavailable` 映射为 `Invalid` / `WaitingDependency`，按 `payload_size` 续扫）；
     - 明确 D.2.2 有界线性流扫描近似的已知局限性，AU 状态机与块内重发布显式 out of scope。
  2. Markdown-only 提交按 ADR-0019 跳过 hosted CI。
  Next Action 指向 Task T12a（frame_packing_arrangement SEI message decoding, package 0.1.36）。
- 2026-08-15：完成帧封装排列 SEI 消息（Frame Packing Arrangement SEI, payload_type == 45）结构化解码（任务 T12a / ADR-0087 / 包版本 0.1.36 / commit `594fa83`）。
  1. 官方规则包（`src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt`）：
     - 在 `SeiRbsp` 的 `switch (payload_type)` 中实现 `case 45` 分支，按 ITU-T H.264 D.1.25 / D.2.25 解码完整字段；
     - 解码 `ue frame_packing_arrangement_id` 与 `u1 frame_packing_arrangement_cancel_flag`；
     - 在 `cancel_flag == 0` 分支内解码立体排布参数：`u7 frame_packing_arrangement_type`、`u1 quincunx_sampling_flag`、`u6 content_interpretation_type`、`u1 spatial_flipping_flag`、`u1 frame0_flipped_flag`、`u1 field_views_flag`、`u1 current_frame_is_frame0_flag`、`u1 frame0_self_contained_flag`、`u1 frame1_self_contained_flag`；
     - 使用计算字段 `computed<bool> has_grid_position = quincunx_sampling_flag == 0 && frame_packing_arrangement_type != 5;` 严格遵循规范条件解码 `u4 frame0_grid_position_x`、`u4 frame0_grid_position_y`、`u4 frame1_grid_position_x`、`u4 frame1_grid_position_y`；
     - 解码 `u8 frame_packing_arrangement_reserved_byte @range(0, 0)` 与 `ue frame_packing_arrangement_repetition_period @range(0, 16384)`；
     - 解码 `u1 frame_packing_arrangement_extension_flag @range(0, 0)` 及末尾条件 `rbsp_trailing_bits;`；
     - `rule.toml` 包版本升级至 `0.1.36`。
  2. 自动化测试套件（`tests/rules/h264_annex_b_analyzer_test.cpp`）：
     - 新增 6 个针对性测试用例：
       - `decodesFramePackingArrangementSeiMessageWithGridPositions`（Side-by-Side arrangement_type == 3 + quincunx == 0 带 grid_position 坐标，断言完整有序子节点列表 33 项、逐字段值与 source span）；
       - `decodesFramePackingArrangementSeiMessageWithQuincunxSamplingWithoutGridPositions`（Top-and-Bottom arrangement_type == 4 + quincunx == 1 无 grid_position 坐标，断言完整有序子节点列表 33 项与逐字段值）；
       - `decodesFramePackingArrangementSeiMessageWithFrameSequentialWithoutGridPositions`（Frame Sequential arrangement_type == 5 + quincunx == 0 遵循 != 5 条件无 grid_position 坐标，断言逐字段值）；
       - `decodesFramePackingArrangementSeiMessageWithCancelFlagTrue`（cancel_flag == 1 取消排布仅解码 id/cancel/extension 字段，断言完整有序子节点列表 16 项与逐字段值）；
       - `decodesMultipleSeiMessagesContainingFramePackingAndRecoveryPoint`（同一 NAL 内 frame_packing + recovery_point 混合多 SEI 消息按顺序完整解析与有序子节点列表断言 33 项）；
       - `reportsTruncatedFramePackingArrangementSeiPayloadAndContinues`（截断 frame_packing 载荷码流安全回滚并继续后续 AUD NAL）。
  3. 测试与验证：
     - `svtool rule check` 通过；
     - H.264 analyzer 套件增至 160 测试方法（162/162 包含 fixture setup）；
     - 本机 dev/ci/sanitize 均为 32/32 且无 sanitizer 报告；
     - hosted run `31873738599` 在 Ubuntu 24.04 / Qt 6.11.1（job `94986199649`）、macOS 15 / Qt 6.11.1（job `94986199632`）、Windows 2022 / Qt 6.10.1（job `94986199663`）全部成功。
  Next Action 指向 Task T12b（display_orientation SEI message decoding specification & implementation, package 0.1.37）。
- 2026-08-15：完成显示方向 SEI 消息（Display Orientation SEI, payload_type == 47）结构化解码（任务 T12b / ADR-0088 / 包版本 0.1.37 / commit `ab85444`）。
  1. 官方规则包（`src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt`）：
     - 在 `SeiRbsp` 的 `switch (payload_type)` 中实现 `case 47` 分支，按 ITU-T H.264 D.1.27 / D.2.27 解码完整字段；
     - 解码 `u1 display_orientation_cancel_flag`；
     - 在 `cancel_flag == 0` 分支内解码显示变换参数：`u1 hor_flip`、`u1 ver_flip`、`u16 anticlockwise_rotation`、`ue display_orientation_repetition_period @range(0, 16384)` 与 `u1 display_orientation_extension_flag @range(0, 0)`；
     - 末尾使用条件 `rbsp_trailing_bits;` 对齐字节边界；
     - `rule.toml` 包版本升级至 `0.1.37`。
  2. 自动化测试套件（`tests/rules/h264_annex_b_analyzer_test.cpp`）：
     - 新增 4 个针对性测试用例：
       - `decodesDisplayOrientationSeiMessageWithRotationAndFlips`（cancel_flag == 0 + hor_flip == 1 + 90度逆时针旋转 16384，断言完整有序子节点列表 17 项、逐字段值与 source span 51..67 / 16-bit）；
       - `decodesDisplayOrientationSeiMessageWithCancelFlagTrue`（cancel_flag == 1 取消显示方向仅解码 cancel 标志与对齐位，断言完整有序子节点列表 17 项与逐字段值）；
       - `decodesMultipleSeiMessagesContainingDisplayOrientationAndRecoveryPoint`（同一 NAL 内 display_orientation + recovery_point 混合多 SEI 消息按顺序完整解析与有序子节点列表断言 31 项）；
       - `reportsTruncatedDisplayOrientationSeiPayloadAndContinues`（截断 display_orientation 载荷码流安全回滚并继续后续 AUD NAL）。
  3. 测试与验证：
     - `svtool rule check` 通过；
     - H.264 analyzer 套件增至 164 测试方法（166/166 包含 fixture setup）；
     - 本机 dev/ci/sanitize 均为 32/32 且无 sanitizer 报告；
     - hosted run `31874725106` 在 Ubuntu 24.04 / Qt 6.11.1（job `94988637131`）、macOS 15 / Qt 6.11.1（job `94988637064`）、Windows 2022 / Qt 6.10.1（job `94988636999`）全部成功。
  Next Action 指向 Task T11b（ambient context import capability slice in core and DSL runtime, ADR-0086）。
- 2026-08-15：完成环境上下文导入与最近参数集解析能力切片（任务 T11b / ADR-0086 / 包版本保持 0.1.37 / commit `cae9c73`）。
  1. 核心模型（`src/core/include/streamview/core/context_directory.h` 与 `src/core/context_directory.cpp`）：
     - 实现 `ContextLookupResult resolveLatestBefore(ContextDefinitionKind kind, quint64 scopeId, SourceBitAddress sourcePosition) const`；
     - 基于 `definitionsByKey_` 有序字典二分确定 `(kind, scopeId)` 连续键区间，在各键历史 generation 向量中以二分查找 `sourcePosition` 前已结束的最大有效定义；
     - 递归检验所选定义的依赖闭包完整性与代际有效性，依赖失效返回 `DependencyUnavailable`；
     - 新增 5 个目录层单测（空目录 NotFound、跨多 key 按位置选最近、同 key 重定义后取最近 generation、依赖不可用返回 DependencyUnavailable、相邻 kind/scope 隔离不串扰）。
  2. DSL 编译器与虚拟机制（`src/rules/`）：
     - 解析器（`dsl.cpp`）：放宽 `context_value` 语法至 2 参（环境导入）或 3 参（有键导入）标识符；
     - 类型 IR 降级（`dsl_ir.cpp`）：放宽 `@context_import("kind")` 为单参；2 参 `context_value(kind, field)` 降低为 `ImportedContextReference`（`keyFieldIndex = std::nullopt`）；同结构体内同一 kind 重复环境导入报错；同结构体有键与环境导入共存消歧，并严格保留有键导入的 `dsl_ir.cpp:1553` 分支支配保证；
     - 虚拟机与执行会话（`dsl_vm.cpp`、`rule_execution_session.cpp`）：支持无键 `context_value` 运行时求值；执行器后置处理正确接入 `resolveLatestBefore` 并在 `NotFound` / `DependencyUnavailable` 时向结构节点挂载诊断并保持单消息级隔离。
  3. 三层测试矩阵：
     - 目录层（`tests/core/context_directory_test.cpp`）：5 个针对性测试全部通过；
     - IR 层（`tests/rules/dsl_ir_test.cpp`）：单参注解/双参表达式降低、有键/环境共存消歧、重复环境导入拒绝、无导入双参表达式拒绝及 malformed typed IR 预检拒绝；
     - 执行层（`tests/rules/rule_execution_session_test.cpp`）：环境上下文解析成功取值、同结构体有键+环境共存运行时正确性、NotFound/DependencyUnavailable 隔离续扫；
     - DSL 与规则引擎全量回归：`svtool rule check` Rule OK，H.264 analyzer 166/166 零回归，本地 dev/ci/sanitize 均为 32/32；
     - hosted run `31876726681` 在 Ubuntu 24.04 / Qt 6.11.1（job `94993463832`）、macOS 15 / Qt 6.11.1（job `94993463779`）、Windows 2022 / Qt 6.10.1（job `94993463806`）全部成功。
  Next Action 指向 Task T11c（Picture Timing SEI message rule consumption, payload_type == 1, package 0.1.38）。
- 2026-08-15：完成 `byte_aligned()` 字节对齐谓词表达式能力切片（任务 T11c-1 / ADR-0089 / 包版本保持 0.1.37 / commit `21da8c2`）。
  1. 规范与语法文档（`docs/format-language/README.md`、`docs/zh-CN/format-language/README.md` 与 ADR-0089）：
     - 明确 `byte_aligned()` 为零参保留表达式，类型为 `bool`，在不推进 reader 的情况下评估当前逻辑坐标是否精确为 8 的倍数；
     - 明确坐标系为 reader 逻辑 bit 坐标 `(reader.position() % 8 == 0)`，纯函数内禁用；
     - 与 `if (!byte_aligned()) { rbsp_trailing_bits; }` 配合用于条件性对齐。
  2. DSL 解析器、类型 IR 与虚拟机运行时（`src/rules/`）：
     - 解析器（`dsl.cpp`）：支持 `byte_aligned` 零参布尔调用表达式，拦截纯函数内调用（`UnknownReference`）与带参数调用（`InvalidExpression`），并将 `byte_aligned` 纳入顶层保留表达式名称查重（`DuplicateName`）；
     - 类型 IR 编译（`dsl_ir.h`、`dsl_ir.cpp`）：新增 `DslTypedExpressionKind::ByteAligned`（类型 `DslScalarType::Bool`），支持静态检查与节点预算计算；
     - 虚拟机与执行器（`dsl_vm.cpp`）：在 `evaluateTypedExpression` 中按 `reader.position() % 8U == 0U` 评估，并在 `validateTypedStruct` 中放宽条件表达式类型校验。
  3. 三层测试矩阵：
     - 解析层（`tests/rules/dsl_test.cpp`）：新增 `parsesByteAlignedCall` 与 `rejectsInvalidByteAlignedCalls`（带参、赋给 u64、纯函数内调用、同名纯函数冲突拦截）；
     - IR 编译层（`tests/rules/dsl_ir_test.cpp`）：新增 `lowersByteAlignedAsAValidatedSourceStateExpression` 校验 typed IR 结构与属性；
     - 虚拟机执行层（`tests/rules/dsl_executor_test.cpp`）：
       - `evaluatesByteAlignedPredicateAcrossAlignedAndUnalignedBitPositions`：断言 24 位对齐载荷（7 项完整有序子节点，`aligned_at_3 == false`, `aligned_at_8 == true`, `aligned_at_24 == true`, `needs_trailing_bits == false` 且跳过 trailing bits）与 19 位非对齐载荷（8 项完整有序子节点，`aligned_at_19 == false`, `needs_trailing_bits == true` 且成功消费 5 位 trailing bits 至 24 位）；
       - `evaluatesByteAlignedInsideRepeatSwitchAndAfterLazyRegions`：断言 `@lazy(2)` 紧随位置、switch 内部带 padding 分支、repeat 循环各迭代内求值正确，并断言 13 项完整有序子节点列表；
     - DSL 与规则引擎全量回归：`svtool rule check` Rule OK，H.264 analyzer 166/166 零回归，本地 dev/ci/sanitize 均为 32/32；
     - hosted run `31879207018` 在 Ubuntu 24.04 / Qt 6.11.1（job `94999305896`）、macOS 15 / Qt 6.11.1（job `94999305954`）、Windows 2022 / Qt 6.10.1（job `94999306008`）全部成功。
  Next Action 指向 Task T11c-2（boolean operands in arithmetic expressions `+` and `*` capability slice, ADR-0090）。
- 2026-08-15：完成加法与乘法算术表达式接受布尔操作数能力切片（任务 T11c-2 / ADR-0090 / 包版本保持 0.1.37 / commit `3281940`）。
  1. 规范与语法文档（`docs/format-language/README.md`、`docs/zh-CN/format-language/README.md` 与 ADR-0090）：
     - 明确 `+`（`Add`）与 `*`（`Multiply`）运算符独立接受 `bool` 或 `u64` 操作数，求值时布尔值自动强转为 `1ULL`（`true`）或 `0ULL`（`false`），表达式结果类型恒为 `u64`；
     - 明确 `-`（`Subtract`）、`/`（`Divide`）与 `%`（`Remainder`）继续严格要求 `u64` 操作数，以防止无符号整数下溢和除零风险；
     - 支持指示函数加权求和语法表达（如 H.264 表格 D-1）。
  2. DSL 解析器、类型 IR 与虚拟机运行时（`src/rules/`）：
     - 解析器（`dsl.cpp`）：在二元表达式类型检查中放宽 `Add` 与 `Multiply` 操作数校验（允许 `Bool` 或 `U64`），若操作数类型非法发出 `Add and multiply operators require u64 or bool operands`，保留 `-`、`/`、`%` 的 `Arithmetic operators require u64 operands`；
     - 类型 IR 降级（`dsl_ir.cpp`）：在 `compileExpression` 中放宽 `Add` 与 `Multiply` 的操作数类型校验，产出类型固定为 `DslScalarType::U64`；
     - 虚拟机与执行器（`dsl_vm.cpp`）：在 `validateTypedExpression` 中放宽 `Add` 与 `Multiply` 操作数类型检查；在 `evaluateTypedExpression` 中对布尔操作数进行数值强转（`true \to 1ULL`、`false \to 0ULL`）。
  3. 三层测试矩阵：
     - 解析层（`tests/rules/dsl_test.cpp`）：新增 `parsesBooleanOperandsInAddAndMultiply`（测试 Table D-1 pure 函数、bool+bool、bool+u64、u64+bool、bool*bool、bool*u64、u64*bool）与 `rejectsInvalidBooleanOperandsInSubtractionDivisionAndRemainder`（7 项针对 `-`、`/`、`%` 使用布尔操作数的精准拒绝测试）；
     - IR 编译层（`tests/rules/dsl_ir_test.cpp`）：新增 `lowersBooleanOperandsInAddAndMultiplyExpressions` 校验内联与独立 computed 表达式 typed IR 的类型与算子；
     - 虚拟机执行层（`tests/rules/dsl_executor_test.cpp`）：新增 `evaluatesBooleanOperandsInAddAndMultiplyAndTableD1Mapping`，遍历测试 Table D-1 全部 16 个 `pic_struct` 值（0..15）的 `NumClockTS` 映射（0..2 为 1、3/4/7 为 2、5/6/8 为 3、9..15 为 0）以及全部布尔组合算术强转值（`true+true==2`, `true+false==1`, `false+false==0`, `true*true==1`, `true*false==0`, `false*false==0`）；
     - DSL 与规则引擎全量回归：`svtool rule check` Rule OK，H.264 analyzer 166/166 零回归，本地 dev/ci/sanitize 均为 32/32；
     - hosted run `31880469742` 在 Ubuntu 24.04 / Qt 6.11.1（job `95002213063`）、macOS 15 / Qt 6.11.1（job `95002213081`）、Windows 2022 / Qt 6.10.1（job `95002213124`）全部成功。
  Next Action 指向 Task T11c-3（Picture Timing SEI message decoding, payload_type == 1, package 0.1.38, ADR-0091）。
- 2026-08-15：完成图像定时 SEI 消息（Picture Timing SEI, payload_type == 1）结构化解码（任务 T11c-3 / ADR-0091 / 包版本 0.1.38 / commit `6fbf585`）。
  1. 规范与双语文档（`docs/adr/0091-h264-picture-timing-sei-message-decoding.md` 与 `docs/zh-CN/adr/0091-h264-picture-timing-sei-message-decoding.md`）：
     - 明确基于 ADR-0086 环境上下文导入从最近有效 SPS 解析 HRD 延迟位宽与图像结构参数；
     - 明确基于 ADR-0090 布尔算术指示求和纯函数实现表格 D-1 `NumClockTS` 映射；
     - 明确基于 ADR-0089 `byte_aligned()` 谓词实现条件性末尾 `rbsp_trailing_bits` 对齐；
     - 归档全部新增字段与标准元素映射消歧表（`full_` / `partial_` 前缀消歧，`time_offset` 二补数有符号解释）。
  2. 规则与执行会话（`src/rules/`）：
     - `h264_annex_b.svfmt`：定义顶层纯函数 `num_clock_ts_for_pic_struct`；在 `SequenceParameterSetRbsp` 中通过嵌套 `optional_value` 导出 `effective_cpb_removal_delay_length_minus1`、`effective_dpb_output_delay_length_minus1`、`effective_time_offset_length` 与 `effective_pic_struct_present_flag`；在 `SeiRbsp` 声明 `@context_import("h264-sps")` 并实现 `case 1:` 结构化分支；
     - `rule_execution_session.cpp`：优化环境上下文导入依赖记录，仅当结构体实例在执行过程中实际访问环境上下文（`importCache.at(importIndex).has_value()`）时才登记导入依赖，避免无上下文依赖的消息（如 user data / display orientation）被误判为依赖缺失。
  3. 全量测试矩阵（`tests/rules/h264_annex_b_analyzer_test.cpp`）：
     - 新增 8 项图像定时测试用例：
       - `decodesPictureTimingSeiMessageWithCpbDpbDelaysAndFullTimestamp`：测试 24 位 CPB/DPB 延迟、`pic_struct = 0`、完整时间戳与 24 位 `time_offset` 解码（断言 38 项完整有序子节点列表与字段值）；
       - `decodesPictureTimingSeiMessageWithPartialTimestamp`：测试部分时间戳各 flag 门禁及 `time_offset` 解码（断言 38 项完整有序子节点列表与字段值）；
       - `decodesPictureTimingSeiMessageWithMultiTimestampPicStruct8`：测试 `pic_struct = 8` 展开 3 组时间戳（断言 46 项完整有序子节点列表）；
       - `decodesPictureTimingSeiMessageWithoutHrdDelaysPicStructOnly`：测试无 HRD 延迟仅有 `pic_struct` 路径（断言 44 项完整有序子节点列表）；
       - `decodesPictureTimingSeiMessageWithByteAlignedPayload`：测试 48 位整字节对齐载荷跳过 payload 内部 trailing bits（断言 16 项完整有序子节点列表）；
       - `decodesPictureTimingSeiMessageWithLatestSpsAcrossMultipleSpsDefinitions`：测试跨多个 SPS 定义时正确绑定最近活跃 SPS（8 位延迟）；
       - `reportsMissingSpsDependencyForPictureTimingSeiMessageAndContinues`：测试缺失前置 SPS 时产生结构级 `WaitingDependency` / `Invalid` 诊断并安全续扫；
       - `reportsTruncatedPictureTimingSeiPayloadAndContinues`：测试截断载荷安全回滚并保留已物化前缀。
     - 规则引擎与解析全量验证：`svtool rule check` Rule OK，H.264 analyzer 174/174（由 166 扩充至 174 且旧规则下测试确为 red），本地 dev/ci/sanitize 均为 32/32（无 sanitizer 警告）；
     - hosted run `31881638934` 在 Ubuntu 24.04 / Qt 6.11.1（job `95004954416`）、macOS 15 / Qt 6.11.1（job `95004954388`）、Windows 2022 / Qt 6.10.1（job `95004954414`）全部成功。
  Next Action 指向 Phase 3 剩余清单核验或下一阶段里程碑推进。
- 2026-08-15：完成 ADR 规范整改与 Session 层环境上下文能力钉死测试（任务 T11c 整改 / commit `fefd90a` 与 commit `9c18436`）。
  1. ADR 双语规范修正（`fefd90a`）：
     - ADR-0089（英中）：修正 §5 消费语法为落地实际形态（`computed<bool> is_aligned = byte_aligned(); computed<bool> needs_trailing_bits = !is_aligned; if (needs_trailing_bits) { rbsp_trailing_bits; }`，对应 `src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt:894-898`）；在能力验证矩阵与影响说明中如实记录直接条件形态 `if (!byte_aligned())` 被条件文法解析闸门拦截（`src/rules/dsl.cpp:1180`，报错原文 `Conditions require a field or context_value equality`）；
     - ADR-0090（英中）：更新 §3 解析器层描述，准确反映保留既有共享错误文本 `Arithmetic operators require u64 operands` 的实现；
     - ADR-0086（英中）：在 §5 追加 5.1 节修订记录，明确环境上下文导入依赖登记时机按需触发（commit `6fbf585`，`src/rules/rule_execution_session.cpp:338-343`）为单消息粒度失败隔离的必要要求。
  2. Session 级能力测试（`9c18436`，`tests/rules/rule_execution_session_test.cpp`）：
     - 新增 `materializesStructureWithUnaccessedAmbientImportWithoutContextAndIsolatesFailures`：
       - (a) 验证声明 `@context_import("h264-sps")` 但执行路径未访问 `context_value` 的消息结构体在 `ContextDirectory` 无任何 SPS 时正常物化（`Materialized`）且不登记导入依赖（`importedContexts.empty()`）；
       - (b) 验证求值 `context_value` 的分支在无 SPS 时精确产生单消息级 `DependencyUnavailable` 诊断；
       - (c) 验证 SPS 注册后求值分支成功绑定对应 generation；
       - (d) 验证 SPS 注册后执行未求值分支依然保持按需登记（`importedContexts.empty()`）。
  3. 全量验证门禁：
     - 本地三套构建与测试全量通过：dev 32/32、ci 32/32、sanitize 32/32（零 ASan/UBSan 告警）；
     - hosted run `31882632928` 在 Ubuntu 24.04 / Qt 6.11.1（job `95007234862`）、macOS 15 / Qt 6.11.1（job `95007234841`）、Windows 2022 / Qt 6.10.1（job `95007234953`）全部成功。
  Next Action 指向 Phase 3 阶段收尾审计（任务 T13）。
- 2026-08-15：完成阶段 3（H.264 正式结构支持）收尾审计与阶段关账（任务 T13）。
  1. H.264 Annex B 官方规则资产全量审计（`src/rules/official/org.streamview.h264/`，当前包版本 `0.1.38`）：
     - 结构范围涵盖 `NalUnitHeader`、`AccessUnitDelimiterRbsp`、`SequenceParameterSetRbsp`、`PictureParameterSetRbsp`、`HrdParameters`、`VuiParameters`、`PredWeightTable`、`SliceHeaderRbsp`、`SeiRbsp`、`EndOfSequenceRbsp`、`EndOfStreamRbsp`；
     - 语法元素全面覆盖 Baseline/Main/High profiles、POC Type 0/1/2、VUI/HRD 参数集、8 种切片类型（IDR、I/P/B 帧及场编码）、参考图像列表重排、显示加权预测表、7 种重点 SEI 消息（Buffering Period / Picture Timing / User Data Registered / User Data Unregistered / Recovery Point / Frame Packing / Display Orientation）以及中途按位置 SPS/PPS 上下文重定义；
     - 规则内声明的全部字段均标注 `@spec` 标准出处引用（ITU-T H.264）与双语语义说明。
  2. 测试套件与阶段门禁验收：
     - H.264 Analyzer 端到端测试用例达到 174/174 全量覆盖，包含完整有序 child 名字列表断言、精确 bit 坐标映射与源区间断言、截断安全回滚与诊断可定位性；
     - DSL 静态规则校验 `svtool rule check` 持续保持 `Rule OK`；
     - 本地 Debug/Release/Sanitize 三套构建全量 32/32 测试通过且零 ASan/UBSan 告警；
     - 阶段 3 清单全部项与 M6 里程碑中 H.264 项全部勾选验收。
  Next Action 指向阶段 4（AAC-LC 正式结构支持）：编写 ADTS 与 AudioSpecificConfig 格式规范与双语 ADR。
- 2026-08-15：阶段 3（H.264 正式结构支持）审计记录结构体名称更正（任务 T13 补正）。
  - 更正说明：上一条审计记录列出了概念性子结构名（`HrdParameters`、`VuiParameters`、`PredWeightTable`、`EndOfSequenceRbsp`、`EndOfStreamRbsp`），而在落地规则源码 `src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt` 中，实际声明的结构体定义及行号严格为：
    1. `struct NalUnitHeader`（第 9 行）
    2. `struct AccessUnitDelimiterRbsp`（第 21 行）
    3. `struct SequenceParameterSetRbsp`（第 43 行，VUI 与 HRD 语法在其内部条件块中扁平展开）
    4. `struct PictureParameterSetRbsp`（第 252 行，含高规格扩展语法）
    5. `struct IdrSliceLayerWithoutPartitioningRbsp`（第 347 行，含切片头语法）
    6. `struct NonIdrSliceLayerWithoutPartitioningRbsp`（第 443 行，加权预测表语法在其内部条件块中扁平展开）
    7. `struct SeiRbsp`（第 756 行，7 种重点 SEI 消息在其内部 switch-case 分支中扁平展开）
    8. `sequence<NalUnitHeader> nal_units = scan(h264_start_code);`（第 1042 行）
    9. `payload<rbsp> nal_units switch (nal_unit_type)`（第 1046–1055 行，其中 NAL type 10 与 11 派发为 `empty` 载荷）
  - 全部 7 个结构体与 1 个序列的所有声明字段均具有标准引用 `@spec` 与双语 `@description` 注解。
  Next Action 指向阶段 4（AAC-LC 正式结构支持，任务 T14–T18）：ADTS 帧枚举机制探测与 ADR-0092 架构起草。
- 2026-08-15：完成 ADTS 帧枚举机制架构探测与双语 ADR-0092 规范制定（任务 T14 / commit `fb69487`、`365049d` 与 `2b9be97`）。
  1. 架构探测与证据闸门：
     - 查明 DSL 解析器（`src/rules/dsl.cpp:3453-3457`）与 IR 降级（`src/rules/dsl_ir.cpp:3568-3574`）中扫描器名称闭集拦截（`Only h264_start_code is supported`）；
     - 明确 ADTS 分帧与 H.264 Annex B 的范式差异（ADTS 头部显式携带 13 位 `aac_frame_length`，支持 $O(1)$ 快速长度链步进）；
     - 设计包含 2 帧前瞻确认的重同步状态机，有效过滤音频载荷内的 `0xFFF` 假同步字。
  2. 规范与双语文档（`docs/adr/0092-aac-adts-frame-enumeration-and-rule-package.md` 与 `docs/zh-CN/adr/0092-aac-adts-frame-enumeration-and-rule-package.md`）：
     - 明确基于 ADR-0090 布尔算术计算字段与带 `at` 锚点的源定位断言表达最小帧长检查（`scratch/probe_adts.svfmt` 实测 `Rule OK`）；
     - 明确依据 ADR-0040 二分法将非布局字段 `sampling_frequency_index` 定级为 `@range(0, 12)` 非致命警告，并保留 `profile` 4 值枚举与 `channel_configuration` 8 值枚举；
     - 更新中英文格式语言参考（`docs/format-language/README.md` 与 `docs/zh-CN/format-language/README.md`）接纳 `scan(adts_frame)`。
- 2026-08-15：完成 ADTS 帧枚举能力切片（任务 T15a / commit `a565d41`）。
  1. DSL 与 IR 编译器层扩展（`src/rules/`）：
     - `dsl.cpp`：放宽 `scan(...)` 检查闸门，接纳 `h264_start_code` 与 `adts_frame`；
     - `dsl_ir.h` / `dsl_ir.cpp`：扩展 `DslScannerKind::AacAdtsFrame`，将 `scan(adts_frame)` 正确降级为类型化 IR 扫描记录；
     - `tests/rules/dsl_test.cpp` / `tests/rules/dsl_ir_test.cpp`：新增 `parsesAdtsFrameScanSequence` 与 `lowersAdtsFrameScanSequenceToTypedIr`，锁定正向解析/降级与未知扫描器负向拦截。
  2. 核心扫描器实现（`src/rules/include/streamview/rules/aac_adts_scanner.h` 与 `src/rules/aac_adts_scanner.cpp`）：
     - 实现 `AacAdtsScanner`，输出携带 `frameSpan`、`headerSpan`、`payloadSpan`、`crcPresent`、`aacFrameLength` 与 `truncated` 的 `AacAdtsRecord` 批次记录；
     - 实现基于 `inSync_` 的快速长度链步进与 2 帧前瞻重同步机制；
     - 严格对齐 ADR-0027 渐进索引合同，支持工作预算限制、取消检查与断点就地恢复。
  3. 全量测试矩阵（`tests/rules/aac_adts_scanner_test.cpp`）：
     - 新增 10 项端到端单元测试用例：`scansValidConsecutiveAdtsFrames`、`scansValidAdtsFramesWithCrc`、`resynchronizesAcrossLeadingGarbageBytes`、`handlesTruncatedFrameAtEof`、`resynchronizesAfterCorruptedFrameInLengthChain`、`rejectsFalseSyncwordsInsidePayload`、`respectsBatchLimitsAndWorkBudget`、`respectsCancellationAndResumesInPlace`、`handlesEmptyAndSmallSources`、`handlesInvalidBatchArguments`；
     - 规则引擎与解析全量验证：`svtool rule check` 保持 `Rule OK`，H.264 analyzer 174/174 零回归，全量测试套件扩充至 33/33 且在 dev/ci/sanitize 三套构建下 100% 通过（零 ASan/UBSan 告警）；
     - hosted run `31891829190` 在 Ubuntu 24.04 / Qt 6.11.1（job `95029138143`）、macOS 15 / Qt 6.11.1（job `95029138162`）、Windows 2022 / Qt 6.10.1（job `95029138117`）全部成功。
  Next Action 指向 Task T15b（ADTS 分析执行器、候选格式探测与多态会话选择）。
- 2026-08-15：完成 ADTS 应用集成切片（任务 T15b / commit `eb4e52c`）。
  1. 候选格式探测器（`src/rules/include/streamview/rules/aac_adts_detector.h` 与 `src/rules/aac_adts_detector.cpp`）：
     - 实现 `detectAacAdtsCandidate`，对前 64 KiB 页面执行 ADTS 同步字链前瞻追踪；
     - 依据同步链长度定级探测置信度（$\ge 3$ 帧为 `Strong`，2 帧为 `Probable`，1 帧为 `Weak`），有效避免孤立 `0xFFF` 假同步字误判为高置信度候选；
     - 单元测试（`tests/rules/aac_adts_detector_test.cpp`）覆盖干净连续流、短流、单帧、垃圾前缀、无长度链伪源与非 ADTS 数据。
  2. ADTS 分析执行器（`src/rules/include/streamview/rules/aac_adts_analyzer.h` 与 `src/rules/aac_adts_analyzer.cpp`）：
     - 实现 `AacAdtsAnalyzer` runner，封装 `AacAdtsScanner` 与 `AnalysisTree`；
     - 明确并在注释中记录工作预算计数语义（快速路径按帧头部字节推进，重同步路径按逐字节步进）；
     - 生成挂载 `header` 与 `raw_data_block` 的结构化分析树，支持取消中断与断点就地恢复；
     - 单元测试（`tests/rules/aac_adts_analyzer_test.cpp`）覆盖批量分析、批次限制、取消恢复与非法参数。
  3. 应用会话多态解耦（`src/app/analysis_session.h` 与 `src/app/analysis_session.cpp`）：
     - 解耦 `AnalysisSession`，通过 `std::variant<H264AnnexBAnalyzer, AacAdtsAnalyzer>` 支持基于候选探测置信度（或显式 `resolvedRule`）的多态格式分发；
     - 严格保持既有不变量：H.264 Annex B 全路径零回归（analyzer 174/174），探测/分析器创建失败绝不替换当前有效会话，未匹配未知源行为保持不变；
     - `tests/app/analysis_session_test.cpp` 新增 `opensAndAnalyzesAacAdtsStreamPolymorphically` 端到端集成用例。
  4. 全套三预设与云端 CI 验证：
     - 本地 dev / ci / sanitize 三套构建全量 35/35 测试 100% 通过（零 ASan/UBSan 告警）；
     - hosted run `31893001154` 在 Ubuntu 24.04 / Qt 6.11.1（job `95031971031`）、macOS 15 / Qt 6.11.1（job `95031971045`）、Windows 2022 / Qt 6.10.1（job `95031971075`）全部成功。
  Next Action 指向 Task T16（创建 AAC 规则包、ADTS 头结构化解码与双语 ADR-0093）。
- 2026-08-15：完成 ADTS 应用集成重构与 DSL 规则驱动架构改造（任务 T15b 重构 / commit `8edede4`）。
  1. 架构阻塞整改（B1、B2、B3 与非阻塞项）：
     - **B1 彻底消除 C++ 语法硬编码**：重写 `AacAdtsAnalyzer`（`src/rules/aac_adts_analyzer.cpp`），全面通过 `DslParser::parse` -> `DslCompiler::compile` -> `RuleExecutionSession` -> `DslExecutor::decodeStruct` 驱动 ADTS 头部结构的语法字段与计算字段物化，结构节点与语法字段严格由 DSL AST 生成，与 `h264_annex_b_analyzer.cpp` 保持同构；
     - **B2 消除规则身份伪造**：在没有内置 AAC 规则包时，`AacAdtsAnalyzer::create(source, errorMessage)` 干净失败返回 `std::nullopt` 并回填 `"No AAC ADTS rule package is bundled"`；在 `resolvedRule.succeeded()` 为假时返回 `std::nullopt` 并回填 `"Failed to resolve AAC ADTS entry rule"`；彻底删除零调用方死桩 `aacAdtsRuleSource()` 与 `loadAacAdtsRulePackage()`；
     - **B3 探测置信度严格收紧与会话选择修复**：`detectAacAdtsCandidate`（`src/rules/aac_adts_detector.cpp`）删除 `sourceFullyInspected` 降档绕过子句，置信度严格锁定为 >=3 帧连续长度链为 `Strong`、2 帧为 `Probable`、1 帧为 `Weak`；`AnalysisSession::createPrepared` 仅当 AAC 置信度为 `Strong` 且 H.264 置信度非 `Strong` 时才进入 AAC 分支，且在创建 AAC 分析器失败（如当前阶段尚无打包规则）时平滑回退至 H.264 既有路径；
     - **跨格式批次类型解耦**：在 `streamview::app` 命名空间引入格式中立的 `AnalysisBatchStatus` 枚举与 `AnalysisBatchResult` 结构体，消除借用 H.264 专属状态的 `static_cast` 转换以及 `AnalysisTree::nalUnitNodes` 的跨格式借用。
  2. 边界测试矩阵扩展（`tests/`）：
     - `tests/rules/aac_adts_analyzer_test.cpp`：使用内存 `RulePackage` + `RulePackageCatalog` 驱动测试解析 ADTS 规则包，验证 `syncword`、`id`、`layer`、`protection_absent` 等 DSL 字段在 `AdtsHeader` 子树中完整物化；验证未安装包时 `create` 失败并报告错误信息；
     - `tests/rules/aac_adts_detector_test.cpp`：消除弱化断言，精确锁定各测试用例置信度分级（`Strong`、`Probable`、`Weak`）；新增 2 KiB 随机二进制中仅含单处偶然 `FF F1` 判定为 `Weak`，以及带有合法 start code 但 NAL 头非法、payload 内含单处偶然同步字的畸形 H.264 判定为 `Weak`；
     - `tests/app/analysis_session_test.cpp`：验证含偶然 `FF F1` 的未知二进制默认走 H.264（无候选命中）、畸形 H.264 默认走 H.264、显式指定 AAC 规则包与内存 catalog 下的多态会话打开。
  3. 全套构建与云端验证：
     - 本地 dev / ci / sanitize 三套构建全量 35/35 测试 100% 通过（ASan/UBSan 零告警，H.264 analyzer 174/174 零回归）；
     - hosted run `31895579352` 在 Ubuntu 24.04 / Qt 6.11.1（job `95038286039`）、macOS 15 / Qt 6.11.1（job `95038285962`）、Windows 2022 / Qt 6.10.1（job `95038285995`）全部成功通过。
  Next Action 指向 Task T16（创建 AAC 官方规则包、ADTS 头结构化解码与双语 ADR-0093）。
- 2026-08-16：阶段 4 任务 T15b 评审勘误与真实 ADTS 降级打开修复（任务 T15b 修复 / commit `229aea5`）。
  - 历史记录勘误说明：
    1. **置信度分级描述勘误**：在旧 T15b 提交 `eb4e52c` 的实现中，`detectAacAdtsCandidate` 曾包含 `sourceFullyInspected` 降档子句（使得 1~2 帧短流在源完全探测时被提升至 `Strong`），导致未知源或含单处 `FF F1` 的二进制被误判为 `Strong` AAC；原记录第 1 款未提及该降档子句。已在 `8edede4` 中删除该降档分支并固化严格连续帧计数（>=3 帧为 `Strong`，2 帧为 `Probable`，1 帧为 `Weak`）。
    2. **未匹配未知源行为与平滑回退勘误**：在旧 T15b 提交 `eb4e52c` 与第一轮重构 `8edede4` 中，`AnalysisSession::createPrepared`（`src/app/analysis_session.cpp:184-213`）在无内置 AAC 规则包时调用 `AacAdtsAnalyzer::create` 返回 `nullptr`，导致真实 3 帧/8 帧 ADTS 文件无法打开，推翻了「未匹配未知源行为保持不变」的原声明。
    3. **错误文案引用勘误**：AAC 分析器创建失败回填的错误文案原文为 `"AAC ADTS rule was not resolved exactly"`（`src/rules/aac_adts_analyzer.cpp:72`）与 `"No AAC ADTS rule package is bundled"`（`src/rules/aac_adts_analyzer.cpp:102`），此前报告曾笔误引用为未存在字符串。
  - 真实 ADTS 会话降级打开与自动选择修复：
    1. **自动格式选择闸门修复**：`src/app/analysis_session.cpp:185-186` 严格要求 `aacConf == Strong && h264Conf != Strong`，并彻底移除 `Probable` 分支；
    2. **无规则包时平滑降级**：`src/app/analysis_session.cpp:191-203` 在 `chooseAac` 成立但 `AacAdtsAnalyzer::create` 失败时（当前阶段未打包 AAC 规则），干净平滑回退至既有 H.264 / 未知源分析路径（`H264AnnexBAnalyzer::create`），确保会话成功打开；
    3. **测试矩阵补全**（`tests/app/analysis_session_test.cpp`）：
       - 新增 `opensRealAdtsWithoutBundledPackageByFallingBackToH264`：真实 3 帧 ADTS（AAC `Strong`）在无包时成功打开会话，保留 AAC `Strong` 探测候选，分析器平滑降级至 `org.streamview.h264` / `annex-b` 且批次分析正常完成；
       - 新增 `opensAdtsWithProbableConfidenceByFallingBackToH264`：2 帧 ADTS（AAC `Probable`）不触发 AAC 自动选择，正常走 H.264 路径打开会话并完成批次分析；
    4. **全套构建与云端 CI 验证**：
       - 本地 dev / ci / sanitize 三套构建全量 35/35 测试 100% 通过（ASan/UBSan 零告警，H.264 analyzer 174/174 零回归）；
       - hosted run `31896991272` 在 Ubuntu 24.04 / Qt 6.11.1（job `95041713226`）、macOS 15 / Qt 6.11.1（job `95041713240`）、Windows 2022 / Qt 6.10.1（job `95041713206`）全部成功通过。
  Next Action 指向 Task T16（创建 AAC 官方规则包、ADTS 头结构化解码与双语 ADR-0093）。

- 2026-08-16：完成 AAC 官方规则包落地、ADTS 固定/可变头结构化解码与双语 ADR-0093（任务 T16 / commit `530187f`、`cd13322`、`7116913` 与 `f0df4e8`）。
  1. 架构规范与双语 ADR-0093（`docs/adr/0093-adts-header-structured-decoding-and-official-rule-package.md` 与 `docs/zh-CN/adr/0093-adts-header-structured-decoding-and-official-rule-package.md`）：
     - 依据 ISO/IEC 13818-7:2006 §8.2 与 ISO/IEC 14496-3:2009 §1.A.1 规范完整定义 `AdtsHeader` 语法结构体，覆盖固定头（`syncword`、`id`、`layer`、`protection_absent`）与可变头（`profile`、`sampling_frequency_index`、`private_bit`、`channel_configuration`、`original_copy`、`home`、`copyright_identification_bit`、`copyright_identification_start`、`aac_frame_length`、`adts_buffer_fullness`、`number_of_raw_data_blocks_in_frame`、`crc_check` 与 `minimum_frame_length`）；
     - 明确多块限制（`number_of_raw_data_blocks_in_frame @equals(0)`）与逐帧错误隔离语义（语法内容错误仅将当前帧节点标记为 `Invalid` 并返回 `true` 继续分析后续帧，基础设施错误返回 `false` 中断流）；
     - 明确头部截断（Error，`TruncatedSource`）与载荷截断（Warning，`TruncatedSource` + 头部结构正常物化）双重截断诊断契约。
  2. 官方规则包与资源注册（`src/rules/official/org.streamview.aac/`）：
     - `rule.toml`：发布 `org.streamview.aac` 官方包（版本 `0.1.0`），入口点 `adts`，深度 `structural`；
     - `src/aac_adts.svfmt`：实现 ADTS 头规则源码，`svtool rule check` 实测 `Rule OK`；
     - `src/rules/CMakeLists.txt`：通过 `qt_add_resources` 将 AAC 规则包资产编译入 `streamview_official_rules_aac` 二进制资源。
  3. 分析执行器与工具集成（`src/rules/` 与 `tools/svtool/`）：
     - `AacAdtsAnalyzer::create(source, &error)` 默认重载连接 `bundledAacAdtsRule()` 自动加载内置包；
     - `publishRecord` 实现逐帧隔离（`InvalidSyntax` / `TruncatedSource` 标记 `Invalid` 后继续步进后续帧）与 EOF 载荷截断标记；
     - `tools/svtool/main.cpp` 引入 AAC ADTS 多态探测与分析支持。
  4. 全量测试矩阵（`tests/`）：
     - `tests/rules/aac_adts_analyzer_test.cpp`：覆盖内置包加载、完整字段名列表有序物化、多块违规逐帧隔离并继续分析、CRC 存在时头部截断（8 字节）、EOF 载荷截断（头部物化 + 帧 Warning 诊断）、短于头部垃圾跳过、破损跨度重同步、批次限制与取消恢复；
     - `tests/app/analysis_session_test.cpp`：覆盖内置包存在时真实 ADTS 自动激活多态会话（`org.streamview.aac` / `adts`），以及 AAC Strong 与 H.264 Strong 争用时默认选 H.264 路径；
     - H.264 规则套件 174/174 零回归，全量测试套件 35/35 在 dev/ci/sanitize 三套构建下 100% 通过（零 ASan/UBSan 告警）；
     - hosted run `31900233536` 在 Ubuntu 24.04 / Qt 6.11.1（job `95049723785`）、macOS 15 / Qt 6.11.1（job `95049723689`）、Windows 2022 / Qt 6.10.1（job `95049723728`）全部成功通过。
  Next Action 指向 Task T17（AAC raw data block 与 AudioSpecificConfig 架构探索与双语 ADR-0094）。

- 2026-08-16：阶段 4 任务 T16 审计记录更正（任务 T16 补正）。
  - 更正说明：
    1. **标准条款引用更正**：上一条记录提及「依据 ISO/IEC 13818-7:2006 §8.2 与 ISO/IEC 14496-3:2009 §1.A.1」，`src/rules/official/org.streamview.aac/src/aac_adts.svfmt` 与 ADR-0093 实际采用 `docs/standards.md:10` 固定的 ISO/IEC 14496-3:2019 Edition 5 subclause 1.6.2.1（固定头）与 1.6.2.2（可变头）（注：该 1.6.2.1/1.6.2.2 引用在后续任务 T17d 中被进一步澄清并更正为 Subpart 1 Annex 1.A 的 1.A.1 与 1.A.2）；
    2. **入口点深度值更正**：上一条记录提及「深度 `structural`」，`src/rules/official/org.streamview.aac/rule.toml` 实际声明为 `depth = "adts-frame"`（严格对齐 ADR-0092 §4.1）。
  Next Action 指向 Task T17（AudioSpecificConfig、GASpecificConfig 与 Program Config Element 架构探索、双语 ADR-0094 与规则落地）。

- 2026-08-16：完成 AudioSpecificConfig 与 Program Config Element 规则消费落地（任务 T17c / commit `d32d606`）。
  1. 架构规范与双语 ADR-0094（`docs/adr/0094-audio-specific-config-and-program-config-element.md` 与 `docs/zh-CN/adr/0094-audio-specific-config-and-program-config-element.md`）：
     - 依据 ISO/IEC 14496-3:2019 subclause 1.6.2.1 与 4.4.1 规范定义 `AudioSpecificConfig`、`GASpecificConfig` 与 `program_config_element`（PCE）结构化语法；
     - 严格推导并形式化验证 PCE 字节对齐补零位精确计算公式：`pce_total_bits = 16 + (audio_object_type == 31) * 6 + (sampling_frequency_index == 15) * 24 + depends_on_core_coder * 14 + 34 + mono_mixdown_present * 4 + stereo_mixdown_present * 4 + matrix_mixdown_idx_present * 3 + num_front_channel_elements * 5 + num_side_channel_elements * 5 + num_back_channel_elements * 5 + num_lfe_channel_elements * 4 + num_assoc_data_elements * 4 + num_valid_cc_elements * 5`，`pce_rem = pce_total_bits % 8`，`pce_alignment_bits = (8 - pce_rem) % 8`。
  2. 官方规则包与资源注册（`src/rules/official/org.streamview.aac/`）：
     - `rule.toml`：发布版本 `0.1.1`，新增 `asc` 入口点（`id = "asc"`, `format = "audio.aac.asc"`, `source = "src/aac_asc.svfmt"`, `depth = "structural"`），并将 `detector = "aac-adts"` 固化于 `adts` 入口点块内；
     - `src/aac_asc.svfmt`：实现 ASC / PCE 完整规则源码（184 行），`svtool rule check` 实测 `Rule OK`；
     - `src/rules/CMakeLists.txt`：将 `official/org.streamview.aac/src/aac_asc.svfmt` 纳入 `streamview_official_rules_aac` Qt 资源编译；
     - `src/rules/aac_adts_analyzer.cpp`：`loadAacAdtsRulePackage` 读取并注册 `src/aac_asc.svfmt`；
     - 公共 API 变更说明：`src/rules/include/streamview/rules/aac_adts_analyzer.h:39` 导出 `loadAacAdtsRulePackage()`。测试（`tests/rules/aac_adts_analyzer_test.cpp`）需要直接驱动官方内置规则包并解析多入口点（`adts` 与 `asc`）、校验包元数据和编译内置规则，而非依赖测试内合成的内存包（如 `tests/app/analysis_session_test.cpp:428` 模式）。该导出仅供测试套件与 `bundledAacAdtsRule()` 内部使用，不对外承诺 public ABI 稳定性。
  3. 测试套件与验证矩阵（`tests/rules/aac_adts_analyzer_test.cpp`）：
     - `resolvesAscEntryPointFromBundledRulePackage`：验证 `org.streamview.aac` 0.1.1 内置包成功加载，`RulePackageCatalog::resolve` 成功解析 `asc` 入口点且元数据（format, depth, sourcePath）完全匹配；
     - `decodesAscCase1Baseline`：基线 ASC（AOT=2, SFI=3, CC=0, DCC=0），对齐 6 bit，`comment_field_bytes` 偏移 7 字节，完整 118 个有序子节点名称与值断言通过；
     - `decodesAscCase2CoreCoderDelay`：DCC=1（14-bit core_coder_delay=0x1ABC），对齐 0 bit，`comment_field_bytes` 偏移 8 字节，完整 113 个有序子节点断言通过；
     - `decodesAscCase3ExtendedAudioObjectType`：AOT=31（6-bit AOT_ext=2），对齐 0 bit，`comment_field_bytes` 偏移 7 字节，完整 113 个有序子节点断言通过；
     - `decodesAscCase4ExplicitSamplingFrequency`：SFI=15（24-bit sampling_frequency=44100），对齐 6 bit，`comment_field_bytes` 偏移 10 字节，完整 119 个有序子节点断言通过；
     - `decodesAscCase5MultichannelFrontAndLfe`：多声道 Front=2 + LFE=1，对齐 0 bit，`comment_field_bytes` 偏移 8 字节，完整 118 个有序子节点断言通过；
     - `decodesAscCase6AllMixdownPresent`：Mono/Stereo/Matrix 全部 mixdown 存在，对齐 3 bit，`comment_field_bytes` 偏移 8 字节，完整 119 个有序子节点断言通过；
     - `decodesAscCase7MultichannelSideBackAssocCc`：Side/Back/Assoc/CC 全部存在，对齐 3 bit，`comment_field_bytes` 偏移 9 字节，完整 122 个有序子节点断言通过；
     - `rejectsAscCase8NonzeroAlignmentBit`：填充位违规断言触发（`DiagnosticCode::InvalidSyntax`，`Field value violates @equals constraint`）；
     - `rejectsAscCase9PrematureTruncation`：截断源错误断言触发（`DiagnosticCode::TruncatedSource`，`Unable to read complete syntax field`）；
     - `bundledAacAdtsRuleResolvesAdtsWithoutRegression`：验证 `adts` 入口点解析与 ADTS 分析无回归；
     - Red 验证：实测在旧版基数 31 错误公式下，Case 1 与 Case 4 对齐位数算错导致 `comment_field_bytes` 数值由 90 严重错位为 2，证实新测试为强有效信号；
     - 本地 dev / ci / sanitize 三套构建全量 35/35 测试全部通过（ASan/UBSan 零告警，AAC analyzer 23/23，H.264 analyzer 174/174）；
     - hosted run 31928187049 在 Ubuntu 24.04 / Qt 6.11.1（job 95118891822）、macOS 15 / Qt 6.11.1（job 95118891810）、Windows 2022 / Qt 6.10.1（job 95118891801）全部成功通过。
  Next Action 指向 Task T17d（T17c 规范引用整改与记录补齐）。

- 2026-08-16：完成 T17c 规范引用整改与记录补齐（任务 T17d）。
  1. 规范标准条款号全面厘清（对照 ISO/IEC 14496-3:2019 第 5 版基线）：
     - `adts_fixed_header` + `adts_variable_header`：子条款 **1.A.1**（标题：*Fixed and variable header of ADTS*，位于 Subpart 1 Annex 1.A）；
     - `adts_error_check`（`crc_check`）：子条款 **1.A.2**（标题：*Error detection*，位于 Subpart 1 Annex 1.A）；
     - `AudioSpecificConfig`：子条款 **1.6.2.1**（标题：*AudioSpecificConfig*，位于 Subpart 1 1.6.2）；
     - `GASpecificConfig`：子条款 **4.4.1**（标题：*GASpecificConfig* / *General Audio specific configuration*，位于 Subpart 4 4.4）；
     - `program_config_element`（PCE）：子条款 **4.4.1.1**（标题：*Program config element (PCE)*，位于 Subpart 4 4.4.1）。
  2. 回退与纠正：
     - 回退 `aac_adts.svfmt:19` 此前误改的 `1.6.2.2`，修正为 `1.A.1`；`crc_check` 修正为 `1.A.2`；
     - `aac_asc.svfmt`：ASC 基础头标 `1.6.2.1`、GASpecificConfig 段标 `4.4.1`、PCE 段 25 个字段全量标 `4.4.1.1`；
     - 双语 ADR 文档同步更新：ADR-0092、ADR-0093 与 ADR-0094 英中双语版本文末均追加「条款引用更正」小节，更新引用列表；
     - 包版本变更：`org.streamview.aac` 由于 `.svfmt` 文件内 `@spec` 注解变更导致内容哈希变化，升级 patch 版本至 `0.1.2`（`rule.toml` 与对应测试断言同步更新）。
  3. 阶段 4 实施计划勾选：
     - 勾选「解析 AudioSpecificConfig、GASpecificConfig 和 Program Config Element」复选框，关联 ADR-0094 9 项测试矩阵与 hosted run 31928187049 验收证据；
     - hosted run 31930252331 在 Ubuntu 24.04 / Qt 6.11.1（job 95123835282）、macOS 15 / Qt 6.11.1（job 95123835271）、Windows 2022 / Qt 6.10.1（job 95123835269）全部成功通过。
  Next Action 指向 Task T18（AAC raw data block 与 channel stream element 解码架构探索）。
- 2026-08-16：完成 ADR 正文条款号与版本号收尾（任务 T17e）。
  1. ADR 正文与参考资料条款号对齐（消除内部矛盾）：
     - `docs/adr/0092-*.md` 与 `docs/zh-CN/adr/0092-*.md`：更正背景第 9 行（Annex 1.A）、`aac_frame_length` 规范引用（1.A.1）、正文条款枚举（1.A.1, 1.A.2）、`sampling_frequency_index` 超范围引用（1.A.1）及 References 引用（1.A.1, 1.A.2）；
     - `docs/adr/0093-*.md` 与 `docs/zh-CN/adr/0093-*.md`：更正 `sampling_frequency_index` 超范围引用（1.A.1），并在 Amendment 中声明正文与参考资料已同步更正；
     - 经过 grep "1.6.2" 全面审计，ADR-0092 与 ADR-0093 中唯一保留的 "1.6.2.1" 仅存在于文末更正说明（用于明确辨析 1.6.2.1 属于 AudioSpecificConfig 而非 ADTS）。
  2. ADR-0094 版本号全面推进至 0.1.2：
     - `docs/adr/0094-*.md` 与 `docs/zh-CN/adr/0094-*.md`：§Context 第 3 条、§1 引导句、§1 清单代码块、§Positive 全部更新为 `0.1.2`；
     - §Verification Matrix 中的 `RulePackage::fromFiles` 输出块经独立 C++ 探针实测更新为真实 loader 输出原文：`id=org.streamview.aac version=0.1.2 license=MIT`。
  3. ADR-0094 过时措辞修正：
     - §2 引导句更正为跨三个子条款的准确表述（1.6.2.1 `AudioSpecificConfig` / 4.4.1 `GASpecificConfig` / 4.4.1.1 `program_config_element`）；
     - §Context 第 7 行与 References 更正：MP4 `esds` box 规范基线严格对齐 `docs/standards.md:11` 的 ISO/IEC 14496-1:2010（Systems 描述符）与 ISO/IEC 14496-14。
  Next Action 指向 Task T18（AAC raw data block 与 channel stream element 解码架构探索）。
- 2026-08-16：完成 AAC raw_data_block 压缩载荷与 Profile 处理的探测与双语 ADR-0095（任务 T18a）。
  1. 架构规范与双语 ADR-0095（`docs/adr/0095-aac-raw-data-block-compressed-payload-and-profile-handling.md` 与 `docs/zh-CN/adr/0095-aac-raw-data-block-compressed-payload-and-profile-handling.md`）：
     - 依据 ISO/IEC 14496-3:2019 定义 `raw_data_block` 在 ADTS 中的编排归属（Subpart 1 Annex 1.A subclause 1.A.1 / Table 1.A.5）与句法定义归属（Subpart 4 subclause 4.5.2.1）；
     - 确定在 `AdtsHeader` 末尾追加 `header_bytes`、`raw_data_block_bytes` 计算字段及 `@lazy(raw_data_block_bytes) bytes raw_data_block` 规则语法，经实测无需新 DSL 语言能力；
     - 确立阶段 4 收尾任务拆分纪律：T18b（执行器视图映射改帧跨度，能力切片）、T18c（规则消费 `@lazy raw_data_block`，版本 0.1.3）、T18d（Profile 处理验证）、T18e（逐 bit 验收审计与阶段 4 闭环）。
  2. 探测实测结论与截断契约决策：
     - 实测确认执行器视图映射为阻塞项：`src/rules/aac_adts_analyzer.cpp:346` 需由 `headerSpan` 改为 `frameSpan`（T18b）；
     - 采纳方案 A 统一 DSL VM 截断语义：载荷截断时由 VM 发出 Error 级 `TruncatedSource`（`Lazy byte region exceeds the available source range`），取代原 C++ 合成 Warning 诊断，并计划更新现存测试断言；
     - 探针全面验证 DSL 侧不支持 Profile 的能力现状：`@enum` 违规为致命错误，`@range` 仅支持单段连续区间，ADTS 2-bit profile 无法表示 AOT 5/29/39；正式确认 ASC 非 GA AOT 解析 GA 基础头为已知且受控的已记录能力边界。
  3. 文档引用同步维护：
     - 同步修正 `docs/adr/0093-*.md` 与 `docs/zh-CN/adr/0093-*.md` 第 17 行 `raw_data_block` 条款号归属并在文末更正小节追加说明。
  Next Action 指向 Task T18b（更新 AAC ADTS 分析器执行器视图映射至帧跨度）。
- 2026-08-16：完成 ADR-0095 审计整改与规范强化（任务 T18a-fix）。
  1. ADR-0095 逐 bit 验收审计独立小节落盘（B1）：
     - 在双语 ADR-0095 中正式建立「逐 bit 验收审计范围与覆盖矩阵」独立小节，将 5 大类别（ADTS 头、ASC-PCE、截断、CRC 存在性、Profile）的既有覆盖与缺口清单逐条精确引用至 `file:line`；
     - 纠正 ADTS 头部覆盖误述：明确指出 ADTS 头部目前尚无 `logicalRange()` / `bitOffset()` 断言，且下标 8–11 字段（`original_copy`、`home`、`copyright_identification_bit`、`copyright_identification_start`）缺失数值断言，将其列为 T18e 明确补齐项；
     - 定死 CRC 存在性与错误的验收语义边界：明确 DSL 不做 CRC-16 多项式除法计算，验收边界严格限定为 `protection_absent == 0` 时物化 bit 56..71 的 `crc_check` 字段、`== 1` 时省略、以及不足 9 字节时报 `TruncatedSource`。
  2. 规则字段精简与条款号自洽（B2, N1, N4）：
     - 消除冗余计算字段：复用既有 `minimum_frame_length` 表达式，直接计算 `raw_data_block_bytes = aac_frame_length - minimum_frame_length`，消除 `header_bytes`，子节点数精简至 18（无 CRC）/ 19（含 CRC）；
     - 条款号自洽：`raw_data_block` 标注严格对齐 Subpart 4 子条款 `@spec("ISO/IEC 14496-3:2019", "4.5.2.1")`，与定义固定/可变头的 `1.A.1` 彻底解耦；ADR-0093 更正小节同步澄清此前 4.5.2.1.1 笔误。
  3. 未识别注解行为记录与复现指令规范（B3, B4, N2, N3, N5）：
     - 修正中文版 `docs/zh-CN/format-language/README.md:443-445` 行号引用；
     - 验证矩阵提供基于独立 C++ 探针的完整复现编译命令与代码构造说明（B4 方案 b）；
     - 探针直接证明 `@range` 无法表达非连续集合（重复 `@range` 被编译器拒绝，多参数 `@range` 被语法分析器拒绝；N3）；
     - 实测并正式记录「未识别注解被静默忽略导致约束失效」现象（N2），作为未来语言闸门强化的候选事项上报；
     - 补齐 Task T18c 测试套件迁移清单（`aac_adts_analyzer_test.cpp:143-170`, `:193-220`, `:114`, `:210`, `:321-361`；N5）。
  Next Action 指向 Task T18b（更新 AAC ADTS 分析器执行器视图映射至帧跨度）。
- 2026-08-16：完成 ADR-0095 测试用例引用整改与审计缺口核实（任务 T18a-fix2）。
  1. 测试名称与行号精准对齐（C1, C2）：
     - 彻底清除所有编造测试名，全面替换为 `tests/rules/aac_adts_analyzer_test.cpp` 中 21 个真实用例的规范名称与行号；
     - 澄清 ADTS 头部解码覆盖全部来自 `createsAnalyzerFromBundledPackageAndDecodesFieldsViaDsl`（:106-227）单一用例的多帧断言；
     - 清理 §4 迁移清单中 `:114` 与 `:210` 两处非断言行，严格保留 `:143-170`、`:193-220` 及 `:321-361` 三处真实断言点。
  2. 审计覆盖与缺口清单严格核实（C3）：
     - Category 2（ASC / PCE）：修正为 `:515-1751`，确认既有覆盖为 113–122 个有序子节点名称与各例 1 处 `comment_field_bytes` 偏移/值断言（:675, :839, :1003, :1173, :1338, :1505, :1675），其余字段无 `logicalRange` / `bitOffset` 断言列为缺口；
     - Category 3（截断）：列出 `:286-320`（`handlesHeaderTruncationWithCrcPresent`）、`:321-361`（`handlesPayloadTruncationAtEof`）、`:363-386`、`:388-417` 四个真实用例区间，并把无 `logicalRange` 断言如实列为缺口；
     - Category 4（CRC）：修正为 `createsAnalyzerFromBundledPackageAndDecodesFieldsViaDsl`（帧 1 `crc_check` 值断言在 :225）与 `handlesHeaderTruncationWithCrcPresent`（:286-320），缺口为 `crc_check` 无 bitOffset 断言。
  3. 条款号出处与双闸门规范标注（C4, 两个小项）：
     - §1 明确标注条款号 `4.5.2.1` 与表 `1.A.5` 为未经规范正文核实，确认的最小范围为 Subpart 1 Annex 1.A 与 Subpart 4；
     - 修正 `dsl_ir.cpp:185` 行号引用；
     - 验证矩阵 P7/P8 标注为「parser 与 compiler 双闸门」。
  Next Action 指向 Task T18b（更新 AAC ADTS 分析器执行器视图映射至帧跨度）。
- 2026-08-16：完成 AAC ADTS 分析器执行器视图映射扩展至帧跨度（任务 T18b / commit `e3f1f1b`）。
  1. 分析器执行逻辑视图映射扩展（`src/rules/aac_adts_analyzer.cpp`）：
     - 将 `publishRecord` 中的 `SourceMapping` 构造由 `{*record.headerSpan}` 调整为 `{*record.frameSpan}`，解除后续 Task T18c 消费 `@lazy raw_data_block` 时的视图跨度阻塞（ADR-0095 §3）；
     - 将局部变量 `headerViewId` 重命名为 `frameViewId`，保持语义命名一致性。
  2. 纯能力/执行器切片与零断言变更验证：
     - 规则包 `org.streamview.aac` 保持版本 `0.1.2`，未修改任何 `.svfmt` 与 `rule.toml` 资产；
     - 现有测试断言改动数为 0，实测确认对既有规则输出为精确 no-op；
     - 本地全量测试 35/35 在 dev/ci/sanitize 三套构建下全部通过（零 sanitizer 告警，AAC analyzer 23/23，H.264 analyzer 174/174）；
     - hosted CI run `31934776263` 在 Ubuntu 24.04 / Qt 6.11.1（job `95134873874`）、macOS 15 / Qt 6.11.1（job `95134873797`）、Windows 2022 / Qt 6.10.1（job `95134873867`）全部 100% 成功通过。
  Next Action 指向 Task T18c（官方 AAC 规则包消费 `@lazy raw_data_block`、升级至 0.1.3 并同步迁移测试断言）。
- 2026-08-16：完成官方 AAC 规则包消费 `@lazy raw_data_block` 与版本升级至 0.1.3（任务 T18c / commit `c5ab86c`）。
  1. 官方规则包规则消费与版本升级（`src/rules/official/org.streamview.aac/`）：
     - `src/aac_adts.svfmt`：在 `AdtsHeader` 中复用 `minimum_frame_length` 追加 `computed<u64> raw_data_block_bytes = aac_frame_length - minimum_frame_length;` 与 `@lazy(raw_data_block_bytes) bytes raw_data_block`（标注 Subpart 4 子条款 `4.5.2.1`）；
     - `rule.toml`：发布版本升级至 `0.1.3`。
  2. 测试套件断言迁移与截断契约对齐（`tests/rules/aac_adts_analyzer_test.cpp`）：
     - `createsAnalyzerFromBundledPackageAndDecodesFieldsViaDsl`：帧 0 子节点数推进至 18（追加 `raw_data_block_bytes` 与 `raw_data_block`）；帧 1 子节点数推进至 19；
     - `handlesPayloadTruncationAtEof`：对齐全库 DSL VM 截断契约，断言严重性由 Warning 迁移至 Error，诊断消息迁移至 `"Lazy byte region exceeds the available source range"`，`header1->state()` 迁移至 `Invalid`；
     - `resolvesAscEntryPointFromBundledRulePackage`：版本断言同步迁移至 `"0.1.3"`（G1）；
     - 实测确认完整帧 20 字节 `raw_data_block` 正确物化（无 CRC 时 bitOffset=56 bitLength=104，含 CRC 时 bitOffset=72 bitLength=88，状态为 `MaterializationState::Lazy`）。
  3. 遗留事项处理与切片拆分（G3）：
     - 确认 `aac_adts_analyzer.cpp:456-467` 现已完全不可达，排定专属切片 Task T18c-2 完成代码清理；
     - 本地全量测试 35/35 在 dev/ci/sanitize 三套构建下全部通过（零 sanitizer 告警，AAC analyzer 23/23，H.264 analyzer 174/174）；
     - hosted CI run `31936367969` 在 Ubuntu 24.04 / Qt 6.11.1（job `95138832621`）、macOS 15 / Qt 6.11.1（job `95138832586`）、Windows 2022 / Qt 6.10.1（job `95138832588`）全部 100% 成功通过。
  Next Action 指向 Task T18c-2（清理 AAC ADTS 分析器中已不可达的载荷截断 Warning 遗留代码）。
- 2026-08-16：完成 AAC ADTS 分析器不可达载荷截断 Warning 遗留代码清理与可达性论证（任务 T18c-2 / commit `7e5b176`）。
  1. 分析器死代码清理（`src/rules/aac_adts_analyzer.cpp`）：
     - 删除原 `publishRecord` 中的 `if (record.truncated) { ... }` 合成 Warning 代码块，执行器能力切片，不升规则包版本（`org.streamview.aac` 保持 `0.1.3`）；
     - 确认 `frameIndex` 与 `frameLocation` 在节点构造与异常诊断分支中仍被正常使用，无任何未使用变量或编译器告警。
  2. 穷举三路径严格可达性论证（ADR-0095 §4 G3）：
     - **路径 A（DSL 失败隔离）**：`!execution.materialized()` 分支在第 426 行标记 `Invalid` 后于第 452 行直接 `return true`，绝无可能流向后续语句；
     - **路径 B（VM 截断必然性）**：`availableBytes < aac_frame_length` 时，Reader 剩余 `availableBytes - minimum_frame_length < raw_data_block_bytes`，`src/rules/dsl_vm.cpp:3140` 中的 `bitCount > reader.remainingBits()` 无条件成立，恒定返回 `TruncatedSource`，必然落入路径 A；
     - **路径 C（守卫恒真不变式）**：Scanner 产出任何 record 均要求 `offset + 7 <= sourceSize`，`availableHeader >= 7` 字节，`record.headerSpan->bitLength() >= 56 > 0` 恒真，第 329 行守卫绝无可能短路。
  3. 实测行为等价性与全量验证：
     - 实测构造 200 字节声明、仅有 50 字节的截断码流，删除前后诊断输出 100% 一致（`diag code=0 sev=2 msg="Lazy byte region exceeds the available source range"`，`AdtsHeader` 保持 17 子节点且状态为 `Invalid`）；
     - 本地全量测试 35/35 在 dev/ci/sanitize 三套构建下全部通过（零 sanitizer 告警，AAC analyzer 23/23，H.264 analyzer 174/174）；
     - hosted CI run `31937210029` 在 Ubuntu 24.04 / Qt 6.11.1（job `95140905425`）、macOS 15 / Qt 6.11.1（job `95140905350`）、Windows 2022 / Qt 6.10.1（job `95140905352`）全部 100% 成功通过。
  Next Action 指向 Task T18d（Profile 处理验证与全库路线图文档对齐）。
- 2026-08-16：完成可达性论证前提缺陷修正、能力边界回归测试与文档整改（任务 T18c-2-fix / commit `474a4af`）。
  1. 新增能力边界回归测试（`tests/rules/aac_adts_analyzer_test.cpp:379-475`）：
     - 新增 `materializesTruncatedTrailingFrameWhenRuleLacksPayloadDeclaration` 用例，构造未声明载荷区域的自定义 ADTS 规则包，验证完整帧 + 截断尾帧码流；
     - 实测断言钉住收窄后的已知能力边界：尾帧 region 节点状态为 `MaterializationState::Materialized`，诊断数严格为 0，`logicalRange().bitLength()` 等于实际可用比特数（30 字节 = 240 比特，被 clamp 的结果）；
     - AAC analyzer 测试套件用例数由 23 扩展至 24，全部 24/24 通过。
  2. 双语 ADR-0095 §4 G3 与 §5 记述整改：
     - 移除无条件不可达表述，明确声明三路径论证严格依赖「生效规则声明了覆盖帧剩余字节的 `@lazy` 区域」（官方包 `0.1.3` 满足）；
     - 明示收窄后果：未声明载荷区域的自定义/第三方包截断尾帧将以 `Materialized` 且无诊断呈现；
     - 阐明架构理由：C++ 中硬编码格式专属诊断违反 DSL-first 架构约束，截断上报的正当机制是规则声明载荷区域后由 VM 通用契约触发；
     - 记录 `record.truncated` 现状：在 `src/` 中已无读取点，作为 scanner 扫描事实保留，由 scanner 测试套件持续断言。
  3. 构建与持续集成验证：
     - 本地全量测试 35/35 在 dev/ci/sanitize 三套构建下全部通过（零 sanitizer 告警，AAC analyzer 24/24，H.264 analyzer 174/174）；
     - hosted CI run `31938569503` 在 Ubuntu 24.04 / Qt 6.11.1（job `95144248440`）、macOS 15 / Qt 6.11.1（job `95144248392`）、Windows 2022 / Qt 6.10.1（job `95144248467`）全部 100% 成功通过。
  Next Action 指向 Task T18d（Profile 处理验证与全库路线图文档对齐）。
- 2026-08-16：完成 zh §5 补齐、全库测试行号引用校准与防漂移纪律建立（任务 T18c-2-fix2 / commit `4b8dd2c`）。
  1. 双语 ADR-0095 §5 完全对称：
     - 在中文版 §5 补齐第 3 条「未声明载荷区域的 ADTS 规则」能力边界记述，保证 EN/zh 均为 3 条。
  2. 测试行号全量校准：
     - 因新增用例位于 `:379-475`，全面校准 ADR-0095 §4 及 §6 中对 `tests/rules/aac_adts_analyzer_test.cpp` 的所有行号引用（Frame 0/1、截断用例、版本断言、ASC 用例 `:629-1865`、`comment_field_bytes` 7 处断言行号）；
     - 逐条通过 `sed -n` 校验，确认 `:144` 为唯一未发生位移的断言行号。
  3. 防复发纪律建立（ADR-0095 §7 及计划纪律第 6 条）：
     - 确立「默认尾部追加」与「同 Commit 行号复核」双重约束。
  Next Action 指向 Task T18d（Profile 处理验证与全库路线图文档对齐）。
- 2026-08-16：完成 Profile 处理验证、Phase 4 第 4 项能力边界闭环、路线图对齐与纪律条款集中归拢（任务 T18d / commit `835a5c3`）。
  1. Profile 处理现状实测与能力边界闭环（Phase 4 第 4 项勾选）：
     - 验证 ADTS 2-bit profile 字段（0=Main, 1=LC, 2=SSR, 3=LTP）全部合法物化（新增用例 `decodesAllStandardAdtsProfilesMainLcSsrLtp`，`tests/rules/aac_adts_analyzer_test.cpp:1916-1958`）；
     - 明确 ADTS 负向测试在 2-bit 语法层不可构造（`bits<2>` 值域 0..3 恰好被 `enum AacProfile` 全覆盖）；
     - 明确 ASC 非 GA AOT（HE-AAC SBR=5, PS=29 及 AAC-ELD=39）正常物化 GA 基线头部语法；
     - 明确判定 StreamView v0.1 不提供非致命 profile 不支持上报机制，Phase 4 第 4 项在此以「能力边界已记录且有测试钉住」正式闭环，未来非致命不支持诊断机制顺延至独立语言/内核切片。
  2. 路线图对齐与双语 ADR-0092 修订：
     - 在双语 ADR-0092 附录中追加说明：Task T17（ADR-0094）统一交付 ASC/PCE 并升级规则包至 `v0.1.2`；Task T18（ADR-0095）交付 `@lazy raw_data_block` 并升级至 `v0.1.3`。
  3. 全局纪律条款集中归拢：
     - 在实施计划头部「## 执行规则」下建立「### 纪律条款」小节，集中归拢 7 条全局执行规范；
     - 修正 T18c-2-fix2 记录中占位符，替换为真实 commit `4b8dd2c`。
  4. 测试与验证：
     - 新增测试用例按纪律第 6 条追加至类末尾（`:1916-1958`），前序所有 24 个用例行号零位移；
     - 本地 dev/ci/sanitize 35/35 全绿，AAC analyzer 25/25，H.264 analyzer 174/174。
  Next Action 指向 Task T18e（阶段 4 逐 bit 验收审计收尾与阶段推进）。
- 2026-08-16：完成 T18e-§0 前置修正（任务 T18e-§0 / commit `2925801`）。
  1. S1 ASC 非 GA AOT 实测覆盖追加：
     - 在 `tests/rules/aac_adts_analyzer_test.cpp` 末尾追加三个测试用例：`decodesAscNonGaAot5Sbr`（`:1960-2128`，AOT 5）、`decodesAscNonGaAot29ParametricStereo`（`:2130-2298`，AOT 29）、`decodesAscNonGaAot39EnhancedLowDelay`（`:2300-2467`，AOT 39）；
     - 全部断言 `DslExecutionStatus::Materialized`、`MaterializationState::Materialized`、零诊断、完整有序子节点名单及 `audio_object_type` / `audio_object_type_ext` 字段值；
     - AAC analyzer 测试用例数由 25 扩展至 28，全部 28/28 通过。
  2. S2 修正虚报表述与真实用例绑定：
     - 计划 :207 勾选注与 ADR-0095 §6 类别 5 证据列全量替换为真实测试用例引用，明确 AOT 5/29 布局与 AOT 2 相同的原因（`aac_asc.svfmt:2` 无约束且 GA 字段无条件解析）。
  3. S3 行号修正：全量将 `:1914-1958` 更正为 `:1916-1958`。
  4. S4 计划行号引用更新与纪律条款扩写：
     - ADR-0095 双语版更新 `implementation-plan.md:196-198` / `:198` $\to$ `:206-208` / `:208`；
     - 纪律第 7 条扩写为「任何使被引用文件行号发生位移的 commit（含 tests/ 与 docs/ 自身），提交前用 grep -n 全量复核所有引用文档中的行号，发现漂移即修正」。
  5. S5 / S6 ADR-0092 文件补齐末尾换行与 T18 正文描述对齐。
  6. 构建与 Hosted CI 验证：
     - 本地 dev/ci/sanitize 35/35 全绿（AAC 28/28，H.264 174/174）；
     - hosted CI run `31941684357` 在 Ubuntu 24.04 / Qt 6.11.1（job `95151697094`）、macOS 15 / Qt 6.11.1（job `95151697041`）、Windows 2022 / Qt 6.10.1（job `95151697136`）全部 100% 成功通过。
  Next Action 指向 Task T18e 主体（阶段 4 逐 bit 验收审计收尾与阶段推进）。
- 2026-08-16：完成 T18e 主体逐 bit 验收审计收尾与阶段 4 闭环（任务 T18e / commit `1b5a161`）。
  1. ADR-0095 §6 五类缺口全量关闭：
     - **类别 1（ADTS 头部）**：通过 `decodesAdtsHeaderBitByBitRangesAndZeroLengthPayload`（`:2473-2600`）补齐全部 18/19 个字段值（含下标 8–11 `original_copy`、`home`、`copyright_identification_bit`、`copyright_identification_start`）、bit 偏移与长度断言，以及零长度载荷帧（`raw_data_block` 状态为 Materialized、长度 0、零诊断）覆盖；
     - **类别 2（ASC/PCE）**：通过 `decodesAscFieldRangesRepresentativeSampling`（`:2606-2677`）以抽样方式覆盖 ASC 头部（`audio_object_type`、`sampling_frequency_index`、`channel_configuration`）、GASpecificConfig（`frame_length_flag`、`depends_on_core_coder`、`extension_flag`）与 PCE（`element_instance_tag`、`object_type`、`comment_field_bytes`）的位置与长度断言；
     - **类别 3（码流截断）**：通过 `verifiesTruncatedFramesLogicalRangesAndDiagnosticLocations`（`:2682-2740`）补齐 15 字节载荷截断与 8 字节含 CRC 头部截断的 `logicalRange()` 及诊断位置区间断言；
     - **类别 4（CRC 存在性/错误）**：通过 `:2473-2600` 显式断言 `crc_check` 位于 bit 56..71（16 bits，数值 `0x1234`），并通过 `:2682-2740` 验证截断位置；
     - **类别 5（不支持 Profile）**：已由 T18d / T18e-§0 闭环（`:1916-1958` 与 `:1960-2467`）。
  2. 阶段 4 验收与阶段推进：
     - 勾选实施计划 :206（`raw_data_block`）与 :208（逐 bit 验收），阶段 4 全部 5 项完成；
     - 阶段 4 正式关闭，`Current Phase` 推进至 5；
     - 阶段 5 第一步任务严格对齐设计规范：先编写 Phase 5 的 ADR 与架构设计，不直接编写 MP4 规则。
  3. 测试与验证：
     - 新增测试用例按纪律第 6 条追加至类末尾，AAC analyzer 测试用例数由 28 增至 31，全部 31/31 通过；
     - CTest 目标数因 §0b 引入 `markdown_hygiene` 目标由 35 增至 36，dev/ci/sanitize 三套构建全部 36/36 全绿。
  Next Action 指向 Phase 5 ADR 与架构设计编写。
- 2026-08-16：完成 T18f 阶段 4 收尾校正（任务 T18f / commit `f8f162a`）。
  1. F1 / F2 Markdown 卫生检查护栏修正（commit `04f15cd`）：
     - 在 `cmake/check_markdown_hygiene.cmake` 中对分号进行转义保护，消除分号导致列表分割进而引发的行号位移缺陷；
     - 给出真实等号证明（`ADR-0090:40` 插入制表符，护栏报出 `:40` 与 `awk` 实测 `:40` 严格相等）；
     - 扫描范围限制为 `git ls-files "*.md"` 并排除 `third_party/`。
  2. F3 / F4 测试断言与覆盖校正：
     - 在 `decodesAdtsHeaderBitByBitRangesAndZeroLengthPayload` 中对 Frame 2 补充 `raw_data_block` 的 `MaterializationState::Lazy` 状态断言（`:2600`）；
     - 对 Frame 3（100 字节含 CRC）补充 19 个完整子节点、`crc_check`（bit 56..71）、`raw_data_block`（bit 72..800，len 728，`MaterializationState::Lazy`）全量断言（`:2652`）。
  3. F5 验收表述与未抽样余量校正：
     - 计划 :206 验收注补入 Lazy 状态断言引用（`:2600` 与 `:2652`）；
     - ADR-0095 §6 类别 2 明确将 PCE 标量字段与声道循环列表列入显式未抽样余量声明；
     - 同 commit 执行纪律第 7 条行号复核，全量同步所有受位移影响的测试行号引用（`:2473-2655`、`:2661-2732`、`:2737-2795`）。
  4. 测试与 Hosted CI 验证：
     - dev/ci/sanitize 三套构建全部 36/36 全绿，AAC analyzer 31/31，H.264 analyzer 174/174。
  Next Action 指向 Phase 5 ADR 与架构设计编写。
- 2026-08-16：完成 Task P5a 双语 ADR-0096 架构设计与阶段 5 切片细化（任务 P5a / commit `f056e2f`）。
  1. 三项核心边界决策明确（D1/D2/D3）：
     - **D1（Box 遍历与 `mdat` Lazy 边界）**：核心库（`src/core/` 与 `src/rules/*.cpp`）保持 100% 格式中立，零 FourCC 字符串侵入；所有 Box 头部与层级关系由 DSL 结构表达，超大 `mdat` 载荷通过 `@lazy` 声明实现零堆内存惰性封装；`size == 0` 与 `size == 1`（largesize）通过 DSL 条件语法与剩余计算处理；
     - **D2（跨层导航引用模型）**：规则包在 v0.1 保持自包含独立，`org.streamview.mp4` 声明 `avcC`/`esds` 解码配置结构并通过注解提供目标格式元数据，会话层通过 `RulePackageStore` 实现跨包入口点链接与坐标映射；
     - **D3（样本表索引与资源预算）**：样本表元数据完整物化，海量重复索引数据（`stsz`/`stco`/`co64`）通过 `@lazy` 与窗口化批次控制，严格遵守单批次 1000 样本与全局 20,000 物化节点预算。
  2. 规范与纪律清理：
     - 新建双语 ADR-0096（`docs/adr/0096-*.md` 与 `docs/zh-CN/adr/0096-*.md`）；
     - 纪律条款追加第 8 条（历史记录 file:line 快照策略）；
     - ADR-0095 §7 补齐 Task T18f 条目并清理过期标记；
     - 细化阶段 5 执行切片（Task P5a..P5g），明确能力切片与规则切片分离。
  3. 卫生检查与验证：
     - 本地 `markdown_hygiene` 护栏验证通过；按 ADR-0019 Markdown-only 跳过 hosted CI。
  Next Action 指向 Task P5b（DSL 编译器未识别注解拦截加固）。
- 2026-08-16：完成 Task P5b-§0 ADR-0096 整改与阶段 5 切片序列重排（任务 P5b-§0 / commit `de7e5da`）。
  1. 引用与限制修正：
     - 将入口点解析引用更正为 `src/rules/include/streamview/rules/rule_catalog.h:52` 中的 `RulePackageCatalog::resolve`；
     - 明确区分编译期展开上限 `maximumExpandedFieldsPerStructure = 99'999`（`dsl_ir.cpp:13`）与运行期节点预算 `defaultMaximumMaterializedNodes() = 100'000`（`dsl_vm.h:36-38`）；
     - 将资源预算标注为设计目标，待 P5j 实测校准；
     - 更新 DSL 语法片段为实测可编译形式，并显式标注 `size == 0` 延伸至 EOF 为 P5c/ADR-0097 待决语言缺口。
  2. 切片重排：
     - 将阶段 5 任务切片由 P5a..P5g 展开细化为 P5b..P5j，前置语言能力探测与原语支持。
  3. 卫生检查与验证：
     - 本地 `markdown_hygiene` 护栏验证通过；按 ADR-0019 Markdown-only 跳过 hosted CI。
  Next Action 指向 Task P5b 主体（DSL 编译器未识别注解编译闸门实现）。
- 2026-08-16：完成 Task P5b DSL 未识别注解编译闸门加固（任务 P5b / commit `b1820bb`）。
  1. 统一注解注册表与编译闸门：
     - 在 `src/rules/dsl.cpp` 引入 `knownAnnotations` 静态注册表与 `validateAnnotations`，集中管理 11 个合法注解（`spec`、`description`、`equals`、`range`、`enum`、`lazy`、`index`、`context`、`context_export`、`context_import`、`context_dependency`）及 1 个预留注解（`target_format`）；
     - 严格拦截标量字段前置/后置、struct、enum、sequence、payload dispatch、entry 等 5 大宿主位置上的未识别注解，统一报错 `DslDiagnosticCode::InvalidAnnotation`（彻底消除 N2 `@equalss(4095)` 静默通过缺陷）；
     - 严格保持 computed、@lazy 区域、compressed_payload、sequence 等既有白名单宿主的允许集与原始诊断文案完全不变。
  2. 测试与回归断言：
     - 在 `tests/rules/dsl_test.cpp:2171-2241` 末尾追加 `rejectsUnrecognizedAnnotationsAndEnforcesHostWhitelist` 与 `verifiesExistingWhitelistedHostsPreserveAllowedSetsAndDiagnostics`；
     - 5 大宿主位置 `@bogus(1)` 拦截、N2 拼写错误拦截及既有白名单宿主回归用例全部实测通过；
     - 官方规则包 `org.streamview.h264` 与 `org.streamview.aac` 经 `svtool rule check` 静态校验零回归通过；
     - 根据纪律第 7 条同步复核并修正 ADR-0095 中因 `dsl.cpp` 变更引发的行号引用（`:2634`、`:2660`）。
  3. 验证与 Hosted CI：
     - 本地 3 套构建全绿（dev/ci/sanitize 36/36，全量通过且无 sanitizer 告警）；
     - Hosted CI run `31953480299`（Windows `95180435231`、Ubuntu `95180435295`、macOS `95180435300`）三平台全部通过。
  Next Action 指向 Task P5c（ISOBMFF 容器语言原语探测与 ADR-0097 编写）。
- 2026-08-17：完成阶段 3 历史完成声明纠偏（任务 T13-R）。
  1. H.264 声明范围与 manifest 对齐：
     - 官方包由 `0.1.38` 升级至 `0.1.39`，入口 profile 只声明实际支持的 `baseline`、`main`、`high`，删除未实现的 `extended`；
     - `loadsBundledRule` 回归锁定包版本、入口 profile 集合与 `baseline-main-high-slice-header` depth，防止 manifest 再次漂移；
     - 双语字段文档明确当前为 8-bit 4:2:0 有界子集，SP/SI、data partition、FMO 与 scaling list 仍为 unsupported，不再把“阶段完成”解释为 H.264 全规范覆盖。
  2. 字段级元数据闭环：
     - `h264_annex_b.svfmt` 为 NAL/AUD、SPS/VUI/HRD、PPS 与 slice 等全部 source-backed 官方声明补齐局部 `@spec` 与 `@description`；
     - H.264 analyzer 测试递归遍历实际 AST，要求每个 source-backed 字段自身携带规范引用与说明，避免依赖父 struct 元数据造成虚假覆盖。
     - 阶段 3 的验收措辞已收窄为“逐字段元数据 + 各语法族代表性合法/非法/span 用例”；现有测试并不声称每个字段各有一组独立正反例与 span 断言。
  3. 验证：
     - 官方 H.264 规则 `svtool rule check` 通过；H.264 analyzer 套件通过；本地 dev/ci/sanitize 全量 36/36 通过。
- 2026-08-17：完成阶段 4 历史完成声明纠偏（任务 T18-R）。
  1. 通用 Unsupported 语义：
     - DSL 新增格式中立语句 `unsupported("reason") at field;`，贯通 parser、typed IR、bytecode、VM 与 rule execution session；
     - 命中分支时保留已解码前缀，在指定字段位置产生 `UnsupportedSyntax`，结构状态为 `Unsupported`，且不读取或解释后续字段；未选分支保持正常 Materialized。
  2. AAC profile 行为修正：
     - 官方 AAC 包由 `0.1.3` 升级至 `0.1.4`；
     - AOT 5、29 与外层转义 AOT 31（包括 AOT 39）在公共 ASC 前缀后明确 Unsupported，不再把 profile 专属 SpecificConfig 位误解码成 GA/PCE；转义 AOT（`audio_object_type == 31`）整体判定为 Unsupported，不进行逐值细分；
     - 四个回归用例断言前缀字段值/位范围/状态、精确诊断锚点与后缀节点缺失，并删除原 `return` 后不可达的巨型旧断言块。
  3. 逐 bit 验收与 CRC 纠偏：
     - `decodesAscAndPceFieldsBitByBit` 使用紧凑 160-bit 向量覆盖全部可达 ASC/GASpecificConfig/PCE 源字段的值、逻辑范围、状态与空诊断；计算字段只验证值/状态；
     - CRC 验收严格限定为单 `raw_data_block` 的字段出现/省略、bit 56..71、载荷 bit 56/72 起点和字段截断。`0x1234` 仅为原样解码值，不代表 CRC 正确；CRC-16 算术与多 raw data block 需要独立 ADR 和格式中立完整性检查 API。
  4. 验证：
     - H.264 与 AAC 三份 bundled rule source 均通过 `svtool rule check`；
     - 本地 dev、ci、sanitize 完整构建与 CTest 均为 36/36，零 ASan/UBSan 报告；本补丁尚无对应 hosted matrix 结果。
  Next Action 保持 Task P5c（ISOBMFF 容器语言原语探测与 ADR-0097 编写）。
- 2026-08-17：完成 Task P5b-R 评审遗留整改与外部审计补丁收口（任务 P5b-R / commit `274a744` 与 `2624276`）。
  1. R1 恢复字段错误恢复守卫（`src/rules/dsl.cpp:1787-1791`）：
     - 恢复 `recoverField()` 中的 `!at(DslTokenKind::RightBrace)` 守卫，消除字段语法错误时误吞结构体闭合花括号引发的级联噪声；
     - 在 `tests/rules/dsl_test.cpp:2242` 追加用例 `recoversFieldSyntaxErrorWithoutDroppingClosingBrace`，锁定单条诊断。
  2. R4 循环体内 Unsupported 诊断去重（`src/rules/dsl_ir.cpp:28-36`）：
     - `addDiagnostic` 按 `(code, message, range)` 对诊断去重，消除 `repeat` 循环体展开时多次重复报出 `Unsupported statements cannot be repeat-local items` 的缺陷；
     - 在 `tests/rules/dsl_ir_test.cpp:3562` 追加用例 `deduplicatesUnsupportedDiagnosticsInsideRepeats`，锁定诊断条数由 3 条降为 1 条。
  3. R2 / R3 / R5 / R6 / R7 规范与 ADR 闭环：
     - R2：复核并修正 ADR-0094 双语版行号引用为 `src/rules/dsl.cpp:3394`；
     - R3：更新双语语言参考（`docs/format-language/README.md` 与 `docs/zh-CN/format-language/README.md`），明确注解编译报错契约、11 大注解 × 宿主白名单矩阵及 `target_format` 预留限制；
     - R5：新建双语 ADR-0098（`docs/adr/0098-*.md` 与 `docs/zh-CN/adr/0098-*.md`），系统沉淀未识别注解编译闸门与显式 `unsupported` 语法；
     - R6：在双语 ADR-0094 末尾追加 Amendment，澄清非 GA / 转义 AOT 不支持诊断行为；
     - R7：在双语 ADR-0095 §5.2、ADR-0098 与本计划中明确转义 AOT（`audio_object_type == 31`）一律统一判定为 Unsupported，不进行逐值细分；修正计划头部 commit 引用至 `de7e5da`。
  4. 构建、验证与 Hosted CI：
     - 官方 H.264 与 AAC 三份 bundled rule 均通过 `svtool rule check`（全量 `Rule OK`）；
     - 本地 dev/ci/sanitize 三套构建全部 36/36 全绿（零 ASan/UBSan 告警）；
     - 连同外部审计 4 个 commit 共同推送后，Hosted CI run `31965623068` 在 macOS 15（job `95210238754`）、Windows 2022（job `95210238781`）、Ubuntu 24.04（job `95210238826`）三平台全部 100% 成功通过。
  Next Action 保持 Task P5c（ISOBMFF 容器语言原语探测与 ADR-0097 编写）。

- 2026-08-17：完成 Task P5b-R2 复审遗留整改（commit `a41d2bc` 与 `7671916`）。
  1. F1（本任务唯一代码决策，方案①改代码）：`markFailure` 仅当 `DslExecutionStatus::Unsupported` 时产出 `DiagnosticSeverity::Warning`，其余失败状态保持 Error；
     - 改动前（红）：4 条 severity 断言全部 FAIL，`Actual (severity): 2 (Error) / Expected: 1 (Warning)`；
     - 改动后（绿）：`streamview_aac_adts_analyzer_tests` 31/31 全过；severity 由 `reportsAscEscapedAudioObjectTypeAsUnsupported`（:997）、`reportsAscNonGaAot5SbrAsUnsupported`（:1902）、`reportsAscNonGaAot29ParametricStereoAsUnsupported`（:1972）、`reportsAscNonGaAot39EnhancedLowDelayAsUnsupported`（:2043）锁定为 Warning；
  2. F2–F8 与 ADR-0098 补充（docs commit `7671916`）：
     - F2：ADR-0095 §5.2 双语改引实际字符串 `unsupported("Escaped AAC object types require a profile-specific SpecificConfig")`；
     - F3：删除 ADR-0098 双语「Gemini Rule 5.1」虚构引用，改为「格式语义只能进 DSL/规则层」全局约束；
     - F4：验证矩阵改真实测试符号与现场行号（`dsl_test.cpp:2171`、`aac_adts_analyzer_test.cpp:959/1866/1936/2006`），删除不可复跑的 `scratch/probe_annotation_gate` 与越界区间 `:1960-2467`；
     - F5：ADR-0094 双语行号 `dsl.cpp:3394→3395`、`dsl_ir.cpp:1557-1561→1564-1569`；ADR-0095 P7/P8 行号 `2676→2677`、`2702→2703`、`dsl_ir.cpp:173→180`、`185→192`；ADR-0086/0090/0092 引用按纪律第 8 条作为历史快照保留；
     - F6：活跃状态头回写 P5b-R / P5b-R2 实测与 hosted 结果；
     - F7：ADR-0098 双语 Status 统一为 Proposed；
     - F8：中文语言参考两行超长行（原 :502=942、:503=522 字符）按既有宽度重排，还原 `该 repeat 中嵌套`；
     - 补充：ADR-0098 明确 `addDiagnostic` 去重为编译器全局行为（dsl_ir.cpp 内 201 个调用点），并以 `struct S { bits<4> n; repeat (n, 3) { bits<4> e @equals(99); } }` 实测（诊断 3 条→1 条）写入文档；
  3. 验证与 Hosted CI：
     - 本地 dev/ci/sanitize 三套 36/36 全绿（dev 50.16s、ci 16.49s、sanitize 131.35s，零 sanitizer 告警）；定向套件 dsl 87 / dsl_ir 88 / AAC 31 / H.264 174；三份 bundled `.svfmt` 全部 `Rule OK`；
     - push 后 Hosted CI run `31969610307` 在 macOS 15（job `95219881189`）、Ubuntu 24.04（job `95219881227`）、Windows 2022（job `95219881244`）三平台全部成功。
  Next Action 保持 Task P5c（ISOBMFF 容器语言原语探测与 ADR-0097 编写）。

- 2026-08-17：完成 Task P5c —— MP4/ISOBMFF 容器原语可表达性探测与双语 ADR-0097（Markdown-only + scratch 探针，commit `4bd1e28`）。
  1. 探测结论（11 个 scratch 探针全部 `svtool rule check` 实跑，均符合预期）：
     - Q① 顶层 box 枚举：`scan(mp4_box)` → `error: Only h264_start_code and adts_frame are supported`（parser `dsl.cpp:3568` / IR `dsl_ir.cpp:3679`）——需新增 `DslScannerKind::Mp4Box`（只做分帧、不认任何 FourCC，与 AacAdtsScanner 只认 syncword/长度链同构）；
     - Q② 嵌套下钻：结构体类型字段 / struct 内 sequence / `payload<mp4>` / `@container` 四条探针全部报错（`Expected bits<N...>` ×2、`The only accepted payload view kind is rbsp`（dsl.cpp:3669）、`Unknown annotation '@container'`（dsl.cpp:1880））——选定最小机制 `@container` + runner 重入（容器语义留 DSL，核心零 FourCC）；
     - Q③ type 标签：fact 10 复确认，ADR-0097 附 40 个 FourCC 常量表；`uuid` 扩展类型以 `@equals(0x75756964)` + 不透明 `bits<128> usertype` 收口（ADR-0096 D1 §5 遗留问题）；
     - Q④ size 分支：`size == 1`/`else` 用 ADR-0096 D1 既有分支内 `computed`+`@lazy` 形态（复确认唯一可行写法）；`size == 0` 选定新增 `available_bytes()` 内建（否决 scanner 计算剩余长度下传）；
     - Q⑤ `@target_format`：后置位被 lazy 白名单拦（`dsl.cpp:1895`）、前置位被拒（`Expected bytes after @lazy(...)`，`dsl.cpp:1112`）——设计为注册 `{target_format, LazyRegion}`、仅后置位，注册表改动落在 P5d-2；
     - Q⑥ 样本表窗口化：`maximumExpandedFieldsPerStructure = 99'999`（dsl_ir.cpp:13）为编译期硬上限，设计 lazy 区域 + 按需窗口解码 + source coordinate 回映 + UI 翻页契约（非可选优化）；
     - Q⑦ moof：三条禁令实测（`dsl_ir.cpp:2741/2748/2771`），box 头后 `unsupported(...) at type;` 形态 `Rule OK`——选定规则内 unsupported（保留已解码前缀、继续后续顶层扫描），否决探测器整份拒绝；
     - Q⑧ 工具链：ffprobe 8.1 + ffmpeg 可用、MP4Box 缺失；ffmpeg 生成普通/分片 fixture，`ffprobe -v trace` 取 box 层级 ground truth（ftyp/free/mdat/moov → mvhd/trak×2 → tkhd/edts/mdia → mdhd/hdlr/minf → vmhd/smhd/dinf/stbl → stsd(avc1+avcC/pasp/btrt, mp4a+esds/btrt)/stts/stss/stsc/stsz/stco；分片：moov(+mvex/trex) → moof→mfhd/traf→tfhd/trun → mdat），`-show_packets` 取 sample offset/时间戳/关键帧（视频帧 1：pos 306 关键帧；帧 2：pos 3282）；
  2. 产出双语 ADR-0097（D1–D7 决策 + 被否决方案 + 影响 + 验证矩阵 + 参考），EN/ZH 结构对称（16 标题 / 42 表格行，行号一一对应）。
  3. 验证：`markdown_hygiene`（CTest #36）通过；双语对称性检查通过；P5c 为 Markdown-only，按 ADR-0019 跳过 hosted CI，本轮无新 run。
  Next Action 指向 Task P5d（能力实现切片）。

- 2026-08-17：完成 Task P5c-R —— ADR-0097 可实施性与证据闭环整改（Markdown-only + scratch 探针，docs commit，ADR 修正 `docs: correct mp4 primitive implementation contracts` + 本记录 commit）。本次更正覆盖原 P5c 记录中的以下错误结论（旧记录保留为历史，以本记录为准）：
  1. 修正 UUID（原 P5c 的 `bits<128> usertype` 非法）：新探针 P8b 实测 `bits<128>` → `error: Bit field width must be in the range 1..64`（`dsl.cpp:1038`）；锁定形态为两个 `bits<64>` 字段（`usertype_hi`/`usertype_lo`），探针 P8a `Rule OK`；明确 v0.1 只透明保存 128 位值，不支持整 UUID 字面量 equality（fact 10）；
  2. 锁定 Mp4Box framing/candidate 合同：合法尺寸集（`size==0` 终结、`size==1 && largesize>=16`、`size>=8`；2..7 与 `largesize<16` 畸形）、头长（普通 8 / large 16 / uuid 另加 16 usertype）、截断头/跨度越界/128 位受检加法/size 小于头长/区域末尾/`size==0` terminal 行为全定义；单 box 合法性与 detector Strong（≥3 连续头）阈值分述；
  3. 修正 scanner/DSL 数据合同：采用 ADTS 同构 span-only 合同（scanner 输出 box span，runner 映射，Box DSL 从源重读 size/type/largesize），撤回原 P5c「scanner publishes header values」错误陈述（值下传需完整 typed scan/value schema，列入被否决方案 7）；
  4. 完整定义 `@container`：锁定后置 `@container(ChildStruct)`（一个 struct identifier 参数、仅 LazyRegion 宿主、arity/kind/存在性诊断），parser AST（dsl.cpp:782-810 通用注解解析）、typed IR（`DslLazyRegion::containerChildStructIndex`）、runner 重入、子映射边界/递归-环策略（递减跨度 + 深度 16 + 节点预算 100'000）/取消/截断/子错误传播；核心/runner 零 FourCC 字面量；探针 P8c 证明名称闸门先行、参数解析为 Identifier（dsl.cpp:801）；
  5. 完整定义 `@target_format`：后置 `@target_format("video/mp4")`、一个 string 参数、仅 LazyRegion；knownAnnotations（dsl.cpp:1834-1866）预留项 `{u"target_format", 0U}`（dsl.cpp:1865）改为 LazyRegion、白名单消息（dsl.cpp:1895）同步放开；typed IR `DslLazyRegion::targetFormat`、FieldMetadata/analysis tree 传播、session/cache 保留（analysis_cache_payload 节点元数据）、AnalysisSession/UI 消费（RulePackageCatalog::resolve，rule_catalog.h:52）；P5d 实现点与正反测试矩阵齐备；
  6. 修正窗口化设计：通用 lazy-region window decoder（分页 + 坐标回映 `container_anchor + index * entry_size`）纳入显式能力切片（P5d-3 子项 / 独立 P5d-4），不假定 P5g 规则自研；硬性编译要求收窄为「产品默认窗口化，小表可 bounded repeat」；
  7. 修正分片 MP4 证据：P7d 只证明 unsupported 形态可编译；moof 节点 Unsupported/Warning、此前节点保留、后续 mdat 续扫三点标为 P5d runner 规范性合同与必测行为（测试向量 /tmp/p5c_frag.mp4，ffprobe trace 摘录 moof→mfhd/traf→tfhd/trun → 后续 mdat）；
  8. 补齐证据：14 探针全部给出完整命令与当次输出；fixture 生成命令写全（普通 6513 字节 / 分片 4505 字节）；ffprobe 输出经 grep 的明确标注为「相关摘录」，覆盖 smhd、音频 btrt、mvex、trex 节点；ADR 的验证矩阵声明与报告事实一致；
  9. 同步修正：knownAnnotations 引用范围改为现场真实 `dsl.cpp:1834-1866`（原 1836-1865）；「seven facts / facts 10–16」计数统一（七条容器事实）；行号全部现场 `grep -n` 取（含 `dsl.cpp:3556-3564` sequence 元素类型查找、`dsl.cpp:3577-3580` @index(progressive) 消费）；EN/ZH 语义与结构对称（16 标题 / 44 表格行，探针 token 与引用行号计数 EN=ZH）。
  验证：14 个 scratch 探针全部 `svtool rule check` 实跑，输出与 ADR 矩阵逐字一致（P7d/P8a `Rule OK`，其余错误消息原文匹配）；`markdown_hygiene`（CTest #36）通过；`git diff --check` 干净；P5c-R 为 Markdown-only，按 ADR-0019 跳过 hosted CI，本轮无新 run（最新 hosted 基线仍为 P5b-R2 run 31969610307）。
  Next Action 保持「主 Agent 复审 P5c-R，未经复审不得开始 P5d」。

- 2026-08-17：完成 Task P5c-R2 —— ADR-0097 实现映射与证据真实性整改（Markdown-only + scratch 探针，docs commit，ADR 修正 + 本记录 commit）。本次更正覆盖 P5c-R 记录中的以下错误结论（旧记录保留为历史，以本记录为准）：
  1. 修正 D1 分帧算术：`size`/`largesize` 是整个 box（含头）的尺寸，span 边界用减法式 `box_size <= region_end - start` 且 `span_end = start + box_size`（头长只用于最小值与载荷锚点）；删除「checked 128-bit」表述；定义 `size==1` 仅剩 8..15 字节为不完整 large 头（停止、不计候选）；明确畸形 size 后 scanner 停止不重同步（MP4 无重同步标记，与 ADTS 尾部垃圾先例刻意分歧）；区分 scanner 通用 8/16 最小值与 DSL 的 uuid 24/32 语义约束；
  2. 补全 UUID 规则形态：scanner 只做通用 8/16、不识别 uuid；24/32 由 DSL 断言 `type @equals(0x75756964)` 后读 16 字节 usertype 实现；写出 normal（payload=size-24）、large（payload=largesize-32）、size==0（读 usertype 后 available_bytes()）三形态——新探针 P8d（完整 size 分支，Rule OK）、P8e（size==0 缺口，`error: Pure function is not declared before this call`，dsl_ir.cpp:772）；
  3. 修正 scanner/ADTS 表述：只保留「span-only + DSL 重读 header」，删除「AacAdtsRecord 形态完全一致」（其另有 headerLength/payloadLength/aacFrameLength/crcPresent），只引 AAC analyzer 用 frameSpan 建 mapping 的先例（aac_adts_analyzer.cpp:309/:346）；
  4. 重写 @container 实现映射：AST `DslLazyRegion`（dsl.h:234-240）保留注解（不新增字段）；typed IR 落 `DslTypedField`（dsl_ir.h:108-124）新增 `containerChildStructIndex`（仅 LazyBytes 有效），删除「DslLazyRegion 加字段」错误；compileLazyRegion（dsl_ir.cpp:2462-2540）引用解析 + addDiagnostic（:28-41）去重 + struct index 存储；状态矩阵锁定真实 `AnalysisTree::canTransition`（analysis_model.cpp:218-251）Lazy→Indexing→Materialized/Invalid/Cancelled/WaitingDependency/Cancelled→Indexing；预算锁定 `DslExecutionLimits`（dsl_vm.h:30-51）确定值（node depth 256 / nodes 100'000 / instructions 1'000'000 / view 64 / call 64 / cancel 1'024）；多 span 映射经逻辑子视图 + `SourceMapping::locate`（dsl_vm.cpp:3258-3262 先例）明确支持，不限制单连续 span；
  5. 重写 @target_format 消费合同：typed 字段落 `DslTypedField.targetFormat`（仅 LazyBytes）；`AnalysisNodeMetadata`（analysis_model.h:76-80）新增 `optional<QString> targetFormat`；缓存新增 `nodeTargetFormatFlag = 4U`（analysis_cache_payload.cpp:35-36 之后空位），encode/decode（:682-683 置位、:691-698 写、:833/:849 读）、旧缓存缺 flag ⇒ nullopt 兼容；`"video/mp4"` 不直接调 `RulePackageCatalog::resolve`（真实签名需 identity+entryPointId+language+engine，rule_catalog.h:52-53）——二选一选案 1：新增 `resolveByFormat` 服务（按 `RulePackageEntryPoint::format`（rule_package.h:61-69）扫描，Found/MissingContent/VersionConflict 三态）；P5d-2 实现、P5i 消费；正反 + 解析三态测试矩阵；
  6. 窗口解码器真实能力：锁定规则侧（entry struct + entry_count + 编译期 entry_size）+ `WindowDecodeRequest`/`WindowDecodeResult`（DslExecutionStatus 五态）+ 固定默认 pageSize=256 + 坐标从 lazy logical range + checked offset 经 SourceMapping::locate（不假设单一 absolute anchor）；切片定为 P5d-3 子项（唯一，删除「P5d-4 或」）；P5d 在 `src/rules/include/streamview/rules/window_decoder.h` 落地；
  7. 证据闭环：D7 测试增至四断言（moof 节点 Unsupported/Warning + moof 前缀 size/type 保留物化 + 此前节点保留 + 后续 mdat 续扫）；fixture 定为生成脚本（ffmpeg 命令提交 tests/fixtures/，测试不引用 /tmp 路径）；修正英文 D7「Rejected Alternatives 9」→ 6；被否决方案补第 10 条（@target_format 携带完整包身份被否决）；
  8. 全量行号现场核对：本轮修正 `dsl_ir.h:85-108→108-124`、`dsl_vm.h:17-31→30-51`、`dsl_ir.cpp:2462-2537→2462-2540`、`dsl_vm.cpp:3169-3262→3169-3320`、`analysis_model.cpp:218-245→218-251`；其余引用逐一 grep -n 确认。
  验证：16 个 scratch 探针全部 `svtool rule check` 实跑，stdout/stderr/exit code 与 ADR 矩阵逐字一致（P7d/P8a/P8d `Rule OK` exit=0，其余 exit=1 错误消息原文匹配）；`markdown_hygiene`（CTest #36）通过；`git diff --check` 干净；EN/ZH 语义与结构对称（16 标题 / 59 表格行 / 16 代码围栏，16 探针 token 与 56 个符号/行号计数 EN=ZH 全等）；P5c-R2 为 Markdown-only，按 ADR-0019 跳过 hosted CI，本轮无新 run（最新 hosted 基线仍为 P5b-R2 run 31969610307）。
  Next Action 保持「主 Agent 复审 P5c-R2，未经复审不得开始 P5d」。

- 2026-08-17：完成 Task P5c-R3 —— ADR-0097 分支线序、窗口绑定与证据终审整改（Markdown-only + scratch 探针，commit `8679565` 与本记录 commit）。本次更正覆盖原 P5c-R2 记录中的以下错误结论（旧记录保留为历史，以本记录为准）：
  1. 修正 D1 size==0 分支与分帧算术：
     - `size == 0` 独立前置处理为 `span = [start, region_end)`、`terminal = true`、`truncated = false`，不再套用 `span_end = start + box_size` 导致零长度 span 错误；
     - 仅 `size != 0` 时进入 `box_size <= remaining` 减法式跨度算术；
     - 明确字节到比特坐标保护（`offset`/`length` 不得超过 `quint64 max / 8`，`SourceSpan::create` 失败产生确定性 `InvalidSourceSpan` 诊断）；
     - 明确截断记录（`truncated = true`）可由分析器物化（附带 `TruncatedSource` 诊断），但不计为候选探测器 `Strong` 链的完整合法节点；
  2. 修正普通 Box 与 UUID 的三路分支与线序：
     - 普通 box 采用互斥三路分支，`size == 0` 分支走 `available_bytes()`，绝不执行 `size - 8` 无符号减法下溢；
     - UUID 按照 ISO/IEC 14496-12 规范线序排列：`size == 1` 时按 `size → type → largesize → usertype_hi → usertype_lo → large_data` 读取，`size != 1` 时按 `size → type → usertype_hi → usertype_lo → payload` 读取；`usertype` 绝不在判断 `size == 1` 之前统一读取；
     - 探针 P8d 证实正确线序语法可编译（`Rule OK`），P8e 证实 `available_bytes()` 仍为唯一缺失内建；
  3. 为样本表窗口解码定义独立 `@window` 规则绑定：
     - 彻底解耦 `@container` 与窗口解码：`@container(Child)` 专用于 `Mp4Box` scanner 重入，固定宽度样本表使用专有 `@window(EntryStruct, entry_count)` 注解；
     - 锁定宿主为 `LazyRegion`、两个 `Identifier` 参数（表项结构体与计数字段）；
     - 映射到 typed IR 独立字段（`windowEntryStructIndex`、`windowEntryCountFieldIndex`、`windowEntrySizeBits`）与节点元数据；
     - 新增探针 P9a/P9b 证实 `@window` 注解名称闸门与参数校验行为；
     - `WindowDecodeRequest` 直接消费该独立元数据，不读取 `containerChildStructIndex`；
  4. 恢复 P5d 任务文档切片：
     - P5d-1：`Mp4Box` scanner + 探测器（`detectMp4Candidate`）；
     - P5d-2：语言/编译器/IR 增量（`@container`、`@window`、`available_bytes()`、`@target_format`、元数据、缓存、`RulePackageCatalog::resolveByFormat`）；
     - P5d-3：`Mp4IsobmffAnalyzer` runner 骨架 + 容器重入 + 共享执行预算 + 窗口解码器（`window_decoder.h`） + D7 续扫/fixture 测试；
  5. 补实 D2 runner 共享执行预算与执行职责：
     - 明确 runner 维护跨重入共享执行状态（`totalNodesMaterialized` 累计计数器、`currentNestingDepth` 深度跟踪、共享 `cancellationToken`）；
     - 明确 `RegisterLazyBytes` 在 VM 执行中仅注册 lazy 节点并存储元数据，不立即执行重入；重入由 runner / session 按需驱动；
  6. 诚实证据与验证：
     - 18 项探针（P1–P9b）全部实跑验证，输出与 ADR 矩阵逐字一致；
     - fixture 生成命令（普通 6513 字节 / 分片 4505 字节，exit=0）与 `ffprobe` 摘录完整附录；
     - `markdown_hygiene`（CTest #36）通过；`git diff --check` 干净；双语 ADR-0097 对称性检查通过（16 标题 / 40 表格行 / 18 代码围栏完全一致）；P5c-R3 为 Markdown-only，按 ADR-0019 跳过 hosted CI。
  Next Action 保持「主 Agent 复审 P5c-R3；未经复审不得开始 P5d」。

- 2026-08-17：完成 Task P5c-R4 —— ADR-0097 合同终态与验证措辞收口（Markdown-only + scratch 探针，commit `ce855d3` 与本记录 commit）。本次收口覆盖原 P5c-R3 记录中的未闭合项与证据措辞：
  1. D1 坐标防护、真实诊断码与探测器分级：
     - 增加字节到比特坐标越界防护：`start`、`length` 与 `start + length` 均受受检加法保护（不超过 `quint64 max / 8`）；
     - `SourceSpan::create` 失败统一使用现有 `DiagnosticCode::SourceError` 与 `DiagnosticSeverity::Error`，节点终态为 `MaterializationState::Invalid`，彻底删除虚构的 `InvalidSourceSpan`；
     - 明确探测器 `detectMp4Candidate` 仅对完整、未截断、格式良好的 box 计数（`Weak=1`、`Probable=2`、`Strong>=3`）；截断记录不计入任何等级，且尾部截断不降低此前完整链等级（3 完整 + 1 截断仍为 `Strong`；首 box 截断返回 `std::nullopt`）；
  2. D2 完整状态转移矩阵与全量共享执行预算：
     - 恢复完整 ASCII 状态转移表，逐项对齐 `AnalysisTree::canTransition`（`analysis_model.cpp:218-251`），消除损坏格式；
     - 定义 `RunnerExecutionBudget` 包含 `remainingNodes`（初始 100'000）与 `remainingInstructions`（初始 1'000'000）双累计扣减余额，以及 `currentNestingDepth`（受 `maximumNodeDepth = 256` 约束）与共享 `cancellation` 标记；
     - 每次调用 VM `execute` 前将单次上限收窄为共享余额，返回后无论状态如何均扣除 `nodesCreated` 与 `instructionsExecuted`；`viewDepth` 与 `callDepth` 明确为单次 VM 栈深上限而非累计预算；
     - 明确 `RegisterLazyBytes` 仅注册元数据，重入由 session 拥有的 `Mp4IsobmffAnalyzer` 驱动；
  3. D3/D4 UUID 最小尺寸与减法下溢诊断语义：
     - 明确 Large UUID（`size == 1`）要求 `largesize >= 32`，Normal UUID（`size != 1 && size != 0`）要求 `size >= 24`；
     - 明确畸形 UUID 流在 `largesize - 32` 或 `size - 24` 时通过 VM 受检无符号减法下溢产生确定性的 `DslExecutionStatus::InvalidSyntax` 与 `"Unsigned subtraction underflow in computed field"` 诊断；
     - 探针 P8d 证实正确分支与线序可编译，探针 P8e 明确为“首先暴露 `available_bytes` 内建缺失”；
  4. D6 @window 完整闭合设计：
     - 锁定 `EntryStruct`（v0.1）仅允许无条件、源 backing 的静态 `bits<N[, endian]>` 标量字段与静态 `bits<N>` 固定长度数组；禁止动态位宽、`ue`/`se`/`ff_coded`、`if`/`else`、`repeat`/`until`、`computed`、`@lazy`、`compressed_payload`、`unsupported` 或嵌套结构体；`entrySizeBits` 必须 `> 0`、字节对齐且受防溢出求和校验；
     - 计数字段必须先于 lazy 区域声明且为无条件无符号标量；`RegisterLazyBytes` 快照其值存入节点元数据；
     - 明确 `WindowDecoder` 为 session 拥有的对象（持有 `DslProgram`、`Source`、`SourceMapping`、`AnalysisTree`、`containerNodeId` 与 `RunnerExecutionBudget`）；
     - 列出 `DslExecutionStatus` 完整 9 态矩阵并指明窗口操作返回子集；
     - 包含 `pageIndex * pageSize`、`entryIndex * entrySizeBits` 与 lazy 边界校验；
  5. 证据与验证规范化：
     - P9a/P9b 明确仅证实未知注解名称闸门（`Unknown annotation '@window'`），参数类型校验留待 P5d-2 注册后单元测试；
     - 探针驱动脚本 `verify_all_18_probes.py` 增加失败非零退出机制，18 项探针全量跑通；
     - `markdown_hygiene`（CTest #36）通过；双语 ADR-0097 对称性检查通过（505 行完全对齐）；`git diff --check 5fbdfed...HEAD` 干净；Markdown-only 按 ADR-0019 跳过 hosted CI。
  Next Action 保持「主 Agent 复审 P5c-R4；未经复审不得开始 P5d」。

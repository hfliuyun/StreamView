# StreamView v0.1 分阶段实施计划

Status: In Progress
Current Phase: 3
Last Completed Step: Admit the reserved external leaves in computed field initializers
Next Action: Decide how a bounded pred_weight_table() selects its effective entry count, then implement it; the language currently cannot express the choice between the locally overridden count and the imported PPS default, and the restriction that would avoid the choice has no legal assertion position
Last Verification: Commits 8d54240 and 3cfef2b; local dev/ci/sanitize each passed 32/32; H.264 analyzer passed 94/94; svtool rule check passed; hosted run 31258216794 passed on Windows, macOS, and Ubuntu
Blockers: None

本文件是实施与恢复入口。英文产品需求、DSL 规范和 ADR 仍是权威设计来源。

## 执行规则

- 每个阶段及步骤使用复选框记录状态。
- 每完成一个阶段，记录验证命令、结果摘要和对应 commit。
- 中断前更新 `Current Phase`、`Last Completed Step`、`Next Action` 和 `Blockers`。
- 发现计划外决策时先暂停，实现前补充 ADR 和中英文文档。
- 不删除已完成记录；需求变化通过追加修订记录追踪。
- 每次 push 前必须先完成本机 Debug、Release、ASan/UBSan 构建与全部测试；CI 专属平台差异仍需由 hosted matrix 验证。

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

- [ ] H.264：EBSP/RBSP、trailing bits、SPS/PPS/VUI/HRD、slice header、SEI 与按位置重定义。
- [ ] AAC-LC：ADTS、AudioSpecificConfig/GASpecificConfig/PCE，压缩 payload 保持 opaque。
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
- [ ] 完成 Baseline/Main/High 8-bit 4:2:0 slice header；slice data 标记为压缩载荷。
- [ ] 所有 SEI 解析 payloadType/payloadSize。
- [ ] 深入解析 buffering period、pic timing、用户数据、recovery point、frame packing 和 display orientation。
- [ ] 支持同 ID SPS/PPS 中途重定义和按位置选择。
- [ ] 为声明范围内每个字段建立规范引用、双语说明、合法/非法样例和 source-span 断言。

## 阶段 4：AAC-LC 正式结构支持

- [ ] 解析 ADTS fixed/variable header、frame length、buffer fullness、raw block count 和 CRC。
- [ ] 解析 AudioSpecificConfig、GASpecificConfig 和 Program Config Element。
- [ ] 将 `raw_data_block` 整体标记为压缩载荷，不隐藏实现 Huffman 解码。
- [ ] 对 HE-AAC、ELD 和其他 profile 明确报告部分识别或不支持。
- [ ] 验证 ADTS、ASC、截断、CRC 错误和不支持 profile 的逐 bit 结果。

## 阶段 5：非分片 MP4/MOV 与跨层导航

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

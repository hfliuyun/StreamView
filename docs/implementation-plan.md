# StreamView v0.1 分阶段实施计划

Status: In Progress
Current Phase: 1
Last Completed Step: Phase 1 runner/CLI plus the M2 file-session, paged raw-data GUI, bounded Annex B detection, and unified source-bit selection slices completed locally
Next Action: Publish incremental analysis-tree updates and implement the field inspector
Last Verification: Local Debug/Release/ASan/UBSan configure, full build, and CTest each passed 20/20
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
- DSL 使用手写 lexer、递归下降声明解析器、Pratt 表达式解析器、静态类型 IR 和受限 bytecode VM。
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
- [ ] 实现 `QAbstractItemModel` 分页分析树和字段检查器，展示值、宽度、逻辑/绝对坐标、说明、规范引用和诊断。
- [x] 实现内部 `svtool rule check` 与 `svtool analyze`，输出与 GUI 使用同一 runner；固定用法、诊断和退出码。
- [x] 增加最小 UI/CLI 回归样例，并验证合法、截断和无 start code 输入。

验收：从一个本地 Annex B 文件可完成打开、树/原始视图查看、字段与 bit 双向选择，以及 CLI 文本分析；全部行为不修改源文件。

### M3：DSL v0.1 执行骨架

依赖：M1/M2 暴露出的稳定 rule-runner 接口；先定义类型/IR/预算边界，再逐项扩展语法。

- [ ] 建立静态类型 IR 与受限 bytecode/VM 边界，统一错误、资源预算和确定性。
- [ ] 逐项加入 enum、显式 endian、`ue/se`、数组、条件、switch、有界循环、纯函数和 computed fields；每项先补英文规范、中文说明、正反例和 TDD。
- [ ] 固化调用/视图深度 64、节点深度 256、单次物化 100,000 节点和每 1,024 指令取消检查。

验收：稳定子集在三平台生成相同 IR/结果，超限与取消保留部分树且诊断可定位。

### M4：映射、lazy 与大型文件底座

依赖：M3 的运行时边界。

- [ ] 实现 mapping-preserving EBSP→RBSP、excluded span、lazy boundary 和可恢复 progressive index。
- [ ] 实现位置感知上下文目录，支持按源位置选择最近有效 SPS/PPS/ASC/sample description。
- [ ] 以稀疏/虚拟 100 GB 源验证初始打开、已知 offset 读取、批次发布、取消和恢复；再接 SQLite WAL 分页缓存。

验收：内存不随源大小线性增长，跨排除字节的字段仍能返回多个 source spans。

### M5：规则分发、身份与持久化

依赖：M3 的语言/引擎版本契约和 M4 的稳定 source/rule identity。

- [ ] 先固定 TOML manifest、content hash、兼容范围和 rule catalog，再实现目录导入与 `.svrule` deterministic ZIP。
- [ ] 拒绝绝对路径、parent traversal、重复/非规范路径、符号链接和 zip bomb；安装内容按 hash 只读保存。
- [ ] 统一 source fingerprint、SQLite cache namespace 与 `.svsession` 精确规则 pinning，使用版本化 JSON/QSaveFile 原子保存。

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
- [ ] 完成文件打开、格式检测、分析树、hex/binary 视图、字段检查器和双向 bit 选择。（文件打开、候选检测、首个 raw/tree 视图和双向 bit 选择已完成；增量树发布与字段检查器待补。）
- [x] 提供内部 `svtool rule check` 与 `svtool analyze`。
- [x] 验证合法和截断 H.264 均能显示精确字段与部分结果。

## 阶段 2：DSL v0.1、沙箱与大型文件基础设施

- [ ] 完成枚举、显式大小端、`ue/se`、数组、条件、switch、有界循环、纯函数和计算字段。
- [ ] 完成 mapped transformation、lazy 区域、渐进索引和位置感知上下文目录。
- [ ] 完成 bytecode 预算和取消：调用/视图深度 64、节点深度 256、单次物化 100,000 节点，每 1,024 指令检查取消。
- [ ] 完成 SQLite 分页缓存、后台批次提交、schema 版本和崩溃恢复。
- [ ] 完成 TOML 清单、本地规则目录导入、`.svrule` 打包安装、哈希和版本并存。
- [ ] 防御路径穿越、zip bomb、符号链接和 Unicode 非规范路径。
- [ ] 为全部稳定 DSL 功能补齐英文规范、中文说明和正反例。
- [ ] 验证 100 GB 虚拟/稀疏源可快速打开、渐进索引、取消和恢复。

## 阶段 3：H.264 正式结构支持

- [ ] 完成 Annex B、EBSP→RBSP、trailing bits、SPS、PPS、VUI/HRD。
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

- [ ] 使用版本化 JSON 与 `QSaveFile` 实现 `.svsession` 原子保存。
- [ ] 保存源身份、规则精确版本/哈希、书签、注释、展开路径和视图状态。
- [ ] 大文件指纹使用大小、纳秒 mtime、首/中/尾各 1 MiB SHA-256；小文件全文哈希。
- [ ] 实现保存/另存为、未保存关闭提示、格式手动覆盖和规则版本管理。
- [ ] 完成进度、取消、诊断汇总、明暗主题和中英双语切换。
- [ ] 验证源变化不会误绑定，旧会话继续使用锁定的旧规则。

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

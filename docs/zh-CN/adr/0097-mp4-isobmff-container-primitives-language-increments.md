# ADR-0097：MP4/ISOBMFF 容器原语可表达性探测与语言增量

- **状态**：Proposed
- **日期**：2026-08-17
- **作者**：StreamView 贡献者

---

## 背景

阶段 5 要求支持非分片 MP4/ISOBMFF 容器解析（`video/mp4`、`audio/mp4`，ISO/IEC 14496-12 / 14496-14 / 14496-15）。ADR-0096（P5a）已定下架构边界——D1 box 遍历与 `mdat` lazy 封装、D2 跨层导航（`avcC`/`esds` 至 H.264/AAC）、D3 样本表窗口化——但其 D1 的 DSL 片段引用了今天不存在的语言构造。本 ADR（任务 P5c，经 P5c-R 与 P5c-R2 修正）探测当前 DSL/运行时对 ISOBMFF 容器结构「可/不可表达」的真实边界，并为每一项能力增量锁定**可直接编码与测试的精确合同**，且每一项都映射到工作树中的真实 C++ 类型/函数。本 ADR **不产出任何规则资产**：全部规则消费推迟到 P5d+（「枚举/下钻机制未定 ⇒ 不得提交 MP4 规则资产」闸门）。

七条容器事实（计划 fact 10–16，主 Agent 已于 2026-08-16 用 `svtool rule check` 实测）作为既知事实纳入，不再重复探测；本轮跑过的探针均现场复确认：

1. **FourCC 匹配今天已可表达**：`bits<32> box_type @equals(0x66747970)` 可编译（`Rule OK`）。无需新增 `bytes` 值类型。
2. **`bits<64>` 放行**——`largesize` 可直接读取；字段宽度上限为 64 位（`Bit field width must be in the range 1..64`，`dsl.cpp:1038`）。
3. **`size == 1` / `else` 的载荷字节数可表达**，但只有一种写法成立：把 `computed` 与 `@lazy` **各自放进 `if`/`else` 两个分支**（跨分支的单一 `computed` 会报 `Computed dependency is not guaranteed on the current branch`）。
4. **`size == 0`（延伸至文件尾）是唯一真实语言缺口**：`compressed_payload` 是唯一的「吃掉剩余」终结符，但受限于 `error: compressed_payload must occur once as the final top-level item`（`dsl.cpp:1589`），进不了 `if` 分支；且不存在 `available_bytes` 内建。
5. **`maximumExpandedFieldsPerStructure = 99'999`（按 struct 计，`dsl_ir.cpp:13`）** 限制每个结构体的字段展开规模；超过上限的样本表必须采用 lazy 区域窗口化。
6. **今天全语言只有「顶层 `sequence` + 单一 `payload<rbsp>` 分派」一种层次机制**（H.264 形态）。嵌套 box 枚举是本 ADR 要解决的硬阻塞。
7. **`bits<128>` 不可表达**（探针：`error: Bit field width must be in the range 1..64`，`dsl.cpp:1038`）；128 位值声明为两个 `bits<64>` 字段。

本轮全部探针使用工作树工具二进制 `build/dev/tools/svtool/svtool`（`svtool 0.1.0 (DSL 0.1)`）执行；scratch 源位于仓库之外的会话 scratch 目录 `/Users/yun/.gemini/antigravity-cli/brain/12458dc0-7cd4-40c3-b0af-86d27dcb7b62/scratch/`。探针源码、命令、stdout/stderr 与 exit code 在 P5c-R2 任务报告中完整复现（下表列出每条命令与逐字结果）：

| 探针 | 命令 | 结果（逐字错误 / 状态） | 现场位置 |
| :--- | :--- | :--- | :--- |
| P1 `scan(mp4_box)` | `svtool rule check scratch/p5c_p1_scan_mp4_box.svfmt` | `error: Only h264_start_code and adts_frame are supported` | parser `dsl.cpp:3568`、IR `dsl_ir.cpp:3679` |
| P2a 结构体类型字段 | `svtool rule check scratch/p5c_p2a_struct_typed_field.svfmt` | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` | 字段类型解析 |
| P2b struct 内的 `sequence` | `svtool rule check scratch/p5c_p2b_nested_sequence.svfmt` | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` | sequence 仅限顶层 |
| P2c `payload<mp4>` view kind | `svtool rule check scratch/p5c_p2c_payload_mp4_kind.svfmt` | `error: The only accepted payload view kind is rbsp` | parser `dsl.cpp:3669`、IR `dsl_ir.cpp:3717` |
| P2d `@container` 注解 | `svtool rule check scratch/p5c_p2d_container_annotation.svfmt` | `error: Unknown annotation '@container'` | `dsl.cpp:1880` |
| P5a `@target_format` 后置于 lazy 区域 | `svtool rule check scratch/p5c_p5a_target_format_lazy.svfmt` | `error: Lazy byte regions accept only @description and @spec` | `dsl.cpp:1895` |
| P5b `@target_format` 置于 `@lazy` 与 `bytes` 之间 | `svtool rule check scratch/p5c_p5b_target_format_preposition.svfmt` | `error: Expected bytes after @lazy(...)` | `dsl.cpp:1112` |
| P7a `unsupported("")` 空 reason | `svtool rule check scratch/p5c_p7a_unsupported_empty_reason.svfmt` | `error: Unsupported statements require a non-empty reason` | `dsl_ir.cpp:2748` |
| P7b `unsupported` 锚定 computed 字段 | `svtool rule check scratch/p5c_p7b_unsupported_computed_anchor.svfmt` | `error: Unsupported anchors require a source-backed scalar field` | `dsl_ir.cpp:2771` |
| P7c `repeat` 内的 `unsupported` | `svtool rule check scratch/p5c_p7c_unsupported_repeat_local.svfmt` | `error: Unsupported statements cannot be repeat-local items` | `dsl_ir.cpp:2741` |
| P7d box 头字段之后 `unsupported`（moof 形态） | `svtool rule check scratch/p5c_p7d_unsupported_positive.svfmt` | `Rule OK` | — |
| P8a uuid 标记，两个 `bits<64>` usertype 字段 | `svtool rule check scratch/p5c_p8_uuid_marker.svfmt` | `Rule OK` | — |
| P8b `bits<128>` 字段 | `svtool rule check scratch/p5c_p8_bits128.svfmt` | `error: Bit field width must be in the range 1..64` | `dsl.cpp:1038` |
| P8c `@container(Child)` 后置位 | `svtool rule check scratch/p5c_p8_container_annotation.svfmt` | `error: Unknown annotation '@container'`（名称闸门先行；参数 `Child` 解析为 `DslAnnotationValueKind::Identifier`，`dsl.cpp:801`） | `dsl.cpp:1880` |
| P8d uuid 完整 size 分支 | `svtool rule check scratch/p5c_p8d_uuid_full_branches.svfmt` | `Rule OK` | — |
| P8e uuid `size == 0` 缺口（`available_bytes()`） | `svtool rule check scratch/p5c_p8e_uuid_size0_gap.svfmt` | `error: Pure function is not declared before this call` | `dsl_ir.cpp:772` |

---

## 决策

### D1：顶层 box 枚举——新增 `DslScannerKind::Mp4Box` 与锁定的分帧合同

scanner kind 闭集 `{H264StartCode, AacAdtsFrame}`（`dsl_ir.h:219-221`）扩展出 `Mp4Box`。新 scanner **只做分帧（framing）**且**不得识别任何具体 FourCC**：「哪个 FourCC 意味着什么」是格式语义，留在 DSL 规则里（`bits<32> type @equals(0x...)` 派发）。scanner 侧的合法性只限于通用 8/16 字节头最小值；`uuid` 的 24/32 字节最小值是 **DSL 语义**（D3），不是 scanner 语义。

**`size`/`largesize` 语义**：按 ISO/IEC 14496-12，`size`（及 `size == 1` 时的 `largesize`）是**整个 box（含头）的尺寸**。scanner 因此绝不计算 `header_len + size`；它直接从读到的字段得出 `box_size`：

- `size != 1`：`box_size = size`。
- `size == 1`：`box_size = largesize`。

**每个 box 起点的合法尺寸集合**（单 box 分帧合法性）：

1. `size == 0`——`box_size == 0` 表示 box 延伸至当前区域 EOF 且**终结**（该区域内其后不存在其它 box）。
2. `size == 1`——large-size box；16 字节头为必需，合法约束为 `largesize >= 16`。
3. `size >= 8`——普通 8 字节头 box。
4. 其它任何取值（`size` 为 2..7；或 `size == 1` 且 `largesize < 16`）**畸形**：不是候选 box，该位置不开启 box。

**分帧算术（减法式、可移植、无 checked-128 强制）**：

- box 起点为 `start`、区域末尾为 `region_end`，定义 `remaining = region_end - start`（按构造非负；区域是有界跨度）。
- **合法跨度**：`box_size <= remaining`。`span_end = start + box_size`；该加法不可能溢出，因为 `box_size <= remaining` 蕴含 `start + box_size <= region_end`。
- **截断跨度**：`box_size > remaining` → box 截断：跨度钳制到区域末尾并标记截断；DSL lazy 区域随后命中既有 VM lazy 截断合同（`TruncatedSource`，`"Lazy byte region exceeds the available source range"`，`dsl_vm.cpp:3249-3252`）。头部格式良好的截断末尾 box 仍计入候选链。

**头长与载荷锚点**（头长只用于最小值与 DSL 载荷锚点，绝不进入分帧跨度）：

- 普通头 8 字节（`size` + `type`）。
- large 头 16 字节（`size` + `type` + `largesize`）。
- `uuid` box：8/16 字节头之后另有 16 字节 `usertype`；规则级载荷锚点相应 +16。scanner 按 `box_size` 分帧，对 `uuid` 透明；24/32 最小值由 DSL 规则（D3）强制，scanner 不读 `type` 故无法执行该约束。
- 最小合法 box 尺寸：普通 8、large 16。`uuid` 专属最小值（24/32）是 DSL 级约束。

**边界行为**（全部定义；下文 ADTS 类比仅限所引两个先例，不做泛化）：

- **截断头**：box 起点处剩余不足 8 字节 → 区域结束；扫描停止；尾部残缺字节不构成候选。
- **`size == 1` 但仅剩 8..15 字节**：16 字节 large 头无法补全 → **不完整 large 头**；扫描停止；该位置不构成候选（`u64 largesize` 无法读取）。
- **畸形 `size`（2..7，或 `size == 1 && largesize < 16`）**：该位置不开启 box，scanner **停止**（不做重同步）。理由：与 ADTS 流不同，MP4 box 流没有可滑动搜索的重同步标记，尝试重同步等于猜测 box 边界；区域在最后一个合法 box 处结束。这是与 ADTS 尾部垃圾先例（沿 `0xFFF` syncword 滑动）的**刻意分歧**，此处不主张任何 ADTS 类比。
- **跨度越界**：由上述截断跨度规则覆盖（`box_size > remaining`）。
- **区域末尾**：scanner 在区域末尾干净停止。

**单 box 合法性与探测器阈值**（两个独立合同）：

- 单 box 合法性即上述分帧规则集（格式良好的头 + 合法跨度）。
- 探测器（`detectMp4Candidate`，P5d-1）按**多 box 长度链自洽性**分级：`Strong` 要求 ≥ 3 个连续格式良好的 box 头且跨度铺满被检前缀；`Probable` = 2；`Weak` = 1。无单证据降级条款（T15b 教训）。精确阈值是 P5d-1 校准目标；≥3 为 Strong 是合同。

**scanner/DSL 数据合同——span-only，不发布头部值**：

- scanner 输出每个 box 的**跨度**（起始偏移、结束偏移、截断标记）。它**不发布** `size`/`type`/`largesize` 值。
- runner 将跨度映射为源子视图；DSL 的 `Box` 结构体在跨度内**从源重新读取** `size`/`type`/`largesize`。准确先例是 AAC 分析器：从帧跨度建 mapping——`core::SourceMapping::create(frameViewId, {*record.frameSpan})`（`aac_adts_analyzer.cpp:346`）并经 `makeLocation({*record.frameSpan})` 定位（`aac_adts_analyzer.cpp:309`）——再从帧跨度重读头部字段。不声称 `Mp4Box` record 形态与 `AacAdtsRecord` 完全一致（后者另有 `headerLength`/`payloadLength`/`aacFrameLength`/`crcPresent` 字段）。
- 早期 P5c 的「scanner 把 size/type/largesize 作为元素值发布」陈述**撤回**（值下传设计需要在 IR 与 VM 中新增完整 typed scan/value schema——见被否决方案 7）。

P5d-1/P5d-2 之后，`sequence<Box> boxes = scan(mp4_box);`（配 `@index(progressive)`）即可表达。

### D2：嵌套下钻——`@container` 注解与锁定的实现映射

今天全语言只有一种层次机制：顶层 `sequence` + `payload<rbsp>` 分派（fact 6；探针 P2a–P2d）。容器 box（`moov`/`trak`/`mdia`/`minf`/`stbl`）需要对一个字节区间**枚举多个子 box**。选定机制为 `@container` + runner 重入。

**锁定语法**（lazy `bytes` 区域的后置位，一个参数，结构体标识符）：

```
@lazy(payload_bytes) bytes payload @container(ChildStruct);
```

- **宿主**：仅 `DslAnnotationTarget::LazyRegion`。`@container` 置于任何其它宿主一律拒绝。
- **参数合同**：恰好一个参数；token 必须解析为 `DslAnnotationValueKind::Identifier`（通用注解参数解析在 `dsl.cpp:801` 将标识符 token 归类；`@index(progressive)` 消费端 `dsl.cpp:3577-3580` 是标识符类参数解析并校验的既有端到端先例——探针 P7d 携带 `@index(progressive)` 且为 `Rule OK`）。标识符必须指名已声明的结构体。
- **诊断**（P5d-2，复用既有 `DslDiagnosticCode` 取值；精确文案由 P5d-2 测试锁定）：
  - 未注册名（今天）：`Unknown annotation '@container'`（`dsl.cpp:1880`）；
  - 宿主非 LazyRegion：宿主白名单报错（通用 `@%1 is not supported on this declaration`，`dsl.cpp:1905-1907`）；
  - arity ≠ 1：`InvalidAnnotation`；
  - 参数 kind ≠ Identifier：`InvalidAnnotation`；
  - 目标结构体未声明：`UnknownReference`（镜像 sequence 元素类型查找，`dsl.cpp:3556-3564`）。

**parser AST**（机制不变）：通用 `DslAnnotation { name = "container", arguments = [{ kind = Identifier, text = structName }] }`，由注解解析器产出（`dsl.cpp:782-810`）；探针 P8c 证明名称闸门先行触发、注册后参数解析为 Identifier。`DslLazyRegion`（`dsl.h:234-240`）在其既有 `annotations` 向量中**保留该注解**——AST 类型不新增字段。

**typed IR**——容器引用**不**存在 parser AST 上，落在 typed 字段上。`DslTypedField`（`dsl_ir.h:108-124`）新增 `std::optional<quint32> containerChildStructIndex`，仅当 `type.kind == DslValueTypeKind::LazyBytes` 时有效（compileLazyRegion 发射 `typedField.type = {DslValueTypeKind::LazyBytes, ...}`，`dsl_ir.cpp:2525`）。被否决的替代：专用 `DslTypedLazyContainer` 类型会为无收益而重复字段生命周期。

**compileLazyRegion 映射**（`dsl_ir.cpp:2462-2540`）：

- 引用解析：区域的字节数表达式经 `resolveExpressionDependency` 解析（`dsl_ir.cpp:2467-2472`，消息 `"Lazy byte-count expression"` / `"Lazy byte counts require scalar unsigned fields"`）；结构体名参数对 `program.structs` 解析并把 struct index 存入 `typedField.containerChildStructIndex`。
- 重复诊断抑制：全部编译诊断经 `addDiagnostic`（`dsl_ir.cpp:28-41`）按 `(code, message, range)` 去重；容器校验诊断走同一路径，无需新抑制机制。
- 编译期校验：结构体存在；是可解码结构体（有字段）；区域的字节数表达式在既有 lazy 校验下保持源锚定。

**runner 读取**（`DslOpcode::RegisterLazyBytes`，`dsl_vm.cpp:3169-3320`）：物化携带 `containerChildStructIndex` 的 lazy 区域时，runner 在该区域的字节跨度 `[anchor_start, anchor_end)` 上重入 `Mp4Box` scanner，用指名结构体作为元素类型物化子节点，挂在**容器的 lazy 节点之下**。容器节点保持 lazy 节点（子节点取代不透明字节视图；载荷绝不急切物化为字节 blob）。

**状态矩阵**（锁定到真实 `AnalysisTree::canTransition`，`analysis_model.cpp:218-251`）：

| 从 | 到（允许） | 容器节点的含义 |
| :--- | :--- | :--- |
| `Lazy` | `Indexing` | 子枚举开始（重入） |
| `Lazy` | `WaitingDependency` | 子重入等待上下文依赖 |
| `Lazy` | `Cancelled` / `Unsupported` / `Invalid` / `Materialized` | 直接终结转换（无子节点） |
| `Indexing` | `WaitingDependency` | 枚举中途依赖等待 |
| `Indexing` | `Cancelled` / `Unsupported` / `Invalid` / `Materialized` | 枚举后的终结状态 |
| `WaitingDependency` | `Indexing` | 依赖已解析；枚举恢复 |
| `WaitingDependency` | `Cancelled` / `Unsupported` / `Invalid` / `Materialized` | 等待中终结 |
| `Cancelled` | `Indexing` | `resumeCancelled` 重入枚举 |
| `Unsupported` / `Invalid` / `Materialized` | — | 终结；无进一步转换 |

物化全部子节点的容器节点终结于 `Materialized`；截断子节点或子节点错误不回滚容器（见下文子错误传播）。

**runner 持有、跨重入共享的预算**（锁定到 `DslExecutionLimits`，`dsl_vm.h:30-51`，无「design target」值）：

- `maximumNodeDepth = 256`（既有 `defaultMaximumNodeDepth()`）约束跨全部重入的嵌套深度；超出在该区域上产生 `ResourceLimit`。
- `maximumMaterializedNodes = 100'000`（既有 `defaultMaximumMaterializedNodes()`）跨全部层级共享。
- `maximumInstructions = 1'000'000`、`maximumViewDepth = 64`、`maximumCallDepth = 64` 经同一 execution-options 结构体按重入生效；取消经共享的 `DslExecutionOptions.cancellation` token 每 `cancellationCheckInterval = 1'024` 条指令检查一次。

**子映射与多 span 处理**：子节点精确铺满 `[anchor_start, anchor_end)`。每个子节点的 `FieldLocation` 由既有逻辑子视图路径产出：`core::LogicalRange::create(core::LogicalBitAddress(mapping.viewId(), logicalStart + fieldStart), bitCount)` 再 `mapping.locate(*range)`（`dsl_vm.cpp:3258-3262`），因此无论容器的 `SourceMapping` 携带多少源 span，子节点都经 `SourceMapping::locate`（`coordinates.h:134`）映射。**决策**：多 span 映射经逻辑子视图 + `locate` 支持（VM 与 AAC/H.264 分析器已用的路径，`aac_adts_analyzer.cpp:346`、`h264_annex_b_analyzer.cpp:411-417`）；不施加单连续 span 限制。

**递归/环策略**：重入由规则逐层声明——只有标注 `@container` 的区域才重入，且区域是互不相交的递减字节跨度，因此按构造不可能产生无界自递归。`maximumNodeDepth = 256` 是确定性兜底。

**取消、截断、子节点错误传播**：子节点物化与顶层扫描共用同一批/取消/预算合同；逐子诊断挂在子节点上；失败或截断的子节点不中止兄弟节点；无全局回滚。

**核心/runner 中立性**：子结构体仅按 IR 索引引用；**`moov`/`trak`/`mdia`/`minf`/`stbl`（或任何）FourCC 字面量绝不出现于 core 或 runner 代码**。

### D3：type 标签匹配——`bits<32>` + `@equals(0x...)`，不新增值类型

fact 10 成立：`bits<32> box_type @equals(0x66747970)` 可编译；十六进制整数字面量放行。P5d+ 规则引用的常用 FourCC 常量表（大端 ASCII 打包进 32 位字面量）：

| FourCC | 值 | FourCC | 值 | FourCC | 值 | FourCC | 值 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `ftyp` | `0x66747970` | `moov` | `0x6D6F6F76` | `mdat` | `0x6D646174` | `free` | `0x66726565` |
| `mvhd` | `0x6D766864` | `trak` | `0x7472616B` | `tkhd` | `0x746B6864` | `edts` | `0x65647473` |
| `elst` | `0x656C7374` | `mdia` | `0x6D646961` | `mdhd` | `0x6D646864` | `hdlr` | `0x68646C72` |
| `minf` | `0x6D696E66` | `vmhd` | `0x766D6864` | `smhd` | `0x736D6864` | `dinf` | `0x64696E66` |
| `dref` | `0x64726566` | `stbl` | `0x7374626C` | `stsd` | `0x73747364` | `stts` | `0x73747473` |
| `stss` | `0x73747373` | `stsc` | `0x73747363` | `stsz` | `0x7374737A` | `stco` | `0x7374636F` |
| `avc1` | `0x61766331` | `avcC` | `0x61766343` | `mp4a` | `0x6D703461` | `esds` | `0x65736473` |
| `pasp` | `0x70617370` | `btrt` | `0x62747274` | `mvex` | `0x6D766578` | `trex` | `0x74726578` |
| `moof` | `0x6D6F6F66` | `mfhd` | `0x6D666864` | `traf` | `0x74726166` | `tfhd` | `0x74666864` |
| `trun` | `0x7472756E` | `udta` | `0x75647461` | `meta` | `0x6D657461` | `ilst` | `0x696C7374` |

**`uuid` 扩展类型 box**（自 P5c 初稿修正、P5c-R2 补全）：`bits<128>` 不可表达（探针 P8b，`dsl.cpp:1038`）。锁定形态：

```
bits<32> size;
bits<32> type @equals(0x75756964);
bits<64> usertype_hi;
bits<64> usertype_lo;
```

**`uuid` 的 scanner/DSL 分工**：scanner 只执行通用最小值（8/16 字节，D1）且对 `type` 透明；24/32 字节最小值是 **DSL 语义**——规则断言 `type @equals(0x75756964)` 并读取 16 字节 `usertype`，此后载荷锚点为 `+24`（普通）/ `+32`（large）。**完整 size 分支形态**（探针 P8d 编译这些分支，`Rule OK`）：

```
if (size == 1) {
    bits<64> largesize;
    computed<u64> large_payload = largesize - 32;
    @lazy(large_payload) bytes large_data;
} else {
    computed<u64> payload = size - 24;
    @lazy(payload) bytes data;
}
```

`size == 0` 分支（延伸至 EOF、终结）读取 `usertype_hi`/`usertype_lo` 后用 `available_bytes()` 内建（D4）为载荷定尺寸——探针 P8e 确认该分支**今天不可表达**：`error: Pure function is not declared before this call`（`dsl_ir.cpp:772`，纯函数闸门拒绝未注册调用）。

合同：v0.1 **透明保存** 128 位值为两个 `bits<64>` 字段（配合 D3 载荷锚点 −24/−32）；**不支持整 UUID 字面量相等比较**（无字符串字面量 tokenizer、无 bytes 值类型，fact 10）。此即收口 ADR-0096 D1 §5 留给 ADR-0097 的待决问题。

### D4：size 分支——分支内 `computed` + `@lazy`；`size == 0` 增量是新增 `available_bytes()` 内建

- **`size == 1` / `else`**：唯一可行写法是分支内形式（fact 3），ADR-0096 D1 已采用该形态：`if (size == 1) { bits<64> largesize; computed<u64> large_payload = largesize - 16; @lazy(large_payload) bytes large_data; } else { computed<u64> payload = size - 8; @lazy(payload) bytes data; }`。本 ADR 复确认其为唯一可行写法；D3 给出 `uuid` 变体（−32 / −24）。
- **`size == 0`**：今天确实不可表达（fact 4）：`compressed_payload` 被限制为最后一个顶层项（`dsl.cpp:1589`），进不了 `if` 分支；且无任何剩余长度查询。

**增量锁定合同——`available_bytes()` 内建**（P5d-2）：

- 语义：返回**当前源区域自当前读取位置起的剩余字节数**（字节粒度；返回值；可用于任意 `computed` 表达式，镜像 `more_rbsp_data()`）。实现映射：在 `compileExpression` 的内建闸门中与 `more_rbsp_data` 并列注册调用（`dsl_ir.cpp:589-611`，`DslTypedExpressionKind::MoreRbspData` 位于 `dsl_ir.cpp:611`；VM 求值位于 `dsl_vm.cpp:496-499`），新增 `DslTypedExpressionKind::AvailableBytes`，按 `reader.remainingBits() / 8` 求值。
- 借助它，`size == 0` 可表达为：

```
if (size == 0) {
    computed<u64> payload_bytes = available_bytes();
    @lazy(payload_bytes) bytes payload;
}
```

- scanner 按 D1 执行 `size == 0` 分帧（跨度至 EOF、终结 box）；内建只让规则为其 lazy 载荷区域定尺寸。
- 被否决方案：scanner 计算剩余长度并下传（见被否决方案 8）。

### D5：`@target_format`——完整合同与消费映射

**锁定语法**（lazy `bytes` 区域的后置位，一个字符串参数）：

```
@lazy(payload_bytes) bytes payload @target_format("video/mp4");
```

- **宿主**：仅 `DslAnnotationTarget::LazyRegion`。
- **参数合同**：恰好一个参数；token 必须解析为 `DslAnnotationValueKind::String`（字符串字面量解析在 `dsl.cpp:798`）；字符串不得为空。
- **注册表改动**（P5d-2）：`knownAnnotations` 位于 `dsl.cpp:1834-1866`；预留项 `{u"target_format", 0U}`（`dsl.cpp:1865`）改为 `{u"target_format", static_cast<quint32>(DslAnnotationTarget::LazyRegion)}`；LazyRegion 白名单消息（`Lazy byte regions accept only @description and @spec`，`dsl.cpp:1895`）同步扩展接纳 `@target_format`。
- **诊断**（P5d-2）：arity ≠ 1、参数 kind ≠ String 或空串 → `InvalidAnnotation`；宿主 ≠ LazyRegion → 白名单报错（`@%1 is not supported on this declaration`，`dsl.cpp:1905-1907`）。
- **typed IR**：`DslTypedField`（非 parser AST）新增 `std::optional<QString> targetFormat`，仅当 `type.kind == DslValueTypeKind::LazyBytes` 时有效；在 `compileLazyRegion`（`dsl_ir.cpp:2462-2540`）编译期校验。
- **节点元数据**：`core::AnalysisNodeMetadata`（`analysis_model.h:76-80`，当前为 `typeName`/`description`/`specification` 三个固定字段）新增 `std::optional<QString> targetFormat`。这是刻意的增量改动：既有结构体是固定字段集而非键值表，故字段以具名成员加入。
- **缓存往返**（`analysis_cache_payload.cpp`）：新增 `nodeTargetFormatFlag = 4U`（`nodeLocationFlag = 1U` / `nodeSpecificationFlag = 2U` 之后的下一个空闲位，`analysis_cache_payload.cpp:35-36`）；在物化结果编码器置位（`:682-683`），在 `specification` 旁读写字符串（`:691-698` 编码；`:833`/`:849` 解码模式），并扩展未知位拒绝掩码（`:796`，当前为 `flags & ~(nodeLocationFlag | nodeSpecificationFlag)`）。**旧缓存兼容**：不带该 flag 位的缓存页解码为 `targetFormat = std::nullopt`（flag 缺失 ⇒ 字段缺失），P5d 之前的缓存恢复不报错。
- **传播**：runner 在物化时把 `targetFormat` 存入 lazy 节点的 `AnalysisNodeMetadata`（`RegisterLazyBytes`，`dsl_vm.cpp:3169-3320`）。
- **消费合同——`"video/mp4"` 不直接传给 `RulePackageCatalog::resolve`**：真实签名是 `resolve(const RulePackageIdentity& identity, QStringView entryPointId, QStringView runningLanguage, QStringView runningEngine)`（`rule_catalog.h:52-53`）——需要完整包身份、入口 ID 与运行语言/引擎。裸格式字符串无法调用它。**决策（两案取一，选案 1）：新增 format→package/entry 选择服务**（P5d-2 落地）：
  - `resolveByFormat(QStringView format, QStringView runningLanguage, QStringView runningEngine)` 扫描已注册包，找 `RulePackageEntryPoint::format`（`rule_package.h:61-69`）等于该格式字符串的入口点，按语言合同/引擎范围过滤，返回 `RuleCatalogLookupResult`。
  - 成功：恰好一个入口点匹配 → `RuleCatalogLookupStatus::Found`。
  - 缺包：无匹配 → `RuleCatalogLookupStatus::MissingContent` 并附消息。
  - 歧义：≥ 2 匹配 → `RuleCatalogLookupStatus::VersionConflict`，消息附竞争包 ID。
  - 被否决的替代（案 2）——注解携带完整包身份——把规则文本与打包细节耦合，否决。
- **P5d 与 P5i 切片**：P5d-2 交付注册表改动、typed IR 字段、节点元数据字段、缓存 flag 与 `resolveByFormat`（全部带测试）；P5i 消费它——UI 对 `avcC`/`esds` lazy 区域的动作读取节点元数据中的 `targetFormat`，调用 `resolveByFormat`，以该区域源跨度回映到容器字段打开目标规则的分析视图（双向导航）。
- **P5d 测试矩阵**：正向——lazy 区域 `@target_format("video/mp4")` 编译、IR 保留字符串、节点元数据携带、缓存编码→解码往返、从缓存恢复的会话保留；反向——arity 0、arity 2、整型参数、位字段宿主、空串（各自带预期 `InvalidAnnotation` 码）；解析——一个匹配入口点 → `Found`，无 → `MissingContent`，两个 → `VersionConflict`。

### D6：样本表窗口化——带锁定接口的真实能力

- 编译期上限保持 `maximumExpandedFieldsPerStructure = 99'999`（按 struct 计，`dsl_ir.cpp:13`）；展开超限为编译错误（fact 5；`repeat(count, 99999)` → `Bounded repeat expansion exceeds the structure materialization limit`）。
- **收窄后的要求**：*产品*必须支持潜在超大样本表（数十万条目），因此**默认**规则形态是 lazy 区域窗口化（fact 15）：`computed<u64> table_bytes = entry_count * 4; @lazy(table_bytes) bytes entry_table;`。条目数远低于按结构体上限的小表**可以**用有界 `repeat` 展开；窗口化不是每个表的编译期强制项。

**窗口解码器——锁定能力接口**（P5d-3 子项；**这是唯一切片**——早前「P5d-3 子项或独立 P5d-4」的歧义已裁定为 P5d-3 子项）：

- **规则侧（规则如何提供解码器输入）**：规则把条目布局声明为可解码结构体 `Entry`（如每个条目一个 `bits<32> offset;`），把 `entry_count` 暴露为标量字段，并以 `entry_count * entry_size`（`entry_size` 是编译期常量表达式，即每条目位宽）为 lazy 区域定尺寸——区域为 `@lazy(table_bytes) bytes entry_table;` 且 `computed<u64> table_bytes = entry_count * entry_size_bytes;`。runner 从容器的 `@container(Entry)` 引用（D2）得出 `entryStructIndex`（typed 条目结构体）。
- **请求/结果类型**（runner/会话持有，仅头文件，P5d-3 中位于 `src/rules/include/streamview/rules/window_decoder.h`）：
  - `WindowDecodeRequest { AnalysisNodeId lazyRegionNode; quint32 entryStructIndex; quint64 entryCount; quint64 entrySizeBits; quint64 pageIndex; quint64 pageSize; }`。
  - `WindowDecodeResult { DslExecutionStatus status; std::vector<AnalysisNodeId> entryNodes; quint64 pageStartIndex; bool nextPageAvailable; }`，状态为 `Materialized`（成功）、`TruncatedSource`（区域短于 `entryCount * entrySizeBits`）、`ResourceLimit`（页超出节点预算）、`Cancelled`（页中途取消 token 触发）、`InvalidDefinition`（请求非法）。
- **分页**：固定默认 `pageSize = 256` 条目，可按请求覆盖；越界 `pageIndex` 返回空页且 `pageStartIndex == entryCount`；底层会话变化时窗口失效；单页绝不超出 `maximumMaterializedNodes`（`dsl_vm.h:36-38`）。
- **坐标映射**：每个条目的位置从 **lazy 区域的逻辑范围加受检偏移**推导，再经 `SourceMapping::locate` 定位——`core::LogicalRange::create(core::LogicalBitAddress(mapping.viewId(), regionLogicalStart + checked(entryIndex * entrySizeBits)), entrySizeBits)` 然后 `locate`（`dsl_vm.cpp:3258-3262` 模式）。受检乘法拒绝溢出；**不假设单一绝对 `container_anchor`**，因为容器的 `SourceMapping` 可能携带多个源 span（D2 多 span 决策）。
- **UI 翻页契约**：固定页大小前/后翻页；条目从 `pageStartIndex` 渲染；坐标来自定位后的 `FieldLocation.sourceSpans()`。
- P5g 规则消费该能力；规则不自研窗口化。

### D7：分片 MP4（`moof`）——规则内 `unsupported` 作为 P5d 规范性合同

- **编译证据（P5c，已验证）**：`bits<32> size; bits<32> type; unsupported("fragmented MP4 (moof/traf) is outside the v0.1 subset") at type;` 形态可编译——探针 P7d，`Rule OK`。P7d **只证明可编译**。
- **以下运行时行为是 P5d runner 的规范性合同与必测行为，不是 P5c 已验证事实**：
  1. `moof` box 节点物化为 `MaterializationState::Unsupported`、`DiagnosticCode::UnsupportedSyntax`、`DiagnosticSeverity::Warning`，锚定在 `type` 字段；
  2. `moof` 节点的**前缀字段 `size` 与 `type` 保持物化**（节点保留已解码的头字段；`markPartial`/`MarkUnsupported` 语义保留已物化字段，按 ADR-0098 合同）；
  3. 此前顶层 box（`ftyp`、`moov`…）保持物化；
  4. 顶层扫描**继续**：同一区域中 `moof` 之后的 box（如分片 fixture 中 `moof` 之后的 `mdat`）仍被物化。
- **P5d 测试必须断言以上全部四点**；测试 fixture 是**提交/生成资产，而非 `/tmp` 路径**：fixture 生成脚本（一条 ffmpeg 调用，见验证矩阵）提交到 `tests/fixtures/` 并由测试构建运行，或提交生成字节；测试绝不引用 `/tmp/p5c_frag.mp4`。`ffprobe -v trace` 摘录显示 `moof` → `mfhd`/`traf` → `tfhd`/`trun`，之后紧跟顶层 `mdat`。
- `unsupported` 的三条禁令不阻碍容器场景（探针 P7a/P7b/P7c，`dsl_ir.cpp:2741/2748/2771`）：`moof` 判定位于 box 结构体顶层（非 repeat-local）；reason 始终非空；锚点是 source-backed 的 `bits<32> type` 字段，绝不用 `computed` 字段。
- 被否决方案：探测器分级整份拒绝（见被否决方案 6）。

---

## 被否决方案

1. **结构体类型字段**（`BoxHeader header;`）做嵌套——否决：parser 今天拒绝结构体类型字段（探针 P2a），且 `@container`（D2）无需新字段类型即可实现嵌套。
2. **嵌套 `sequence` 声明**——否决：需要新语法、IR 降级与 VM 排序语义，且与「sequence 仅限顶层」契约冲突（探针 P2b）；`@container` 严格更小。
3. **为嵌套引入 `payload<box>` view kind**——v0.1 否决：分派视图按值选一个结构体，而容器要枚举多个子 box，解决不了嵌套（探针 P2c）。将来可作为顶层派发的人机工学优化回归。view kind 闭集暂时保持 `{rbsp}`（`dsl.cpp:3669`、`dsl_ir.cpp:3717`）。
4. **`bits<128>` 表达 `uuid` usertype**——否决：不可表达（`Bit field width must be in the range 1..64`，`dsl.cpp:1038`，探针 P8b）；锁定形态为两个 `bits<64>` 字段（探针 P8a）。
5. **整 UUID 字面量相等比较**——v0.1 否决：无字符串字面量 tokenizer、无 bytes 值类型（fact 10）；v0.1 仅透明保存 128 位值。
6. **探测器级整份分片 MP4 拒绝**——否决（D7）：丢失已解码前缀与后续 box 扫描，并把 FourCC 逻辑塞进核心检测。
7. **scanner 头部值发布**（scanner 把 `size`/`type`/`largesize` 作为元素值输出）——否决：span-only 合同（D1）严格更小，且匹配既有 runner record 映射（`aac_adts_analyzer.cpp:309/:346` 先例）；值发布需要为 IR 与 VM 新增 typed scan/value schema，无已证收益。
8. **scanner 计算剩余长度下传以表达 `size == 0`**——否决，改用 `available_bytes()` 内建（D4）：内建格式中立、可复用，且避免为角例引入 context 管线。
9. **为 FourCC 新增 `bytes` 值类型**——已被 fact 10 否决：`bits<32>` + `@equals(0x...)` 足够。
10. **`@target_format` 携带完整包身份**——否决（D5）：把规则文本与打包细节耦合；`resolveByFormat` 服务让注解保持裸格式字符串。

---

## 影响与结论

### 正向收益

- P5d 之后 ISOBMFF box 树完全可表达：带锁定分帧合同的顶层枚举（D1）、带真实状态机与预算映射的容器下钻（D2）、type 派发（D3）、含 `size == 0` EOF 情形的 size 算术（D4）、带锁定接口的窗口解码器（D6）、以及分片 MP4 规范性合同（D7）。
- `@target_format`（D5）提供 P5i 消费的跨格式导航元数据，含锁定的元数据、缓存往返、旧缓存兼容与 `resolveByFormat` 解析合同。
- 核心保持格式中立：scanner 只知分帧（D1）；runner 重入是通用的（D2）；`src/` 核心路径零 FourCC 字面量。
- 本 ADR 的每项能力增量都映射到工作树的真实 C++ 类型/函数，且有可直接编码与测试的合同；P5d 切片按 T4a/T4b 先例可独立闭环。

### 负向代价

- P5d 切片需落地四个新能力增量：(1) `DslScannerKind::Mp4Box` scanner + 探测器（P5d-1），(2) `@container` 注解 + runner 重入（P5d-2），(3) `available_bytes()` 内建（P5d-2），(4) `@target_format` 注册 + 元数据 + 缓存 + `resolveByFormat`（P5d-2）——外加窗口解码器（P5d-3 子项，唯一窗口化切片）。
- `AnalysisNodeMetadata` 与缓存格式新增一个可选字段；旧缓存页不带它解码（`targetFormat = nullopt`）。
- `payload<rbsp>` 专属限制保留；顶层派发在新增派发 view kind 之前只能用 `type` 上的 `if`/`else`。
- `size == 0` box 天然终结（延伸至 EOF），同一区域内其后不可能再有其它顶层 box；规则须反映这一点。

---

## 验证矩阵与证据

本轮全部 16 个探针使用工作树工具二进制 `build/dev/tools/svtool/svtool`（`svtool 0.1.0 (DSL 0.1)`）执行；scratch 源位于仓库之外的会话 scratch 目录。下表列出每个探针的命令与结果；**探针源码、可执行命令、stdout/stderr 与 exit code 在 P5c-R2 任务报告中完整复现**。

| # | 探针 | 命令 | 结果 |
| :--- | :--- | :--- | :--- |
| P1 | `scan(mp4_box)` 顶层枚举 | `svtool rule check scratch/p5c_p1_scan_mp4_box.svfmt` | `error: Only h264_start_code and adts_frame are supported`（`dsl.cpp:3568`） |
| P2a | 结构体类型字段 | `svtool rule check scratch/p5c_p2a_struct_typed_field.svfmt` | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` |
| P2b | struct 内 `sequence` | `svtool rule check scratch/p5c_p2b_nested_sequence.svfmt` | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` |
| P2c | `payload<mp4>` view kind | `svtool rule check scratch/p5c_p2c_payload_mp4_kind.svfmt` | `error: The only accepted payload view kind is rbsp` |
| P2d | `@container` 注解 | `svtool rule check scratch/p5c_p2d_container_annotation.svfmt` | `error: Unknown annotation '@container'` |
| P5a | `@target_format` 后置于 lazy 区域 | `svtool rule check scratch/p5c_p5a_target_format_lazy.svfmt` | `error: Lazy byte regions accept only @description and @spec`（`dsl.cpp:1895`） |
| P5b | `@target_format` 置于 `@lazy` 与 `bytes` 之间 | `svtool rule check scratch/p5c_p5b_target_format_preposition.svfmt` | `error: Expected bytes after @lazy(...)`（`dsl.cpp:1112`） |
| P7a | `unsupported("")` 空 reason | `svtool rule check scratch/p5c_p7a_unsupported_empty_reason.svfmt` | `error: Unsupported statements require a non-empty reason`（`dsl_ir.cpp:2748`） |
| P7b | `unsupported` 锚定 computed 字段 | `svtool rule check scratch/p5c_p7b_unsupported_computed_anchor.svfmt` | `error: Unsupported anchors require a source-backed scalar field`（`dsl_ir.cpp:2771`） |
| P7c | `repeat` 内 `unsupported` | `svtool rule check scratch/p5c_p7c_unsupported_repeat_local.svfmt` | `error: Unsupported statements cannot be repeat-local items`（`dsl_ir.cpp:2741`） |
| P7d | box 头字段后 `unsupported`（moof 形态） | `svtool rule check scratch/p5c_p7d_unsupported_positive.svfmt` | `Rule OK` |
| P8a | uuid 标记，两个 `bits<64>` usertype | `svtool rule check scratch/p5c_p8_uuid_marker.svfmt` | `Rule OK` |
| P8b | `bits<128>` 字段 | `svtool rule check scratch/p5c_p8_bits128.svfmt` | `error: Bit field width must be in the range 1..64`（`dsl.cpp:1038`） |
| P8c | `@container(Child)` 后置位 | `svtool rule check scratch/p5c_p8_container_annotation.svfmt` | `error: Unknown annotation '@container'`（名称闸门；`Child` 解析为 Identifier，`dsl.cpp:801`） |
| P8d | uuid 完整 size 分支 | `svtool rule check scratch/p5c_p8d_uuid_full_branches.svfmt` | `Rule OK` |
| P8e | uuid `size == 0` 缺口（`available_bytes()`） | `svtool rule check scratch/p5c_p8e_uuid_size0_gap.svfmt` | `error: Pure function is not declared before this call`（`dsl_ir.cpp:772`） |

**Fixture 生成（完整命令；输出为经 `grep` 的相关摘录，非原始完整输出）：**

- 普通 fixture：
  `ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=duration=0.2:size=64x48:rate=10 -f lavfi -i sine=frequency=440:duration=0.2 -c:v libx264 -preset ultrafast -c:a aac -y /tmp/p5c_fixture.mp4`
  → 6,513 字节文件（`ls -la /tmp/p5c_fixture.mp4` → `-rw-r--r--@ 1 yun wheel 6513`）。
- 分片 fixture：
  `ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=duration=0.3:size=64x48:rate=10 -c:v libx264 -preset ultrafast -movflags frag_keyframe+empty_moov -y /tmp/p5c_frag.mp4`
  → 4,505 字节文件。
- 这两条命令即 P5d-3 提交到 `tests/fixtures/` 的 fixture 生成脚本（D7）；上述 `/tmp` 输出是探针期产物，不是持久测试资产。

**Box 级 ground truth（`ffprobe -v trace`，经 `grep` 的相关摘录）**——普通 fixture，覆盖本 ADR 声称的节点（含 `smhd` 与两条轨的 `btrt`）：

```
type:'ftyp' parent:'root' sz: 32 8 6513
type:'free' parent:'root' sz: 8 40 6513
type:'mdat' parent:'root' sz: 5012 48 6513
type:'moov' parent:'root' sz: 1461 5060 6513
type:'mvhd' parent:'moov' sz: 108 8 1453
type:'trak' parent:'moov' sz: 606 116 1453        (视频轨)
type:'tkhd' parent:'trak' sz: 92 8 598
type:'edts' parent:'trak' sz: 36 100 598
type:'elst' parent:'edts' sz: 28 8 28
type:'mdia' parent:'trak' sz: 470 136 598
type:'mdhd' parent:'mdia' sz: 32 8 462
type:'hdlr' parent:'mdia' sz: 45 40 462
type:'minf' parent:'mdia' sz: 385 85 462
type:'vmhd' parent:'minf' sz: 20 8 377
type:'dinf' parent:'minf' sz: 36 28 377
type:'dref' parent:'dinf' sz: 28 8 28
type:'stbl' parent:'minf' sz: 321 64 377
type:'stsd' parent:'stbl' sz: 189 8 313
size=173 4CC=avc1 codec_type=0
type:'avcC' parent:'stsd' sz: 51 8 87
type:'pasp' parent:'stsd' sz: 16 59 87
type:'btrt' parent:'stsd' sz: 20 75 87            (视频 btrt)
type:'stts' parent:'stbl' sz: 24 197 313
type:'stss' parent:'stbl' sz: 20 221 313
type:'stsc' parent:'stbl' sz: 28 241 313
type:'stsz' parent:'stbl' sz: 28 269 313
type:'stco' parent:'stbl' sz: 24 297 313
type:'trak' parent:'moov' sz: 641 722 1453        (音频轨)
type:'stsd' parent:'stbl' sz: 126 8 352
size=110 4CC=mp4a codec_type=1
type:'esds' parent:'stsd' sz: 54 8 74
type:'btrt' parent:'stsd' sz: 20 62 74            (音频 btrt)
type:'smhd' parent:'minf' sz: 16 8 412            (音频声音媒体头)
```

**分片 fixture（`ffprobe -v trace`，经 `grep` 的相关摘录）**——覆盖 `mvex`/`trex` 与 `moof` 链及其后的 `mdat`：

```
type:'mvex' parent:'moov' sz: 40 610 740
type:'trex' parent:'mvex' sz: 32 8 32
type:'moof' parent:'root' sz: 124 792 4505
type:'mfhd' parent:'moof' sz: 16 8 116
type:'traf' parent:'moof' sz: 100 24 116
type:'tfhd' parent:'traf' sz: 36 8 92
type:'trun' parent:'traf' sz: 36 64 92
type:'mdat' parent:'root' sz: 3530 916 4505       (位于 moof 之后；D7 续扫目标)
```

**Sample 级 ground truth（`ffprobe -show_packets`，视频流，相关摘录）：**

```
$ ffprobe -v error -select_streams v:0 -show_packets -show_entries packet=pos,pts_time,duration_time,flags -of csv=p=0 /tmp/p5c_fixture.mp4
0.000000,0.100000,306,K__      (帧 1：pts 0.0s，duration 0.1s，绝对字节位置 306，关键帧)
0.100000,0.100000,3282,___     (帧 2：pts 0.1s，pos 3282，非关键帧)
```

工具链结论：`ffprobe` 8.1 与 `ffmpeg` 可用；`MP4Box` 未安装（`which MP4Box mp4box` → not found）。`ffmpeg` + `ffprobe -v trace`（box 类型/size/偏移/父子关系）+ `ffprobe -show_packets`（sample 偏移/时间戳/关键帧）即可完成生成与 ground truth 验证，全程不手算任何 bit 偏移，满足 P5d+ fixture 的「禁止手算 bit 位置」惯例。

---

## 参考

- ADR-0096：MP4/ISOBMFF 容器架构、Box 遍历、跨层导航与样本索引边界（其 D1 分支内 `computed` + `@lazy` 形态由本 ADR 复确认；其 `size == 0` 与 `uuid` 两个开放缺口在本 ADR 收口）。
- ADR-0098：未识别注解编译闸门与显式不支持语法（注解注册表 `knownAnnotations`，`dsl.cpp:1834-1866`，与 `unsupported` 语句契约）。
- ADR-0040：非致命语法警告与范围注解（D7 应用的致命/非致命二分法）。
- ISO/IEC 14496-12:2015（ISOBMFF box 结构与 `size`/`largesize`/`size == 0` 语义；`uuid` 16 字节 `usertype`）。
- 任务 P5c/P5c-R/P5c-R2 定义与计划 fact 10–16，`Sub-Agent分步开发指导计划.md`。

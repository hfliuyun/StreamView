# ADR-0097：MP4/ISOBMFF 容器原语可表达性探测与语言增量

- **状态**：Proposed
- **日期**：2026-08-17
- **作者**：StreamView 贡献者

---

## 背景

阶段 5 要求支持非分片 MP4/ISOBMFF 容器解析（`video/mp4`、`audio/mp4`，ISO/IEC 14496-12 / 14496-14 / 14496-15）。ADR-0096（P5a）已定下架构边界——D1 box 遍历与 `mdat` lazy 封装、D2 跨层导航（`avcC`/`esds` 至 H.264/AAC）、D3 样本表窗口化——但其 D1 的 DSL 片段引用了今天不存在的语言构造。本 ADR（任务 P5c，经 P5c-R、P5c-R2 与 P5c-R3 修正）探测当前 DSL/运行时对 ISOBMFF 容器结构「可/不可表达」的真实边界，并为每一项能力增量锁定**可直接编码与测试的精确合同**，且每一项都映射到工作树中的真实 C++ 类型/函数。本 ADR **不产出任何规则资产**：全部规则消费推迟到 P5d+（「枚举/下钻机制未定 ⇒ 不得提交 MP4 规则资产」闸门）。

七条容器事实（计划 fact 10–16，主 Agent 已于 2026-08-16 用 `svtool rule check` 实测）作为既知事实纳入，不再重复探测；本轮跑过的探针均现场复确认：

1. **FourCC 匹配今天已可表达**：`bits<32> box_type @equals(0x66747970)` 可编译（`Rule OK`）。无需新增 `bytes` 值类型。
2. **`bits<64>` 放行**——`largesize` 可直接读取；字段宽度上限为 64 位（`Bit field width must be in the range 1..64`，`dsl.cpp:1038`）。
3. **`size == 1` / `else` 的载荷字节数可表达**，但只有一种写法成立：把 `computed` 与 `@lazy` **各自放进 `if`/`else` 两个分支**（跨分支的单一 `computed` 会报 `Computed dependency is not guaranteed on the current branch`）。
4. **`size == 0`（延伸至文件尾）是唯一真实语言缺口**：`compressed_payload` 是唯一的「吃掉剩余」终结符，但受限于 `error: compressed_payload must occur once as the final top-level item`（`dsl.cpp:1589`），进不了 `if` 分支；且不存在 `available_bytes` 内建。
5. **`maximumExpandedFieldsPerStructure = 99'999`（按 struct 计，`dsl_ir.cpp:13`）** 限制每个结构体的字段展开规模；超过上限的样本表必须采用 lazy 区域窗口化。
6. **今天全语言只有「顶层 `sequence` + 单一 `payload<rbsp>` 分派」一种层次机制**（H.264 形态）。嵌套 box 枚举是本 ADR 要解决的硬阻塞。
7. **`bits<128>` 不可表达**（探针：`error: Bit field width must be in the range 1..64`，`dsl.cpp:1038`）；128 位值声明为两个 `bits<64>` 字段。

本轮全部 18 项探针使用工作树工具二进制 `build/dev/tools/svtool/svtool`（`svtool 0.1.0 (DSL 0.1)`）执行；scratch 源位于仓库之外的会话 scratch 目录。探针源码、命令、stdout/stderr 与 exit code 在任务报告中完整复现（下表列出每条命令与逐字结果）：

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
| P8d uuid 完整 size 分支（正确线序） | `svtool rule check scratch/p5c_p8d_uuid_full_branches.svfmt` | `Rule OK`（验证正确线序语法与 IR 合法性） | — |
| P8e uuid `size == 0` 缺口（`available_bytes()`） | `svtool rule check scratch/p5c_p8e_uuid_size0_gap.svfmt` | `error: Pure function is not declared before this call` | `dsl_ir.cpp:772` |
| P9a `@window(Entry, entry_count)` 后置位 | `svtool rule check scratch/p5c_p9a_window_annotation.svfmt` | `error: Unknown annotation '@window'` | `dsl.cpp:1880` |
| P9b `@window(123, 456)` 非标识符参数 | `svtool rule check scratch/p5c_p9b_window_token_kind.svfmt` | `error: Unknown annotation '@window'` | `dsl.cpp:1880` |

---

## 决策

### D1：顶层 box 枚举——新增 `DslScannerKind::Mp4Box` 与锁定的分帧合同

scanner kind 闭集 `{H264StartCode, AacAdtsFrame}`（`dsl_ir.h:219-221`）扩展出 `Mp4Box`。新 scanner **只做分帧（framing）**且**不得识别任何具体 FourCC**：「哪个 FourCC 意味着什么」是格式语义，留在 DSL 规则里（`bits<32> type @equals(0x...)` 派发）。scanner 侧的合法性只限于通用 8/16 字节头最小值；`uuid` 的 24/32 字节最小值是 **DSL 语义**（D3），不是 scanner 语义。

**`size`/`largesize` 语义**：按 ISO/IEC 14496-12，`size`（及 `size == 1` 时的 `largesize`）是**整个 box（含头）的尺寸**。

**分帧算法与伪代码（独立 `size == 0` 分支、减法式算术）**：

在有界区域 `[region_start, region_end)` 内的 box 起点偏移 `start` 处：
令 `remaining = region_end - start`。

```text
if remaining < 8:
    // 截断头：区域结束，扫描干净停止。
    stop_scanning()

read u32 size, u32 type

if size == 0:
    // 延伸至区域末尾的终结 box
    span = [start, region_end)
    terminal = true
    truncated = false
    emit_record(span, terminal=true, truncated=false)
    stop_scanning()

if size == 1:
    if remaining < 16:
        // 不完整 large 头（无法读取 largesize）
        stop_scanning()
    read u64 largesize
    if largesize < 16:
        // 畸形 large size：非合法 box，扫描立即停止
        stop_scanning()
    box_size = largesize
else if size >= 8:
    box_size = size
else:
    // 畸形 size（2..7）：非合法 box，扫描立即停止
    stop_scanning()

// Size != 0 跨度计算
if box_size <= remaining:
    span = [start, start + box_size)
    terminal = false
    truncated = false
    emit_record(span, terminal=false, truncated=false)
    advance(start + box_size)
else:
    // box 超出区域剩余字节
    span = [start, region_end)
    terminal = true
    truncated = true
    emit_record(span, terminal=true, truncated=true)
    stop_scanning()
```

**字节到比特坐标保护**：
在构造位地址前，偏移与长度值均经过保护校验：
`start <= std::numeric_limits<quint64>::max() / 8` 且 `span_length <= std::numeric_limits<quint64>::max() / 8`。
若 `core::SourceSpan::create` 因算术越界失败，scanner 产生确定性的错误状态 / `InvalidSourceSpan` 诊断，绝不发出非法位地址。

**截断记录与候选探测器分级**：
- 截断 box 记录（`truncated = true`）正常发出，以便分析器将残缺 box 头部物化上树并附带 `TruncatedSource` 诊断（`"Lazy byte region exceeds the available source range"`，`dsl_vm.cpp:3249-3252`）。
- 在候选探测器（`detectMp4Candidate`，P5d-1）中，截断 box **不计入** `Strong` 候选链阈值。`Strong` 必须由 $\ge 3$ 个连续完整、格式良好且未截断的 box 跨度铺满被检前缀；`Probable` = 2；`Weak` = 1。

**scanner/DSL 数据合同——span-only，不发布头部值**：
scanner 输出每个 box 的**跨度**（起始偏移、结束偏移、截断标记）。它**不发布** `size`/`type`/`largesize` 值。runner 将跨度映射为源子视图（`aac_adts_analyzer.cpp:309/:346` 先例），DSL 的 `Box` 结构体在跨度内**从源重新读取** `size`/`type`/`largesize`。

P5d-1/P5d-2 之后，`sequence<Box> boxes = scan(mp4_box);`（配 `@index(progressive)`）即可表达。

---

### D2：嵌套下钻——`@container` 注解、Runner 重入与共享执行状态

容器 box（`moov`/`trak`/`mdia`/`minf`/`stbl`）需要对其载荷字节区域枚举多个子 box。

**锁定语法**（后置于 lazy `bytes` 区域，一个结构体标识符参数）：

```svfmt
@lazy(payload_bytes) bytes payload @container(ChildStruct);
```

- **宿主**：`DslAnnotationTarget::LazyRegion` 专有。
- **参数契约**：严格一个参数；token 必须解析为 `DslAnnotationValueKind::Identifier`（`dsl.cpp:801`），命名一个已声明结构体。
- **诊断**（P5d-2）：
  - 未注册名称：`Unknown annotation '@container'`（`dsl.cpp:1880`，探针 P2d/P8c）；
  - 宿主非 LazyRegion：`@container is not supported on this declaration`（`dsl.cpp:1905-1907`）；
  - 参数个数 != 1 或非标识符：`InvalidAnnotation`；
  - 目标结构体未声明：`UnknownReference`（`dsl.cpp:3556-3564` 先例）。

**解析器 AST**：`DslAnnotation { name = "container", arguments = [{ kind = Identifier, text = structName }] }` 存入 `DslLazyRegion`（`dsl.h:234-240`）的既有 `annotations` 向量。无 AST 类型变动。

**类型化 IR**：`DslTypedField`（`dsl_ir.h:108-124`）增加 `std::optional<quint32> containerChildStructIndex`（仅 `type.kind == DslValueTypeKind::LazyBytes` 时有效）。在 `compileLazyRegion`（`dsl_ir.cpp:2462-2540`）中解析绑定。

**`RegisterLazyBytes` 职责与 Runner 重入**：
- `DslOpcode::RegisterLazyBytes`（`dsl_vm.cpp:3169-3320`）**仅注册 lazy 节点**并存储元数据（`containerChildStructIndex`），**不立即执行重入**。
- 子 box 枚举由 runner / 分析会话（`Mp4IsobmffAnalyzer`，P5d-3）在容器字节跨度 `[anchor_start, anchor_end)` 上重入 `Mp4Box` scanner，以指定结构体为元素类型物化子节点并挂载于容器 lazy 节点之下。

**Runner 拥有的跨重入共享执行状态追踪器**：
`DslExecutionLimits`（`dsl_vm.h:30-51`）提供静态上限配置，runner 维护跨所有嵌套重入的运行时状态：
- `quint64 totalNodesMaterialized`（初值 0，每物化一个节点递增，受 `limits.maximumMaterializedNodes = 100'000` 约束）；
- `quint32 currentNestingDepth`（初值 0，进入子扫描前递增、返回后递减，受 `limits.maximumNodeDepth = 256` 约束）；
- 共享的 `cancellationToken` 每 1,024 条指令检查一次；
- 超过任何上限时将容器区域标记为 `ResourceLimit` 并终止子枚举。

**状态转移矩阵**（`AnalysisTree::canTransition`，`analysis_model.cpp:218-251`）：
`Lazy` $    o$ `Indexing` $    o$ `Materialized` / `Invalid` / `Cancelled` / `WaitingDependency`。

**内核/Runner 中立性**：子结构体仅通过 IR 索引引用；**核心与 runner 代码中绝不出现任何 FourCC 字面量**。

---

### D3：标签匹配——`bits<32>` + `@equals(0x...)`、UUID 线序与三路分支

Fact 10 成立：`bits<32> box_type @equals(0x66747970)` 可编译；支持十六进制字面量。常用 FourCC 常量表：
`ftyp` (0x66747970), `moov` (0x6D6F6F76), `mdat` (0x6D646174), `free` (0x66726565), `mvhd` (0x6D766864), `trak` (0x7472616B), `tkhd` (0x746B6864), `edts` (0x65647473), `elst` (0x656C7374), `mdia` (0x6D646961), `mdhd` (0x6D646864), `hdlr` (0x68646C72), `minf` (0x6D696E66), `vmhd` (0x766D6864), `smhd` (0x736D6864), `dinf` (0x64696E66), `dref` (0x64726566), `stbl` (0x7374626C), `stsd` (0x73747364), `stts` (0x73747473), `stss` (0x73747373), `stsc` (0x73747363), `stsz` (0x7374737A), `stco` (0x7374636F), `avc1` (0x61766331), `avcC` (0x61766343), `mp4a` (0x6D703461), `esds` (0x65736473), `pasp` (0x70617370), `btrt` (0x62747274), `mvex` (0x6D766578), `trex` (0x74726578), `moof` (0x6D6F6F66), `mfhd` (0x6D666864), `traf` (0x74726166), `tfhd` (0x74666864), `trun` (0x7472756E), `udta` (0x75647461), `meta` (0x6D657461), `ilst` (0x696C7374)。

**UUID Box 线序与 DSL 结构**：
按 ISO/IEC 14496-12：
- 对 large UUID（`size == 1`），码流上 `largesize`（第 8..15 字节）位于 `usertype`（第 16..31 字节）之前。
- 对普通 UUID（`size >= 24`）与 EOF UUID（`size == 0`），`usertype` 紧跟 `type`（第 8..23 字节）。
- `usertype` 绝不能在判断 `size == 1` 之前无条件统一读取。
- DSL 结构采用三路互斥分支，防止任何无符号减法下溢：

```svfmt
struct UuidBox {
    bits<32> size;
    bits<32> type @equals(0x75756964);
    if (size == 1) {
        bits<64> largesize;
        bits<64> large_usertype_hi;
        bits<64> large_usertype_lo;
        computed<u64> large_payload = largesize - 32;
        @lazy(large_payload) bytes large_data;
    } else {
        bits<64> usertype_hi;
        bits<64> usertype_lo;
        if (size == 0) {
            computed<u64> payload_bytes = available_bytes();
            @lazy(payload_bytes) bytes payload;
        } else {
            computed<u64> payload = size - 24;
            @lazy(payload) bytes data;
        }
    }
}
```

探针 P8d 证实此正确线序与三路分支语法的语法/IR 合法性（`Rule OK`）。探针 P8e 证实 `available_bytes()` 是当前唯一缺失的内建（`error: Pure function is not declared before this call`，`dsl_ir.cpp:772`）。

---

### D4：尺寸分支——普通 Box 三路分支与 `available_bytes()` 内建

**普通 Box 三路分支结构**：
为防止 `size == 0` 执行 `size - 8`（导致无符号数运行时减法下溢），普通 box 采用如下嵌套分支结构：

```svfmt
struct Box {
    bits<32> size;
    bits<32> type;
    if (size == 1) {
        bits<64> largesize;
        computed<u64> large_payload = largesize - 16;
        @lazy(large_payload) bytes large_data;
    } else {
        if (size == 0) {
            computed<u64> payload_bytes = available_bytes();
            @lazy(payload_bytes) bytes payload;
        } else {
            computed<u64> payload = size - 8;
            @lazy(payload) bytes data;
        }
    }
}
```

**`available_bytes()` 内建契约（P5d-2）**：
- 返回当前 source reader 从当前位位置起剩余的字节数：`reader.remainingBits() / 8`。
- 在 `compileExpression`（`dsl_ir.cpp:589-611`，`DslTypedExpressionKind::AvailableBytes`）中注册，并在 `dsl_vm.cpp:496-499` 中求值。
- 非负 `u64` 标量值，可在任意 `computed` 表达式中使用。

---

### D5：`@target_format`——注册表、元数据、缓存与 `resolveByFormat`

**锁定语法**（后置于 lazy `bytes` 区域，一个字符串参数）：

```svfmt
@lazy(payload_bytes) bytes payload @target_format("video/mp4");
```

- **宿主**：`DslAnnotationTarget::LazyRegion` 专有。
- **注册表变更**（P5d-2）：`knownAnnotations`（`dsl.cpp:1834-1866`）更新 `{u"target_format", static_cast<quint32>(DslAnnotationTarget::LazyRegion)}`；lazy region 白名单消息（`dsl.cpp:1895`）同步放开 `@target_format`。
- **类型化 IR**：`DslTypedField` 增加 `std::optional<QString> targetFormat`。
- **节点元数据**：`core::AnalysisNodeMetadata`（`analysis_model.h:76-80`）增加 `std::optional<QString> targetFormat`。
- **缓存编码**：`analysis_cache_payload.cpp:35-36` 增加 `nodeTargetFormatFlag = 4U`。缺少该标志位的旧缓存解码为 `targetFormat = std::nullopt`。
- **`RulePackageCatalog::resolveByFormat` 服务（P5d-2）**：
  `resolveByFormat(QStringView format, QStringView runningLanguage, QStringView runningEngine)` 扫描规则包匹配 `RulePackageEntryPoint::format`（`rule_package.h:61-69`），返回 `Found`、`MissingContent` 或 `VersionConflict`。
- **P5d 与 P5i 分工**：P5d-2 交付注解、元数据、缓存标志位与 `resolveByFormat`；P5i 在 UI 导航动作中消费。

---

### D6：样本表窗口化——专有 `@window` 注解与解码器契约

`@container` 严格用于 `Mp4Box` scanner 的 box 流重入。针对固定宽度样本表（`stts`、`stsc`、`stsz`、`stco`、`co64`）的窗口化，定义专有的 `@window` 注解。

**锁定语法**（后置于 lazy `bytes` 区域，两个标识符参数）：

```svfmt
@lazy(table_bytes) bytes entries @window(EntryStruct, entry_count_field);
```

- **宿主**：`DslAnnotationTarget::LazyRegion` 专有。
- **参数契约**：严格两个参数，均为 `DslAnnotationValueKind::Identifier`（表项结构体类型名，以及所在结构体内声明的计数标量字段名）。
- **诊断**（P5d-2）：
  - 未注册名称：`Unknown annotation '@window'`（`dsl.cpp:1880`，探针 P9a/P9b）；
  - 宿主非 LazyRegion：`@window is not supported on this declaration`；
  - 参数个数 != 2 或非标识符：`InvalidAnnotation`；
  - 表项结构体未声明：`UnknownReference`；
  - 计数字段未声明 / 非标量：`UnknownReference` / `InvalidType`。
- **类型化 IR**：`DslTypedField` 增加：
  - `std::optional<quint32> windowEntryStructIndex`；
  - `std::optional<quint32> windowEntryCountFieldIndex`；
  - `std::optional<quint64> windowEntrySizeBits`（编译期计算为 `EntryStruct` 全部字段位宽之和）。
- **节点元数据 / 会话查询**：`AnalysisNodeMetadata` / 会话接口暴露窗口元数据（`windowEntryStructIndex`、`windowEntrySizeBits`、`entryCount`）。UI 与会话调用方直接读取该元数据，无需猜测结构体索引或条目数。
- **窗口解码器接口**（`src/rules/include/streamview/rules/window_decoder.h`，P5d-3 子项）：
  - `WindowDecodeRequest { AnalysisNodeId lazyRegionNode; quint32 entryStructIndex; quint64 entryCount; quint64 entrySizeBits; quint64 pageIndex; quint64 pageSize; }`。
  - `WindowDecodeResult { DslExecutionStatus status; std::vector<AnalysisNodeId> entryNodes; quint64 pageStartIndex; bool nextPageAvailable; }`。
  - 固定默认 `pageSize = 256` 条目；坐标从 lazy 逻辑范围 + 校验后偏移经 `SourceMapping::locate` 映射。

---

### D7：分片 MP4（`moof`）——规则内 `unsupported` 契约

- **编译证据（P5c，已验证）**：`unsupported("fragmented MP4 (moof/traf) is outside the v0.1 subset") at type;` 可编译——探针 P7d，`Rule OK`。
- **P5d-3 Runner 规范性行为（测试断言）**：
  1. `moof` box 节点物化为 `MaterializationState::Unsupported`，`DiagnosticCode::UnsupportedSyntax`，`DiagnosticSeverity::Warning`，锚定于 `type`；
  2. `moof` 前缀字段 `size` 与 `type` 保持物化；
  3. 之前的顶层 box（`ftyp`, `moov`, ...）保持物化；
  4. 顶层扫描继续：区域内紧随 `moof` 之后的 box（如 `mdat`）仍正常物化。
- 测试 fixture 由 `tests/fixtures/` 内提交的脚本生成（P5d-3），绝不引用 `/tmp` 路径。

---

## 被否决方案

1. **结构体类型字段**（`BoxHeader header;`）用于嵌套——否决：解析器拒绝结构体作为字段类型（探针 P2a）；`@container` 实现嵌套无需新类型。
2. **嵌套 `sequence` 声明**——否决：需要新文法与 VM 语义；与 sequence 仅限顶层契约冲突（探针 P2b）。
3. **新增 `payload<box>` 视图类型**——v0.1 否决：分派视图单选一个结构体，而容器枚举多个子 box（探针 P2c）。
4. **`bits<128>` 用于 UUID**——否决：不可表达（`Bit field width must be in the range 1..64`，`dsl.cpp:1038`，探针 P8b）；锁定形式为两个 `bits<64>` 字段（探针 P8a）。
5. **整 UUID 字面量等值比较**——v0.1 否决：无字符串字面量 tokenizer 或 bytes 值类型。
6. **探测器级整文件拒绝分片 MP4**——否决（D7）：丢失已解码前缀且阻止后续 box 扫描。
7. **scanner 发布头部字段值**——否决：span-only 契约（D1）与既有 runner 记录映射一致（`aac_adts_analyzer.cpp:309/:346`）。
8. **scanner 计算剩余长度下传以支持 `size == 0`**——否决：选定更通用的 `available_bytes()` 内建（D4）。
9. **复用 `@container` 用于样本表窗口化**——否决（D6）：`@container` 触发 `Mp4Box` scanner 重入；样本表需要专有的 `@window(Entry, count)` 绑定以处理固定宽度记录。
10. **`@target_format` 携带完整包身份**——否决（D5）：将规则文本与打包细节耦合；`resolveByFormat` 服务保持注解为清晰的格式字符串。

---

## 影响与结论

### 正向收益
- ISOBMFF box 树在 P5d 后完全可表达：顶层分帧（D1）、带共享执行状态的容器重入（D2）、正确 UUID 线序（D3）、无溢出的安全三路分支（D4）、跨格式导航元数据（D5）、专有 `@window` 样本表绑定（D6）以及规范性分片 MP4 处理（D7）。
- 核心引擎保持 100% 格式中立（核心无 FourCC）。
- 所有能力均映射到工作树真实 C++ 类型并具备可直接测试的合同。

### 负向代价与切片
- P5d 能力实现分为三个子切片交付：
  - **P5d-1**：`Mp4Box` scanner + 探测器（`detectMp4Candidate`）。
  - **P5d-2**：语言/编译器/IR 增量（`@container`、`@window`、`available_bytes()`、`@target_format`、元数据、缓存、`RulePackageCatalog::resolveByFormat`）。
  - **P5d-3**：`Mp4IsobmffAnalyzer` runner 骨架 + 容器重入 + 共享执行预算 + 窗口解码器（`window_decoder.h`） + D7 续扫/fixture 测试。

---

## 验证矩阵与证据

所有 18 项探针均使用 `build/dev/tools/svtool/svtool`（`svtool 0.1.0 (DSL 0.1)`）实跑验证；scratch 源位于会话 scratch 目录。

| # | 探针 | 命令 | 结果 |
| :--- | :--- | :--- | :--- |
| P1 | `scan(mp4_box)` | `svtool rule check scratch/p5c_p1_scan_mp4_box.svfmt` | `error: Only h264_start_code and adts_frame are supported` (`dsl.cpp:3568`) |
| P2a | 结构体类型字段 | `svtool rule check scratch/p5c_p2a_struct_typed_field.svfmt` | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` |
| P2b | struct 内的 `sequence` | `svtool rule check scratch/p5c_p2b_nested_sequence.svfmt` | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` |
| P2c | `payload<mp4>` view kind | `svtool rule check scratch/p5c_p2c_payload_mp4_kind.svfmt` | `error: The only accepted payload view kind is rbsp` |
| P2d | `@container` | `svtool rule check scratch/p5c_p2d_container_annotation.svfmt` | `error: Unknown annotation '@container'` |
| P5a | `@target_format` 后置 lazy | `svtool rule check scratch/p5c_p5a_target_format_lazy.svfmt` | `error: Lazy byte regions accept only @description and @spec` (`dsl.cpp:1895`) |
| P5b | `@target_format` 前置位 | `svtool rule check scratch/p5c_p5b_target_format_preposition.svfmt` | `error: Expected bytes after @lazy(...)` (`dsl.cpp:1112`) |
| P7a | `unsupported("")` | `svtool rule check scratch/p5c_p7a_unsupported_empty_reason.svfmt` | `error: Unsupported statements require a non-empty reason` (`dsl_ir.cpp:2748`) |
| P7b | `unsupported` 锚定 computed | `svtool rule check scratch/p5c_p7b_unsupported_computed_anchor.svfmt` | `error: Unsupported anchors require a source-backed scalar field` (`dsl_ir.cpp:2771`) |
| P7c | `repeat` 内的 `unsupported` | `svtool rule check scratch/p5c_p7c_unsupported_repeat_local.svfmt` | `error: Unsupported statements cannot be repeat-local items` (`dsl_ir.cpp:2741`) |
| P7d | box 头字段后 `unsupported` | `svtool rule check scratch/p5c_p7d_unsupported_positive.svfmt` | `Rule OK` |
| P8a | uuid 标记，两个 `bits<64>` | `svtool rule check scratch/p5c_p8_uuid_marker.svfmt` | `Rule OK` |
| P8b | `bits<128>` 字段 | `svtool rule check scratch/p5c_p8_bits128.svfmt` | `error: Bit field width must be in the range 1..64` (`dsl.cpp:1038`) |
| P8c | `@container(Child)` 后置位 | `svtool rule check scratch/p5c_p8_container_annotation.svfmt` | `error: Unknown annotation '@container'`（名称闸门先行；`Child` 解析为 Identifier，`dsl.cpp:801`） |
| P8d | uuid 完整 size 分支（正确线序） | `svtool rule check scratch/p5c_p8d_uuid_full_branches.svfmt` | `Rule OK` |
| P8e | uuid `size == 0` 缺口（`available_bytes()`） | `svtool rule check scratch/p5c_p8e_uuid_size0_gap.svfmt` | `error: Pure function is not declared before this call` (`dsl_ir.cpp:772`) |
| P9a | `@window(Entry, entry_count)` 后置位 | `svtool rule check scratch/p5c_p9a_window_annotation.svfmt` | `error: Unknown annotation '@window'` (`dsl.cpp:1880`) |
| P9b | `@window(123, 456)` 非标识符参数 | `svtool rule check scratch/p5c_p9b_window_token_kind.svfmt` | `error: Unknown annotation '@window'` (`dsl.cpp:1880`) |

**Fixture 生成命令与实测大小：**

- 普通 fixture：
  `ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=duration=0.2:size=64x48:rate=10 -f lavfi -i sine=frequency=440:duration=0.2 -c:v libx264 -preset ultrafast -c:a aac -y /tmp/p5c_fixture.mp4`
  → exit code 0，6,513 字节（`-rw-r--r--@ 1 yun wheel 6513`）。
- 分片 fixture：
  `ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=duration=0.3:size=64x48:rate=10 -c:v libx264 -preset ultrafast -movflags frag_keyframe+empty_moov -y /tmp/p5c_frag.mp4`
  → exit code 0，4,505 字节（`-rw-r--r--@ 1 yun wheel 4505`）。

**Box 级事实基准（`ffprobe -v trace` 经 grep 筛选的相关摘录）**——普通 fixture（6,513 字节）：

```
type:'ftyp' parent:'root' sz: 32 8 6513
type:'free' parent:'root' sz: 8 40 6513
type:'mdat' parent:'root' sz: 5012 48 6513
type:'moov' parent:'root' sz: 1461 5060 6513
type:'mvhd' parent:'moov' sz: 108 8 1453
type:'trak' parent:'moov' sz: 606 116 1453        (video track)
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
type:'btrt' parent:'stsd' sz: 20 75 87            (video btrt)
type:'stts' parent:'stbl' sz: 24 197 313
type:'stss' parent:'stbl' sz: 20 221 313
type:'stsc' parent:'stbl' sz: 28 241 313
type:'stsz' parent:'stbl' sz: 28 269 313
type:'stco' parent:'stbl' sz: 24 297 313
type:'trak' parent:'moov' sz: 641 722 1453        (audio track)
type:'stsd' parent:'stbl' sz: 126 8 352
size=110 4CC=mp4a codec_type=1
type:'esds' parent:'stsd' sz: 54 8 74
type:'btrt' parent:'stsd' sz: 20 62 74            (audio btrt)
type:'smhd' parent:'minf' sz: 16 8 412            (audio sound media header)
```

**分片 fixture（`ffprobe -v trace` 经 grep 筛选的相关摘录）**——4,505 字节：

```
type:'mvex' parent:'moov' sz: 40 610 740
type:'trex' parent:'mvex' sz: 32 8 32
type:'moof' parent:'root' sz: 124 792 4505
type:'mfhd' parent:'moof' sz: 16 8 116
type:'traf' parent:'moof' sz: 100 24 116
type:'tfhd' parent:'traf' sz: 36 8 92
type:'trun' parent:'traf' sz: 36 64 92
type:'mdat' parent:'root' sz: 3530 916 4505       (follows moof; D7 continuation target)
```

**样本级事实基准（`ffprobe -show_packets` 视频流相关摘录）：**

```
0.000000,0.100000,306,K__      (frame 1: pts 0.0s, duration 0.1s, absolute byte pos 306, keyframe)
0.100000,0.100000,3282,___     (frame 2: pts 0.1s, pos 3282, non-keyframe)
```

---

## 参考资料

- ADR-0096：MP4/ISOBMFF 容器架构、Box 遍历与跨层导航引用模型。
- ADR-0098：未识别注解编译闸门与显式不支持语法。
- ADR-0040：非致命语法警告与值域注解。
- ISO/IEC 14496-12:2015（ISOBMFF 容器 box 结构与 `size`/`largesize`/`size == 0` 语义；`uuid` 16 字节 `usertype`）。
- Task P5c/P5c-R/P5c-R2/P5c-R3 定义与容器事实 10–16，`Sub-Agent分步开发指导计划.md`。

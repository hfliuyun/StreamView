# ADR-0097：MP4/ISOBMFF 容器原语可表达性探测与语言增量

- **状态**：Proposed
- **日期**：2026-08-17
- **作者**：StreamView 贡献者

---

## 背景

阶段 5 要求支持非分片 MP4/ISOBMFF 容器解析（`video/mp4`、`audio/mp4`，ISO/IEC 14496-12 / 14496-14 / 14496-15）。ADR-0096（P5a）已定下架构边界——D1 box 遍历与 `mdat` lazy 封装、D2 跨层导航（`avcC`/`esds` 至 H.264/AAC）、D3 样本表窗口化——但其 D1 的 DSL 片段引用了今天不存在的语言构造。本 ADR（任务 P5c）探测当前 DSL/运行时对 ISOBMFF 容器结构「可/不可表达」的真实边界，并决定最小的语言与核心增量。本 ADR **不产出任何规则资产**：全部规则消费推迟到 P5d+（「枚举/下钻机制未定 ⇒ 不得提交 MP4 规则资产」闸门）。

六条容器事实（计划 fact 10–16，主 Agent 已于 2026-08-16 用 `svtool rule check` 实测）作为既知事实纳入，不再重复探测；其中本轮跑过的探针均现场复确认：

1. **FourCC 匹配今天已可表达**：`bits<32> box_type @equals(0x66747970)` 可编译（`Rule OK`）。无需新增 `bytes` 值类型。
2. **`bits<64>` 放行**——`largesize` 可直接读取。
3. **`size == 1` / `else` 的载荷字节数可表达**，但只有一种写法成立：把 `computed` 与 `@lazy` **各自放进 `if`/`else` 两个分支**（跨分支的单一 `computed` 会报 `Computed dependency is not guaranteed on the current branch`）。
4. **`size == 0`（延伸至文件尾）是唯一真实语言缺口**：`compressed_payload` 是唯一的「吃掉剩余」终结符，但受限于 `error: compressed_payload must occur once as the final top-level item`（`dsl.cpp:1589`），进不了 `if` 分支；且不存在 `available_bytes` 内建。
5. **`maximumExpandedFieldsPerStructure = 99'999`（按 struct 计，`dsl_ir.cpp:13`）** 使样本表窗口化成为编译期硬约束，而非可选优化。
6. **今天全语言只有「顶层 `sequence` + 单一 `payload<rbsp>` 分派」一种层次机制**（H.264 形态）。嵌套 box 枚举是本 ADR 要解决的硬阻塞。

本轮 scratch 探针证据（`svtool rule check`）：

| 探针 | 结果（逐字错误 / 状态） | 现场位置 |
| :--- | :--- | :--- |
| `scan(mp4_box)` | `error: Only h264_start_code and adts_frame are supported` | parser `dsl.cpp:3568`、IR `dsl_ir.cpp:3679` |
| `BoxHeader header;`（结构体类型字段） | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` | 字段类型解析 |
| struct 内的 `sequence<Child> children = scan(...)` | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` | sequence 仅限顶层 |
| `payload<mp4> ...` view kind | `error: The only accepted payload view kind is rbsp` | parser `dsl.cpp:3669`、IR `dsl_ir.cpp:3717` |
| `@container` 注解 | `error: Unknown annotation '@container'` | `dsl.cpp:1880` |
| `@lazy(4) bytes payload @target_format("video/mp4")` | `error: Lazy byte regions accept only @description and @spec` | `dsl.cpp:1895` |
| `@lazy(4) @target_format(...) bytes payload`（前置位） | `error: Expected bytes after @lazy(...)` | `dsl.cpp:1112` |
| `unsupported("") at type;` | `error: Unsupported statements require a non-empty reason` | `dsl_ir.cpp:2748` |
| `unsupported(...) at <computed 字段>` | `error: Unsupported anchors require a source-backed scalar field` | `dsl_ir.cpp:2771` |
| `repeat` 内的 `unsupported(...) at type;` | `error: Unsupported statements cannot be repeat-local items` | `dsl_ir.cpp:2741` |
| box 头字段之后的 `unsupported("fragmented MP4 (moof/traf) is outside the v0.1 subset") at type;` | `Rule OK` | — |

---

## 决策

### D1：顶层 box 枚举——新增 `DslScannerKind::Mp4Box`

scanner kind 闭集 `{H264StartCode, AacAdtsFrame}`（`dsl_ir.h:219-221`）扩展出 `Mp4Box`。新 scanner **只做分帧（framing）**，与 `AacAdtsScanner` 对称（它只认 `0xFFF` syncword 与 `aac_frame_length` 长度链，不认任何 profile 语义）：

- 每个 box 起点读取 `u32 size` 与 4 字节 `type`；若 `size == 1` 再读 `u64 largesize`；若 `size == 0` 则 box 跨度延伸至 EOF，且该 box 为终结性。
- box 跨度即纯长度前缀推进：`size`（或 `largesize`），`size == 0` 时为「剩余字节」。
- scanner **不得识别任何具体 FourCC**。「哪个 FourCC 意味着什么」是格式语义，留在 DSL 规则里（`bits<32> type @equals(0x...)` 派发）。scanner 只认识「框架」、不认识「含义」——与 H.264 起始码、ADTS 长度链的立场一致。
- 候选检测：一个格式良好的 box 头（`size >= 8` 或 `size == 0`，且跨度不超出剩余源）后接至少一个格式良好的头（或 EOF）构成候选，镜像 ADTS「≥N 帧长度链」立场；精确阈值由 P5d 校准。

P5d 之后，`sequence<Box> boxes = scan(mp4_box);`（配 `@index(progressive)`）即可表达。scanner 把 `size`/`type`/`largesize` 作为元素值发布，规则的 `Box` 结构体声明并物化这些字段（与 ADTS 头部字段的 record 契约相同）。

### D2：嵌套下钻——`@container` 注解 + runner 重入（最小机制）

今天全语言只有一种层次机制：顶层 `sequence` + `payload<rbsp>` 分派（fact 6；探针见上）。容器 box（`moov`/`trak`/`mdia`/`minf`/`stbl`）需要对一个字节区间**枚举多个子 box**。三个候选：

- **A — `@container` 注解 + runner 重入**：规则用 `@container` 标注某个 lazy `bytes` 区域；runner 对该字节跨度重入 `Mp4Box` scanner，在容器节点之下物化子 box 节点。
- **B — 嵌套 `sequence` 声明**（struct 体内的 `sequence<Child> children = scan(...)`）：今天被字段类型解析拒绝（探针见上）；需要新语法、IR 与 VM 排序语义，并破坏「sequence 仅限顶层」契约——改动最重。
- **C — 新增 `payload<box>` view kind** 并放开「payload 必须挂在已声明顶层 sequence」限制：payload 分派是按 switch 值**选一个**结构体；容器需要**枚举多个**子 box，故分派视图本身不解决嵌套，只改善顶层派发的人机工学。

**决策：A。** 职责划分：

- **DSL**：「哪些区域是容器」是格式语义——规则用 `@container` 标注其 lazy 载荷区域并指名子 box 结构体；FourCC 派发与各 box 的载荷布局留在规则里。
- **核心/runner**：重入式 box 扫描（字节跨度 → `Mp4Box` 枚举 → 容器节点下的子节点）格式中立，**零 FourCC 字面量**。

C 保留为将来顶层派发的人机工学优化选项，v0.1 不做。

### D3：type 标签匹配——`bits<32>` + `@equals(0x...)`，不新增值类型

fact 10 成立：`bits<32> box_type @equals(0x66747970)` 可编译，十六进制整数字面量放行。P5d+ 规则引用的常用 FourCC 常量表（大端 ASCII 打包进 32 位字面量）：

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

**`uuid` 扩展类型 box**：`uuid` 标记可表达为 `bits<32> type @equals(0x75756964)` 后接不透明 `bits<128> usertype;` 字段。针对特定 UUID 的字节串相等比较仍不可表达（无字符串字面量 tokenizer、无 bytes 值类型，fact 10），故 v0.1 中 `uuid` box 声明为不透明 `usertype` 或经 `unsupported` 上报；此即收口 ADR-0096 D1 §5 留给 ADR-0097 的待决问题。

### D4：size 分支——分支内 `computed` + `@lazy`；`size == 0` 的增量是新增 `available_bytes()` 内建

- **`size == 1` / `else`**：唯一可行写法是分支内形式（fact 12），ADR-0096 D1 已采用该形态：`if (size == 1) { bits<64> largesize; computed<u64> large_payload = largesize - 16; @lazy(large_payload) bytes large_data; } else { computed<u64> payload = size - 8; @lazy(payload) bytes data; }`。本 ADR 复确认其为唯一可行写法。
- **`size == 0`**：今天确实不可表达（fact 13）：`compressed_payload` 被限制为最后一个顶层项（`dsl.cpp:1589`），进不了 `if` 分支；且无任何剩余长度查询。

最小增量的两个候选：

- **A — scanner 计算剩余长度并下传**：scanner 把 `size == 0` 解析为实际剩余长度，经 header/context 值流交给规则。否决理由：把 scanner 内部实现耦合进 DSL 值流，只在 box 边界这一种情形有意义，且为单一角例引入新的 context 管线。
- **B — 新增 `available_bytes()` 内建**：字节粒度的「当前源区域自当前读取位置起的剩余字节数」查询，可用于任意 `computed` 表达式（镜像 `more_rbsp_data()`，但按字节、返回值）。

**决策：B。** 格式中立、最小、可复用，并使 `size == 0` 可表达为：

```
if (size == 0) {
    computed<u64> payload_bytes = available_bytes();
    @lazy(payload_bytes) bytes payload;
}
```

scanner 仍按 D1 执行 `size == 0` 分帧（跨度至 EOF、终结 box）；内建只让规则为其 lazy 载荷区域定尺寸。P5d 在表达式语言与 VM 中实现该内建。

### D5：`@target_format`——并入中央注解注册表的设计

注解注册表是 `dsl.cpp:1836-1865` 的 `knownAnnotations`；`@target_format` 当前以空宿主集预留（`dsl.cpp:1865` 的 `{u"target_format", 0U}`，`dsl.cpp:1864` 注释「Reserved for Task P5h」）。P5d-2 的设计：

- 注册为 `{u"target_format", DslAnnotationTarget::LazyRegion}`，使通用白名单在 lazy 字节区域上放行它。LazyRegion 特判消息（`Lazy byte regions accept only @description and @spec`，`dsl.cpp:1895`）必须同步扩展以接纳 `@target_format`，保证宿主白名单与报错文案一致（P5b 闸门不得自我拦截：P5h 规则要用 `@target_format`）。
- **位置只能是后置位**（`bytes name` 之后）：在 `@lazy(...)` 与 `bytes` 之间插注解会被拒绝（`Expected bytes after @lazy(...)`，`dsl.cpp:1112`；探针见上）。用法形态：

```
@lazy(payload_bytes) bytes payload @target_format("video/mp4");
```

- 语义：该注解给 lazy 区域打上目标格式标识，供 `AnalysisSession`/UI 做跨层导航（P5h：`avcC`/`esds` 区域 → `RulePackageCatalog::resolve` 目标入口 → 坐标映射视图）。
- 本 ADR 只写设计；注册表与白名单改动按计划的能力切片顺序落在 P5d-2（`dsl.cpp:1864` 注释写 P5h，是因为 P5h 是*消费*任务）。

### D6：样本表窗口化——强制要求，附分页/坐标契约

窗口化是硬要求而非优化：`maximumExpandedFieldsPerStructure = 99'999`（按 struct 计，`dsl_ir.cpp:13`）把超出上限的表展开变成编译错误（fact 14：`repeat(count, 99999)` → `Bounded repeat expansion exceeds the structure materialization limit`；同一 struct 两张 60000 条表失败；拆成两个 struct 各自 99998 成功）。默认写法是 lazy 区域形态（fact 15）：

```
computed<u64> table_bytes = entry_count * 4;
@lazy(table_bytes) bytes entry_table;
```

窗口读取契约（设计）：

- **物化**：表元数据（`entry_count`、表配置）完整物化；条目数组是 lazy 字节区域，绝不展开为字段。
- **窗口解码**：由 runner/会话按用户请求按需解码有界窗口（页）；不做全表急切解码。
- **source coordinate 回映**：每个窗口条目的逻辑范围 = 容器锚点 + `index * entry_size`；绝对源 bit 可恢复，UI 以源偏移展示。坐标一律由 lazy 区域锚点推导，禁止手算。
- **UI 翻页契约**：固定页大小；前/后翻页；会话变化时窗口失效；单页物化绝不超出节点预算（`defaultMaximumMaterializedNodes() = 100'000`，`dsl_vm.h:36-38`）。

### D7：分片 MP4（`moof`）——box 头之后用规则内 `unsupported`

两种机制：

- **A — 规则内 `unsupported`**：规则声明 `moof`/`mfhd`/`traf` box 结构体；读毕 `size`/`type`（可选 `mfhd`）后在 `type` 处发 `unsupported("fragmented MP4 (moof/traf) is outside the v0.1 subset") at type;`。后果：已解码前缀（`ftyp`、`moov`…）保持物化；`moof` box 节点留在树上，状态为 `MaterializationState::Unsupported`、`DiagnosticCode::UnsupportedSyntax`（Warning）、锚点在 `type`；顶层 box 扫描**继续**处理后续 box（例如 `moof` 之后的 `mdat`）。探针（见下）证明该形态可编译（`Rule OK`）。
- **B — 探测器分级拒绝**：探测器在构树之前就把整份源判为不支持；无任何 box 节点；单条源级错误；且迫使探测器做 FourCC 嗅探（`moof`）——格式语义进入核心检测。

**决策：A。** 保留已解码前缀与逐 box 粒度，继续后续顶层扫描，保持探测器格式无关，并符合 ADR-0040 的非致命哲学（码流合法，只是超出声明子集）。

`unsupported` 的三条禁令不阻碍容器场景（探针对 `dsl_ir.cpp:2741/2748/2771` 验证）：(i) 不得置于 repeat 内——`moof` 判定在 box 结构体顶层；(ii) reason 不得为空——始终提供真实原因字符串；(iii) 锚点不得为 computed 字段——锚点是 `bits<32> type` 字段，绝不用 `computed`。

---

## 被否决方案及理由

1. **结构体类型字段**（`BoxHeader header;`）做嵌套——否决：parser 今天拒绝结构体类型字段，且 `@container`（D2）无需新字段类型即可实现嵌套。
2. **嵌套 `sequence` 声明**——否决：需要新语法、IR 降级与 VM 排序语义，且与「sequence 仅限顶层」契约冲突；`@container` 严格更小。
3. **为嵌套引入 `payload<box>` view kind**——v0.1 否决：分派视图按值选一个结构体，而容器要枚举多个子 box，解决不了嵌套。将来可作为顶层派发的人机工学优化回归。view kind 闭集暂时保持 `{rbsp}`（`dsl.cpp:3669`、`dsl_ir.cpp:3717`）。
4. **scanner 计算剩余长度下传以表达 `size == 0`**——否决，改用 `available_bytes()` 内建（D4）：内建格式中立、可复用，且避免为角例引入 context 管线。
5. **探测器级整份分片 MP4 拒绝**——否决（D7）：丢失已解码前缀与后续 box 扫描，并把 FourCC 逻辑塞进核心检测。
6. **为 FourCC 新增 `bytes` 值类型**——已被 fact 10 否决：`bits<32>` + `@equals(0x...)` 足够，不引入新值类型。

---

## 影响

### 正面

- P5d 之后 ISOBMFF box 树完全可表达：顶层枚举（D1）、容器下钻（D2）、type 派发（D3）、含 `size == 0` EOF 情形的 size 算术（D4）、样本表硬性窗口化契约（D6）。
- `@target_format`（D5）提供 P5h 消费的跨格式导航元数据，且注解闸门不自我拦截。
- 分片 MP4 在规则层非致命处理（D7）：保留已解码前缀、逐 box 诊断、继续扫描——与 ADR-0040 一致。
- 核心保持格式中立：scanner 只知分帧（D1）；runner 重入是通用的（D2）；`src/` 核心路径零 FourCC 字面量。

### 负面

- P5d 切片需落地四个新能力增量：(1) `DslScannerKind::Mp4Box` + scanner，(2) `@container` 注解 + runner 重入，(3) `available_bytes()` 内建，(4) `@target_format` 注册 + LazyRegion 白名单放开。每片按 T4a/T4b 先例独立闭环可测。
- `payload<rbsp>` 专属限制保留；顶层派发在新增派发 view kind 之前只能用 `type` 上的 `if`/`else`。
- `size == 0` box 天然终结（延伸至 EOF），同一区域内其后不可能再有其它顶层 box；规则须反映这一点。

---

## 验证矩阵与证据

本轮全部探针使用工作树工具二进制 `build/dev/tools/svtool/svtool`（`svtool 0.1.0 (DSL 0.1)`）执行；scratch 源位于仓库之外的会话 scratch 目录。完整命令与输出在 P5c 任务报告中逐条复现。

| # | 探针 | 命令 | 结果 |
| :--- | :--- | :--- | :--- |
| P1 | `scan(mp4_box)` 顶层枚举 | `svtool rule check p5c_p1_scan_mp4_box.svfmt` | `error: Only h264_start_code and adts_frame are supported`（`dsl.cpp:3568`） |
| P2a | 结构体类型字段 | `svtool rule check p5c_p2a_struct_typed_field.svfmt` | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` |
| P2b | struct 内 `sequence` | `svtool rule check p5c_p2b_nested_sequence.svfmt` | `error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type` |
| P2c | `payload<mp4>` view kind | `svtool rule check p5c_p2c_payload_mp4_kind.svfmt` | `error: The only accepted payload view kind is rbsp` |
| P2d | `@container` 注解 | `svtool rule check p5c_p2d_container_annotation.svfmt` | `error: Unknown annotation '@container'` |
| P5a | `@target_format` 后置于 lazy 区域 | `svtool rule check p5c_p5a_target_format_lazy.svfmt` | `error: Lazy byte regions accept only @description and @spec`（`dsl.cpp:1895`） |
| P5b | `@target_format` 置于 `@lazy` 与 `bytes` 之间 | `svtool rule check p5c_p5b_target_format_preposition.svfmt` | `error: Expected bytes after @lazy(...)`（`dsl.cpp:1112`） |
| P7a | `unsupported("")` 空 reason | `svtool rule check p5c_p7a_unsupported_empty_reason.svfmt` | `error: Unsupported statements require a non-empty reason`（`dsl_ir.cpp:2748`） |
| P7b | `unsupported` 锚定 computed 字段 | `svtool rule check p5c_p7b_unsupported_computed_anchor.svfmt` | `error: Unsupported anchors require a source-backed scalar field`（`dsl_ir.cpp:2771`） |
| P7c | `repeat` 内 `unsupported` | `svtool rule check p5c_p7c_unsupported_repeat_local.svfmt` | `error: Unsupported statements cannot be repeat-local items`（`dsl_ir.cpp:2741`） |
| P7d | box 头字段后 `unsupported`（moof 形态） | `svtool rule check p5c_p7d_unsupported_positive.svfmt` | `Rule OK` |
| T1 | fixture 生成 | `ffmpeg ... -f lavfi -i testsrc=... -c:v libx264 -preset ultrafast -c:a aac -y /tmp/p5c_fixture.mp4` | 生成 6,513 字节普通 MP4 |
| T2 | box 级 ground truth | `ffprobe -v trace -i /tmp/p5c_fixture.mp4` | box 层级：`ftyp`/`free`/`mdat`/`moov`；`moov`→`mvhd`/`trak`×2；`trak`→`tkhd`/`edts`/`mdia`；`mdia`→`mdhd`/`hdlr`/`minf`；`minf`→`vmhd`/`smhd`/`dinf`/`stbl`；`stbl`→`stsd`（`avc1`+`avcC`/`pasp`/`btrt`，`mp4a`+`esds`/`btrt`）/`stts`/`stss`/`stsc`/`stsz`/`stco` |
| T3 | 分片 fixture | `ffmpeg ... -movflags frag_keyframe+empty_moov -y /tmp/p5c_frag.mp4` | 4,505 字节分片 MP4：`moov`（+`mvex`/`trex`）后为 `moof`→`mfhd`/`traf`→`tfhd`/`trun`，再 `mdat` |
| T4 | sample 级 ground truth | `ffprobe -v error -select_streams v:0 -show_packets -show_entries packet=pos,pts_time,duration_time,flags -of csv=p=0 /tmp/p5c_fixture.mp4` | 视频帧 1：pos 306、pts 0.0、关键帧；帧 2：pos 3282、pts 0.1 |

工具链结论：`ffprobe` 8.1 与 `ffmpeg` 可用；`MP4Box` 未安装。`ffmpeg` + `ffprobe -v trace`（box 类型/size/偏移/父子关系）+ `ffprobe -show_packets`（sample 偏移/时间戳/关键帧）即可完成生成与 ground truth 验证，全程不手算任何 bit 偏移，满足 P5d+ fixture 的「禁止手算 bit 位置」惯例。

---

## 参考

- ADR-0096：MP4/ISOBMFF 容器架构、Box 遍历、跨层导航与样本索引边界（其 D1 分支内 `computed` + `@lazy` 形态由本 ADR 复确认；其 `size == 0` 与 `uuid` 两个开放缺口在本 ADR 收口）。
- ADR-0098：未识别注解编译闸门与显式不支持语法（注解注册表与 `unsupported` 语句契约）。
- ADR-0040：非致命语法警告与范围注解（D7 应用的致命/非致命二分法）。
- ISO/IEC 14496-12:2015（ISOBMFF box 结构与 `size`/`largesize`/`size == 0` 语义）。
- 任务 P5c 定义与计划 fact 10–16，`Sub-Agent分步开发指导计划.md`。

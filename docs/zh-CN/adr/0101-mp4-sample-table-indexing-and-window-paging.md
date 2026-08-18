# ADR-0101：MP4 样本表索引与窗口分页规则 v0.1.2

- **状态**：提议（Proposed）
- **日期**：2026-08-18
- **作者**：StreamView 贡献者

---

## 背景

任务 P5f 完成了 `moov` 容器层级与时间头规则（`org.streamview.mp4` v0.1.1），确立了 `moov`、`trak`、`edts`、`mdia`、`minf` 与 `stbl` 的递归容器导航。

任务 P5g 在 `org.streamview.mp4` v0.1.2 中实现样本表索引与窗口分页规则：
1. 样本时间 Box（`stts` / `0x73747473`）：`TimeToSampleEntry` 及 `@window(TimeToSampleEntry, entry_count)`。
2. 样本分块 Box（`stsc` / `0x73747363`）：`SampleToChunkEntry` 及 `@window(SampleToChunkEntry, entry_count)`。
3. 样本尺寸 Box（`stsz` / `0x7374737A`）：`sample_size == 0` 时采用 `SampleSizeEntry` 与 `@window(SampleSizeEntry, sample_count)`，`sample_size > 0` 时采用标量统一样本尺寸。
4. 32 位分块偏移 Box（`stco` / `0x7374636F`）：`ChunkOffsetEntry` 及 `@window(ChunkOffsetEntry, entry_count)`。
5. 64 位分块偏移 Box（`co64` / `0x636F3634`）：`ChunkLargeOffsetEntry` 及 `@window(ChunkLargeOffsetEntry, entry_count)`。

上述 5 种样本表 Box 必须：
- 解析 FullBox 头部（`version`、`flags`）并对 `version != 0` 报告 `unsupported(...)` 语法诊断；
- 支持 32 位标准 Box 尺寸、64 位 `largesize`（`size == 1`）以及 `size == 0` 延伸至 EOF；
- 复用现有 DSL `@window` 注解、`WindowDecoder`、`RunnerExecutionBudget`、分页缓存与源坐标映射，核心 C++ 引擎不包含格式专属 FourCC 逻辑；
- 保持严格切片边界：编解码配置 Box（`stsd`、`avc1`、`avcC`、`mp4a`、`esds`）延后至任务 P5h，跨层导航延后至任务 P5i。

---

## 决策

### 1. 规则包清单（`src/rules/official/org.streamview.mp4/rule.toml`）

规则包版本从 `0.1.1` 升级至 `0.1.2`：

```toml
manifest-version = 1

[package]
id = "org.streamview.mp4"
version = "0.1.2"
authors = ["StreamView contributors"]
license = "MIT"
dependencies = []

[compatibility]
language = "0.1"
engine = ">=0.1.0 <0.2.0"

[[entrypoints]]
id = "main"
format = "video.mp4"
source = "src/mp4_isobmff.svfmt"
profiles = ["isobmff"]
depth = "boxes"
detector = "mp4-box"
```

### 2. DSL 样本表条目结构与 Box 模式

```svfmt
@spec("ISO/IEC 14496-12:2015", "8.6.1.2")
@description("Time to Sample entry.")
struct TimeToSampleEntry {
    bits<32> sample_count
        @description("Number of consecutive samples with this delta.");
    bits<32> sample_delta
        @description("Delta of these samples in timescale units.");
}

@spec("ISO/IEC 14496-12:2015", "8.6.1.2")
@description("Time to Sample box payload.")
struct TimeToSampleBox {
    bits<8> version
        @description("Version (0).");
    bits<24> flags
        @description("Flags.");
    bits<32> entry_count
        @description("Number of time-to-sample entries.");
    if (version == 0) {
        computed<u64> table_bytes = entry_count * 8;
        @lazy(table_bytes) bytes entries @window(TimeToSampleEntry, entry_count);
    } else {
        unsupported("Unsupported stts FullBox version") at version;
    }
}

@spec("ISO/IEC 14496-12:2015", "8.7.4")
@description("Sample to Chunk entry.")
struct SampleToChunkEntry {
    bits<32> first_chunk
        @description("First chunk index using this description.");
    bits<32> samples_per_chunk
        @description("Number of samples in each chunk.");
    bits<32> sample_description_index
        @description("Sample description index for these samples.");
}

@spec("ISO/IEC 14496-12:2015", "8.7.4")
@description("Sample to Chunk box payload.")
struct SampleToChunkBox {
    bits<8> version
        @description("Version (0).");
    bits<24> flags
        @description("Flags.");
    bits<32> entry_count
        @description("Number of sample-to-chunk entries.");
    if (version == 0) {
        computed<u64> table_bytes = entry_count * 12;
        @lazy(table_bytes) bytes entries @window(SampleToChunkEntry, entry_count);
    } else {
        unsupported("Unsupported stsc FullBox version") at version;
    }
}

@spec("ISO/IEC 14496-12:2015", "8.7.3.2")
@description("Sample Size entry.")
struct SampleSizeEntry {
    bits<32> entry_size
        @description("Size of individual sample in bytes.");
}

@spec("ISO/IEC 14496-12:2015", "8.7.3.2")
@description("Sample Size box payload.")
struct SampleSizeBox {
    bits<8> version
        @description("Version (0).");
    bits<24> flags
        @description("Flags.");
    bits<32> sample_size
        @description("Default sample size (if 0, individual sizes follow in table).");
    bits<32> sample_count
        @description("Number of samples.");
    if (version == 0) {
        if (sample_size == 0) {
            computed<u64> table_bytes = sample_count * 4;
            @lazy(table_bytes) bytes entries @window(SampleSizeEntry, sample_count);
        }
    } else {
        unsupported("Unsupported stsz FullBox version") at version;
    }
}

@spec("ISO/IEC 14496-12:2015", "8.7.5")
@description("32-bit Chunk Offset entry.")
struct ChunkOffsetEntry {
    bits<32> chunk_offset
        @description("32-bit byte offset of chunk from beginning of file.");
}

@spec("ISO/IEC 14496-12:2015", "8.7.5")
@description("32-bit Chunk Offset box payload.")
struct ChunkOffsetBox {
    bits<8> version
        @description("Version (0).");
    bits<24> flags
        @description("Flags.");
    bits<32> entry_count
        @description("Number of chunk offset entries.");
    if (version == 0) {
        computed<u64> table_bytes = entry_count * 4;
        @lazy(table_bytes) bytes entries @window(ChunkOffsetEntry, entry_count);
    } else {
        unsupported("Unsupported stco FullBox version") at version;
    }
}

@spec("ISO/IEC 14496-12:2015", "8.7.5")
@description("64-bit Chunk Offset entry.")
struct ChunkLargeOffsetEntry {
    bits<64> chunk_offset
        @description("64-bit byte offset of chunk from beginning of file.");
}

@spec("ISO/IEC 14496-12:2015", "8.7.5")
@description("64-bit Chunk Offset box payload.")
struct ChunkLargeOffsetBox {
    bits<8> version
        @description("Version (0).");
    bits<24> flags
        @description("Flags.");
    bits<32> entry_count
        @description("Number of 64-bit chunk offset entries.");
    if (version == 0) {
        computed<u64> table_bytes = entry_count * 8;
        @lazy(table_bytes) bytes entries @window(ChunkLargeOffsetEntry, entry_count);
    } else {
        unsupported("Unsupported co64 FullBox version") at version;
    }
}
```

### 3. 窗口分页与执行语义

1. **计数变量无条件约束**：
   - 按 ADR-0097 与 `dsl_ir.cpp` 规范，传递给 `@window` 的计数变量（`entry_count`、`sample_count`）必须在宿主载荷结构体（`TimeToSampleBox`、`SampleToChunkBox`、`SampleSizeBox`、`ChunkOffsetBox`、`ChunkLargeOffsetBox`）顶层无条件声明。
2. **WindowDecoder 集成**：
   - `WindowDecoder` 按需批量解码固定大小条目，避免一次性物化数兆字节的大型样本表。
   - 重复请求同一页时复用已缓存树节点，不发生冗余分配。
   - `RunnerExecutionBudget` 预算约束防止超大 `entry_count` 码流耗尽内存。
3. **坐标映射**：
   - 窗口区域内的样本条目通过 `SourceMapping` 和 `SourceBitAddress` 精确回映到源文件物理与逻辑位区间。

---

## 影响

- MP4 样本表索引（`stts`、`stsc`、`stsz`、`stco`、`co64`）完全由 DSL 规则表达并激活。
- 窗口分页机制在容器分析期间约束内存开销。
- P5h 编解码配置与 P5i 跨层导航边界清晰隔离。

# ADR-0100：MP4 moov 容器层级与时间头规则 v0.1.1

- **状态**：提议（Proposed）
- **日期**：2026-08-18
- **作者**：StreamView 贡献者

---

## 背景

任务 P5e 建立了官方 `org.streamview.mp4` 规则包（v0.1.0），包含顶层 Box 扫描（`ftyp`、`mdat` lazy、`moof` unsupported 警告与未知 box 兜底 opaque 载荷）。

任务 P5f 将 `org.streamview.mp4` 扩展至版本 `0.1.1`，明确规范：
1. 对所有容器 Box 使用 `@container(Box)` 进行递归容器重入：
   - 电影 Box（`moov` / `0x6D6F6F76`）
   - 轨道 Box（`trak` / `0x7472616B`）
   - 编辑 Box（`edts` / `0x65647473`）
   - 媒体 Box（`mdia` / `0x6D646961`）
   - 媒体信息 Box（`minf` / `0x6D696E66`）
   - 样本表 Box（`stbl` / `0x7374626C`）
2. 头信息与元数据叶子 Box：
   - 电影头 Box（`mvhd` / `0x6D766864`）
   - 轨道头 Box（`tkhd` / `0x746B6864`）
   - 编辑列表 Box（`elst` / `0x656C7374`）
   - 媒体头 Box（`mdhd` / `0x6D646864`）
   - 处理器引用 Box（`hdlr` / `0x68646C72`）
3. 支持 FullBox `version == 0`（32 位时间戳与时长字段）和 `version == 1`（64 位时间戳与时长字段），按 ISO/IEC 14496-12:2015 消费精确码流比特跨度。
   - 对 `size == 1` 的 Box，64 位 `largesize` 必须紧跟在 `size` 与 `type` 之后、FullBox 载荷之前消费；容器和下述专用头/元数据 Box 均遵守该顺序。
   - 除 `0` 与 `1` 之外的 FullBox 版本在 version 字段处报告 `UnsupportedSyntax`，不得静默回退为 version-0 布局。
4. 明确的切片边界：
   - 样本表分页（`stts`、`stsc`、`stsz`、`stco`、`co64`）延后至任务 P5g；
   - 编解码配置 Box（`stsd`、`avc1`、`mp4a`、`avcC`、`esds`）延后至任务 P5h；
   - 在 P5f 中，`stbl` 内部未定义专用头的载荷 Box 均作为带 `@lazy` 载荷的标准 Box 解析。

---

## 决策

### 1. 规则包清单（`src/rules/official/org.streamview.mp4/rule.toml`）

规则包版本从 `0.1.0` 升级至 `0.1.1`：

```toml
manifest-version = 1

[package]
id = "org.streamview.mp4"
version = "0.1.1"
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

### 2. DSL Box 语法与容器递归

所有容器 Box（`moov`、`trak`、`edts`、`mdia`、`minf`、`stbl`）均通过 `@lazy(...) bytes ... @container(Box);` 重入 `Mp4IsobmffAnalyzer` 容器扫描。

```svfmt
@spec("ISO/IEC 14496-12:2015", "4.2")
@description("ISO Base Media File Format box.")
struct Box {
    bits<32> size
        @description("Box size in bytes. 1 indicates 64-bit largesize; 0 indicates extends to EOF.");
    bits<32> type
        @description("Box FourCC type identifier.");
    if (type == 0x66747970) {
        ...
    } else {
        if (type == 0x6D6F6F76) {
            if (size == 1) {
                bits<64> moov_largesize;
                computed<u64> moov_large_payload_bytes = moov_largesize - 16;
                @lazy(moov_large_payload_bytes) bytes moov_large_payload @container(Box);
            } else {
                if (size == 0) {
                    computed<u64> moov_eof_payload_bytes = available_bytes();
                    @lazy(moov_eof_payload_bytes) bytes moov_eof_payload @container(Box);
                } else {
                    computed<u64> moov_payload_bytes = size - 8;
                    @lazy(moov_payload_bytes) bytes moov_payload @container(Box);
                }
            }
        } else {
            if (type == 0x6D766864) {
                if (size == 1) {
                    bits<64> mvhd_largesize;
                }
                bits<8> mvhd_version
                    @description("Version of movie header box (0 or 1).");
                bits<24> mvhd_flags
                    @description("Flags (reserved).");
                if (mvhd_version == 1) {
                    bits<64> mvhd_v1_creation_time
                        @description("Creation time (seconds since 1904-01-01).");
                    bits<64> mvhd_v1_modification_time
                        @description("Modification time (seconds since 1904-01-01).");
                    bits<32> mvhd_v1_timescale
                        @description("Time scale units per second.");
                    bits<64> mvhd_v1_duration
                        @description("Duration of movie in timescale units.");
                } else {
                    if (mvhd_version == 0) {
                        bits<32> mvhd_v0_creation_time
                            @description("Creation time (seconds since 1904-01-01).");
                        bits<32> mvhd_v0_modification_time
                            @description("Modification time (seconds since 1904-01-01).");
                        bits<32> mvhd_v0_timescale
                            @description("Time scale units per second.");
                        bits<32> mvhd_v0_duration
                            @description("Duration of movie in timescale units.");
                    } else {
                        unsupported("Unsupported mvhd FullBox version") at mvhd_version;
                    }
                }
                bits<32> mvhd_rate
                    @description("Playback rate (fixed-point 16.16, 0x00010000 is 1.0).");
                bits<16> mvhd_volume
                    @description("Audio volume (fixed-point 8.8, 0x0100 is full).");
                bits<16> mvhd_reserved;
                bits<64> mvhd_reserved_2;
                computed<u64> mvhd_matrix_count = 9;
                repeat (mvhd_matrix_count, 9) {
                    bits<32> mvhd_matrix;
                }
                computed<u64> mvhd_pre_defined_count = 6;
                repeat (mvhd_pre_defined_count, 6) {
                    bits<32> mvhd_pre_defined;
                }
                bits<32> mvhd_next_track_id
                    @description("Next track ID to assign.");
            } else {
                ...
            }
        }
    }
}
```

### 3. 头字段合同

1. **电影头（`mvhd`，条款 8.2.2）**：
   - `mvhd_version`：`bits<8>`
   - `mvhd_flags`：`bits<24>`
   - `creation_time`、`modification_time`、`duration`：`version == 1` 为 64 位，`version == 0` 为 32 位
   - `timescale`：`bits<32>`
   - `rate`：`bits<32>`（16.16 定点数）
   - `volume`：`bits<16>`（8.8 定点数）
   - `reserved`：`bits<16>` + `bits<64>`
   - `matrix`：9 项 `bits<32>`
   - `pre_defined`：6 项 `bits<32>`
   - `next_track_id`：`bits<32>`

2. **轨道头（`tkhd`，条款 8.3.2）**：
   - `tkhd_version`：`bits<8>`
   - `tkhd_flags`：`bits<24>`
   - `creation_time`、`modification_time`、`duration`：`version == 1` 为 64 位，`version == 0` 为 32 位
   - `track_id`：`bits<32>`
   - `reserved`：`bits<32>`
   - `reserved_2`：`bits<64>`
   - `layer`：`bits<16>`
   - `alternate_group`：`bits<16>`
   - `volume`：`bits<16>`
   - `reserved_3`：`bits<16>`
   - `matrix`：9 项 `bits<32>`
   - `width`：`bits<32>`（16.16 定点数）
   - `height`：`bits<32>`（16.16 定点数）

3. **媒体头（`mdhd`，条款 8.4.2）**：
   - `mdhd_version`：`bits<8>`
   - `mdhd_flags`：`bits<24>`
   - `creation_time`、`modification_time`、`duration`：`version == 1` 为 64 位，`version == 0` 为 32 位
   - `timescale`：`bits<32>`
   - `pad`：`bits<1>`
   - `language`：`bits<15>`（打包 3x5-bit ISO-639-2/T 语言码）
   - `pre_defined`：`bits<16>`

4. **处理器引用（`hdlr`，条款 8.4.3）**：
   - `hdlr_version`：`bits<8>`
   - `hdlr_flags`：`bits<24>`
   - `pre_defined`：`bits<32>`
   - `handler_type`：`bits<32>`
   - `reserved_0`、`reserved_1`、`reserved_2`：3 项 `bits<32>`（静态字节对齐以支持后续 lazy 区域）
   - `name`：`@lazy` bytes

5. **编辑列表（`elst`，条款 8.6.6）**：
   - `elst_version`：`bits<8>`
   - `elst_flags`：`bits<24>`
   - `elst_entry_count`：`bits<32>`
   - `repeat (elst_entry_count, 64)`（P5f 最多接受 64 个条目；更大计数返回 `InvalidSyntax`，以保持 DSL 有界投影合同）：
     - `segment_duration`、`media_time`：`version == 1` 为 64 位，`version == 0` 为 32 位
     - `media_rate_integer`：`bits<16>`
     - `media_rate_fraction`：`bits<16>`

---

## 影响

- MP4 分析现已支持完整的 5 层容器嵌套（`moov -> trak -> mdia -> minf -> stbl`），完全由 DSL 规则驱动，核心引擎无硬编码逻辑。
- 时间头在 version 0 与 version 1 下消费精确码流比特。
- large-size `hdlr` 在 FullBox 载荷前消费 `largesize`，并保持 handler name 的正确跨度。
- 未支持的 FullBox 版本保留已解码前缀并产生 warning，不继续按 v0 解码后缀。
- P5g 样本表索引与 P5h 编解码配置隔离至后续任务交付。

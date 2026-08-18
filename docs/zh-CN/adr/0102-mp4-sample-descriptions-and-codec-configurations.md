# ADR-0102：MP4 样本描述与编解码配置规则 v0.1.3

- **状态**：提议（Proposed）
- **日期**：2026-08-18
- **作者**：StreamView 贡献者

---

## 背景

任务 P5g 完成了样本表索引与窗口分页规则（`org.streamview.mp4` v0.1.2），确立了 `stts`、`stsc`、`stsz`、`stco` 与 `co64` 的分页解码机制。

任务 P5h 在 `org.streamview.mp4` v0.1.3 中实现样本描述（`stsd`）、样本条目（`avc1`、`mp4a`）以及编解码配置（`avcC`、`esds`）规则：
1. 样本描述 Box（`stsd` / `0x73747364`）：FullBox 容器，容纳视觉与音频样本条目。
2. 视觉样本条目（`avc1` / `0x61766331`）：ISO/IEC 14496-12 78 字节视觉样本条目头部，作为子编解码配置 Box 的容器。
3. 音频样本条目（`mp4a` / `0x6D703461`）：ISO/IEC 14496-14 28 字节音频样本条目头部，作为子描述符 Box 的容器。
4. AVC 配置 Box（`avcC` / `0x61766343`）：ISO/IEC 14496-15 `AVCDecoderConfigurationRecord`，将重复的序列参数集（SPS）、图像参数集（PPS）和 High-profile SPS 扩展集公开为带有 `@target_format("video.h264.nal")` 注解的 `@lazy(...) bytes` 区域。
5. 基本流描述符 Box（`esds` / `0x65736473`）：ISO/IEC 14496-14 FullBox 描述符的 AAC 支持子集，校验 `ES_Descriptor`（标签 0x03）、`DecoderConfigDescriptor`（标签 0x04）与 `DecoderSpecificInfo`（标签 0x05），按三个 flag 消费 ES 可选字段，并支持 1–4 字节可扩展长度编码（最高有效位 0x80）。`AudioSpecificConfig` 载荷公开为带有 `@target_format("audio.aac.asc")` 注解的 `@lazy(...) bytes` 区域。

所有结构必须：
- 校验 FullBox 头部版本（`stsd` 版本 0，`esds` 版本 0）及 `avcC` `configurationVersion == 1`，对未处理版本报告 `unsupported(...)` 语法诊断；
- 支持 32 位标准 Box 尺寸、64 位 `largesize`（`size == 1`）以及 `size == 0` 延伸至 EOF；
- 校验 descriptor 标签、AVC/AAC 保留位，并对第五个长度 continuation 字节报告 `unsupported(...)`；descriptor 长度会被解码，但仍在外层 Box view 内消费，不创建新的 descriptor bounded view；
- 在整数字节 lazy 区域、可变 bounded/sentinel repeat 与 switch 分支合流中保持 DSL 字节对齐追踪，不采用 C++ 解码器专用旁路；
- 保持严格切片边界：任务 P5h 仅生成 AST 节点与 `@target_format` 元数据，不触发子格式分析器调用或实现跨层会话导航（延后至任务 P5i）。

---

## 决策

### 1. 规则包清单（`src/rules/official/org.streamview.mp4/rule.toml`）

规则包版本从 `0.1.2` 升级至 `0.1.3`：

```toml
manifest-version = 1

[package]
id = "org.streamview.mp4"
version = "0.1.3"
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

### 2. DSL 样本描述与编解码配置模式

```svfmt
@spec("ISO/IEC 14496-12:2015", "8.5.2")
@description("Sample Description Box.")
struct SampleDescriptionBox {
    bits<8> version
        @description("Version (0).");
    bits<24> flags
        @description("Flags.");
    bits<32> entry_count
        @description("Number of sample entries.");
    if (version == 0) {
        computed<u64> stsd_entries_bytes = available_bytes();
        @lazy(stsd_entries_bytes) bytes entries
            @description("Sample description entries.")
            @container(Box);
    } else {
        unsupported("Unsupported stsd FullBox version") at version;
    }
}

@spec("ISO/IEC 14496-12:2015", "8.5.2.2")
@description("Visual Sample Entry (AVC1).")
struct VisualSampleEntry {
    bits<48> reserved_entry
        @description("Reserved 6 bytes.");
    bits<16> data_reference_index
        @description("Data reference index.");
    bits<16> pre_defined
        @description("Pre-defined (0).");
    bits<16> reserved_1
        @description("Reserved (0).");
    bits<32> pre_defined_1_0
        @description("Pre-defined 1/3 (0).");
    bits<32> pre_defined_1_1
        @description("Pre-defined 2/3 (0).");
    bits<32> pre_defined_1_2
        @description("Pre-defined 3/3 (0).");
    bits<16> width
        @description("Visual display width in pixels.");
    bits<16> height
        @description("Visual display height in pixels.");
    bits<32> horizresolution
        @description("Horizontal resolution (0x00480000 = 72 dpi).");
    bits<32> vertresolution
        @description("Vertical resolution (0x00480000 = 72 dpi).");
    bits<32> reserved_2
        @description("Reserved (0).");
    bits<16> frame_count
        @description("Frames per sample (usually 1).");
    bits<64> compressorname_0
        @description("Compressor name 1/4.");
    bits<64> compressorname_1
        @description("Compressor name 2/4.");
    bits<64> compressorname_2
        @description("Compressor name 3/4.");
    bits<64> compressorname_3
        @description("Compressor name 4/4.");
    bits<16> depth
        @description("Color depth (0x0018 = 24-bit).");
    bits<16> pre_defined_2
        @description("Pre-defined (-1).");
    computed<u64> avc1_children_bytes = available_bytes();
    @lazy(avc1_children_bytes) bytes child_boxes
        @description("Visual sample entry child boxes.")
        @container(Box);
}

@spec("ISO/IEC 14496-14:2020", "5.6.1")
@description("Audio Sample Entry (MP4A).")
struct AudioSampleEntry {
    bits<48> reserved_entry
        @description("Reserved 6 bytes.");
    bits<16> data_reference_index
        @description("Data reference index.");
    bits<64> reserved_1
        @description("Reserved 8 bytes (0).");
    bits<16> channelcount
        @description("Channel count.");
    bits<16> samplesize
        @description("Sample size in bits (16).");
    bits<16> pre_defined
        @description("Pre-defined (0).");
    bits<16> reserved_2
        @description("Reserved (0).");
    bits<32> samplerate
        @description("Sample rate (16.16 fixed-point).");
    computed<u64> mp4a_children_bytes = available_bytes();
    @lazy(mp4a_children_bytes) bytes child_boxes
        @description("Audio sample entry child boxes.")
        @container(Box);
}

@spec("ISO/IEC 14496-15:2019", "5.2.4.1")
@description("AVC Decoder Configuration Box (avcC).")
struct AvcConfigurationBox {
    bits<8> configurationVersion
        @description("Configuration version (1).");
    if (configurationVersion == 1) {
        bits<8> avcProfileIndication
            @description("AVC profile indication.");
        bits<8> profile_compatibility
            @description("Profile compatibility flags.");
        bits<8> avcLevelIndication
            @description("AVC level indication.");
        bits<6> reserved_6bits @equals(63)
            @description("Reserved 6 bits (111111b).");
        bits<2> lengthSizeMinusOne
            @description("NAL unit length field size minus one.");
        bits<3> reserved_3bits @equals(7)
            @description("Reserved 3 bits (111b).");
        bits<5> numOfSequenceParameterSets
            @description("Number of sequence parameter sets.");
        repeat (numOfSequenceParameterSets, 31) {
            bits<16> sequenceParameterSetLength
                @description("SPS NAL unit length in bytes.");
            @lazy(sequenceParameterSetLength) bytes sequenceParameterSetNALUnit
                @description("SPS NAL unit bytes.")
                @target_format("video.h264.nal");
        }
        bits<8> numOfPictureParameterSets
            @description("Number of picture parameter sets.");
        repeat (numOfPictureParameterSets, 64) {
            bits<16> pictureParameterSetLength
                @description("PPS NAL unit length in bytes.");
            @lazy(pictureParameterSetLength) bytes pictureParameterSetNALUnit
                @description("PPS NAL unit bytes.")
                @target_format("video.h264.nal");
        }
        computed<bool> has_profile_extensions =
            avcProfileIndication == 100 || avcProfileIndication == 110 ||
            avcProfileIndication == 122 || avcProfileIndication == 144;
        if (has_profile_extensions) {
            bits<6> reserved_chroma_format @equals(63);
            bits<2> chroma_format;
            bits<5> reserved_bit_depth_luma @equals(31);
            bits<3> bit_depth_luma_minus8;
            bits<5> reserved_bit_depth_chroma @equals(31);
            bits<3> bit_depth_chroma_minus8;
            bits<8> numOfSequenceParameterSetExt;
            repeat (numOfSequenceParameterSetExt, 255) {
                bits<16> sequenceParameterSetExtLength;
                @lazy(sequenceParameterSetExtLength) bytes sequenceParameterSetExtNALUnit
                    @target_format("video.h264.nal");
            }
        }
    } else {
        unsupported("Unsupported avcC configurationVersion") at configurationVersion;
    }
}

@spec("ISO/IEC 14496-14:2020", "5.6.1")
@description("Elementary Stream Descriptor Box (esds).")
struct ElementaryStreamDescriptorBox {
    bits<8> version
        @description("Version (0).");
    bits<24> flags
        @description("Flags.");
    if (version == 0) {
        bits<8> es_tag @equals(3)
            @description("ES_DescrTag (0x03).");
        bits<1> es_len_more0;
        bits<7> es_len_val0;
        if (es_len_more0 == 1) {
            bits<1> es_len_more1;
            bits<7> es_len_val1;
            if (es_len_more1 == 1) {
                bits<1> es_len_more2;
                bits<7> es_len_val2;
                if (es_len_more2 == 1) {
                    bits<1> es_len_more3;
                    bits<7> es_len_val3;
                    if (es_len_more3 == 1) {
                        unsupported("ES_Descriptor length exceeds four bytes") at es_len_more3;
                    }
                }
            }
        }
        bits<16> es_id
            @description("ES ID.");
        bits<1> streamDependenceFlag
            @description("Stream dependence flag.");
        bits<1> urlFlag
            @description("URL flag.");
        bits<1> ocrStreamFlag
            @description("OCR stream flag.");
        bits<5> streamPriority
            @description("Stream priority.");
        if (streamDependenceFlag == 1) {
            bits<16> dependsOn_ES_ID;
        }
        if (urlFlag == 1) {
            bits<8> URLlength;
            @lazy(URLlength) bytes URLstring;
        }
        if (ocrStreamFlag == 1) {
            bits<16> OCR_ES_Id;
        }
        bits<8> dc_tag @equals(4)
            @description("DecoderConfigDescrTag (0x04).");
        bits<1> dc_len_more0;
        bits<7> dc_len_val0;
        if (dc_len_more0 == 1) {
            bits<1> dc_len_more1;
            bits<7> dc_len_val1;
            if (dc_len_more1 == 1) {
                bits<1> dc_len_more2;
                bits<7> dc_len_val2;
                if (dc_len_more2 == 1) {
                    bits<1> dc_len_more3;
                    bits<7> dc_len_val3;
                    if (dc_len_more3 == 1) {
                        unsupported("DecoderConfigDescriptor length exceeds four bytes") at dc_len_more3;
                    }
                }
            }
        }
        bits<8> objectTypeIndication @equals(64)
            @description("Object type indication (0x40 for Audio ISO/IEC 14496-3 AAC).");
        bits<6> streamType @equals(5)
            @description("Stream type (0x05 for AudioStream).");
        bits<1> upStream
            @description("Upstream flag.");
        bits<1> reserved_1bit @equals(1)
            @description("Reserved bit (1).");
        bits<24> bufferSizeDB
            @description("Buffer size in bytes.");
        bits<32> maxBitrate
            @description("Maximum bitrate.");
        bits<32> avgBitrate
            @description("Average bitrate.");
        bits<8> dsi_tag @equals(5)
            @description("DecSpecificInfoTag (0x05).");
        bits<1> dsi_len_more0;
        bits<7> dsi_len_val0;
        if (dsi_len_more0 == 1) {
            bits<1> dsi_len_more1;
            bits<7> dsi_len_val1;
            if (dsi_len_more1 == 1) {
                bits<1> dsi_len_more2;
                bits<7> dsi_len_val2;
                if (dsi_len_more2 == 1) {
                    bits<1> dsi_len_more3;
                    bits<7> dsi_len_val3;
                    if (dsi_len_more3 == 1) {
                        unsupported("DecoderSpecificInfo length exceeds four bytes") at dsi_len_more3;
                    } else {
                        computed<u64> asc_len4 = dsi_len_val0 * 2097152 + dsi_len_val1 * 16384 + dsi_len_val2 * 128 + dsi_len_val3;
                        @lazy(asc_len4) bytes asc_bytes4
                            @description("AudioSpecificConfig payload.")
                            @target_format("audio.aac.asc");
                    }
                } else {
                    computed<u64> asc_len3 = dsi_len_val0 * 16384 + dsi_len_val1 * 128 + dsi_len_val2;
                    @lazy(asc_len3) bytes asc_bytes3
                        @description("AudioSpecificConfig payload.")
                        @target_format("audio.aac.asc");
                }
            } else {
                computed<u64> asc_len2 = dsi_len_val0 * 128 + dsi_len_val1;
                @lazy(asc_len2) bytes asc_bytes2
                    @description("AudioSpecificConfig payload.")
                    @target_format("audio.aac.asc");
            }
        } else {
            computed<u64> asc_len1 = dsi_len_val0;
            @lazy(asc_len1) bytes asc_bytes1
                @description("AudioSpecificConfig payload.")
                @target_format("audio.aac.asc");
        }
    } else {
        unsupported("Unsupported esds FullBox version") at version;
    }
}
```

### 3. 顶层 Box 分发

顶层 `Box` 结构分发：
- `0x73747364`（`stsd`）：`@container(SampleDescriptionBox)`
- `0x61766331`（`avc1`）：`@container(VisualSampleEntry)`
- `0x6D703461`（`mp4a`）：`@container(AudioSampleEntry)`
- `0x61766343`（`avcC`）：`@container(AvcConfigurationBox)`
- `0x65736473`（`esds`）：`@container(ElementaryStreamDescriptorBox)`

### 4. Lazy 区域与 Repeat 循环间的 DSL 字节对齐追踪

由于 `@lazy(byte_count) bytes` 消耗整数个字节（`byte_count * 8` 位），在字节边界开始的 lazy 区域在结束时同样处于字节边界。DSL 编译器与 IR 下降逻辑在全部构成元素均处于字节边界时，在 lazy 区域与 repeat 循环迭代之间保持 `byteAligned = true`。

---

## 后果

- 通过声明式 DSL 规则解码本 ADR 声明的 P5h 样本描述与 AAC/AVC 编解码配置子集；descriptor 长度仍受外层 Box view 约束。
- 编解码配置载荷附加 `@target_format("video.h264.nal")` 与 `@target_format("audio.aac.asc")` 元数据供下游会话导航使用。
- 正确建模多字节描述符长度扩展、ES 可选字段、High-profile SPS 扩展与重复 SPS/PPS 结构；第五个 continuation 字节报告为不支持。
- 跨层分析器解析与 UI 导航保持清晰解耦，留待任务 P5i 实施。

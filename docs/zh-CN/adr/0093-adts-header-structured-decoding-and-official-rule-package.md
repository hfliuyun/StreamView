# ADR-0093: ADTS 头部结构化解码与 AAC 官方规则包

## 状态

提议中 (Proposed)

## 背景

任务 T15 确立了 ADTS 帧枚举能力（`AacAdtsScanner` / `DslScannerKind::AacAdtsFrame`）、候选格式探测（`detectAacAdtsCandidate`）与应用会话多态解耦（`AnalysisSession`）。

任务 T16 引入官方 AAC 规则包（`org.streamview.aac`，版本从 `0.1.0` 起步），在 DSL 格式语言中定义正式的 ADTS 头部语法（`src/aac_adts.svfmt`），并在 StreamView 中激活端到端 AAC 码流分析。

依据 ISO/IEC 14496-3:2019（第 5 版），音频数据传输流（ADTS）帧由以下结构组成：
1. `adts_fixed_header`（28 bits，条款 1.6.2.1，表 1.6.2.1）：包含同步字、MPEG 音频版本、layer、CRC 保护标志、profile、采样率索引、私有位、声道配置、original/copy 与 home。
2. `adts_variable_header`（28 bits，条款 1.6.2.1，表 1.6.2.1）：包含版权标识位、总帧长度（`aac_frame_length`）、缓冲区饱满度（`adts_buffer_fullness`）与每帧原始数据块数减 1（`number_of_raw_data_blocks_in_frame`）。
3. `adts_error_check`（16 bits，条款 1.6.2.2，表 1.6.2.2）：在 `protection_absent == 0` 时条件存在。
4. `adts_raw_data_block`（条款 1.6.2.3）：音频负载，包含 SCE、CPE、LFE、DSE、PCE、FIL、TERM 等语法元素。

## 决策

我们规范并实现正式的 `org.streamview.aac` 规则包与 ADTS 头部结构化解码：

### 1. 官方规则包清单 (`rule.toml`)

AAC 规则包建立在 `src/rules/official/org.streamview.aac/rule.toml`，初始版本为 `0.1.0`：

```toml
manifest-version = 1

[package]
id = "org.streamview.aac"
version = "0.1.0"
authors = ["StreamView contributors"]
license = "MIT"
dependencies = []

[compatibility]
language = "0.1"
engine = ">=0.1.0 <0.2.0"

[[entrypoints]]
id = "adts"
format = "audio.aac.adts"
source = "src/aac_adts.svfmt"
profiles = ["lc"]
depth = "adts-frame"
detector = "aac-adts"
```

包资产通过 Qt 资源系统（`official_packages.qrc`）内置于二进制文件中，并通过 `AacAdtsAnalyzer::create(source, errorMessage)` 解析加载，与 `h264_annex_b_analyzer.cpp` 中的 `bundledH264AnnexBRule()` 模式完全同构。

### 2. DSL 语法定义 (`src/aac_adts.svfmt`)

正式格式规则声明如下：

```svfmt
enum AacProfile {
    main = 0;
    lc = 1;
    ssr = 2;
    ltp = 3;
}

enum AacChannelConfiguration {
    custom = 0;
    mono = 1;
    stereo = 2;
    three_channel = 3;
    four_channel = 4;
    five_channel = 5;
    five_one = 6;
    seven_one = 7;
}

@spec("ISO/IEC 14496-3:2019", "1.6.2.1")
@description("Audio Data Transport Stream (ADTS) fixed and variable header.")
struct AdtsHeader {
    bits<12> syncword @equals(4095)
        @description("12-bit syncword, always 0xFFF.");
    bits<1> id
        @description("MPEG audio version: 0 for MPEG-4, 1 for MPEG-2.");
    bits<2> layer @equals(0)
        @description("MPEG layer, always 0 for AAC.");
    bits<1> protection_absent
        @description("0 indicates 16-bit CRC is present; 1 indicates CRC absent.");
    bits<2> profile @enum(AacProfile)
        @description("AAC object type profile.");
    bits<4> sampling_frequency_index @range(0, 12)
        @description("Sampling frequency index from 0 (96 kHz) to 12 (7.35 kHz).");
    bits<1> private_bit
        @description("Private bit for application usage.");
    bits<3> channel_configuration @enum(AacChannelConfiguration)
        @description("Channel configuration.");
    bits<1> original_copy
        @description("Original/copy bit.");
    bits<1> home
        @description("Home/reserved bit.");
    bits<1> copyright_identification_bit
        @description("Copyright identification bit.");
    bits<1> copyright_identification_start
        @description("Copyright identification start.");
    bits<13> aac_frame_length
        @description("Total frame length in bytes including headers, CRC, and payload.");
    bits<11> adts_buffer_fullness
        @description("Buffer fullness; 0x7FF indicates variable bit rate (VBR).");
    bits<2> number_of_raw_data_blocks_in_frame @equals(0)
        @description("Number of raw data blocks minus 1; 0 denotes single raw data block.");
    if (protection_absent == 0) {
        bits<16> crc_check
            @spec("ISO/IEC 14496-3:2019", "1.6.2.2")
            @description("16-bit CRC error check word.");
    }
    computed<u64> minimum_frame_length =
        (protection_absent == 1) * 7 +
        (protection_absent == 0) * 9;
    assert(aac_frame_length >= minimum_frame_length) at aac_frame_length;
}

@index(progressive)
sequence<AdtsHeader> frames = scan(adts_frame);
entry frames;
```

### 3. 值域定级与诊断策略

依据 ADR-0040 二分法定级：

1. **影响布局的关键不变量（Layout-Critical Invariants）**：
   - `syncword @equals(4095)`：12 位 `0xFFF`。不满足时终止帧解析并触发重同步机制。
   - `layer @equals(0)`：在 ISO/IEC 14496-3 与 ISO/IEC 13818-7 中，AAC 码流严格要求 `layer == 0`。
   - `number_of_raw_data_blocks_in_frame @equals(0)`：将当前解析范围约束为单原始数据块码流。
2. **值域级非致命字段（Value-Domain Classifications）**：
   - `sampling_frequency_index`：采用 `@range(0, 12)` 约束（ISO/IEC 14496-3 表 1.16）。13、14 与转义值 15（在 ADTS 中依据条款 1.6.2.1 被禁用）产生非致命诊断告警，不中断帧解码。
   - `profile`：声明为 4 值枚举 `enum AacProfile`（`0` Main, `1` LC, `2` SSR, `3` LTP）。注明 profile `3` (LTP) 在 MPEG-2 AAC（`id == 1`）中为保留值。
   - `channel_configuration`：保留完整 8 值枚举 `enum AacChannelConfiguration`（`0` Custom/PCE .. `7` 7.1）。`channel_configuration == 0` 表示原始数据块中包含程序配置元素 PCE（在任务 T18 中支持）。
   - `adts_buffer_fullness`：`0x7FF` 为符合规范的标准特殊值，表示可变码率（VBR）码流。
3. **源码锚定最小帧长断言**：
   - 通过 `computed<u64> minimum_frame_length` 结合 ADR-0090 布尔算术计算头部基准长度；
   - 通过 `assert(aac_frame_length >= minimum_frame_length) at aac_frame_length;` 校验声明长度，在码流损坏且声明长度小于头部字节数时发射源码锚定诊断。

### 4. 显式延期范围

1. `number_of_raw_data_blocks_in_frame > 0`（多原始数据块及块间 16 位 CRC 头 `raw_data_block_position`）显式延期。
2. `raw_data_block` 内部音频语法元素（ASC、SCE、CPE、LFE、PCE 等）的深入结构化解析显式延期至任务 T17 与 T18。

### 5. 架构探测与证据

上述 `.svfmt` 规则定义已通过 StreamView 规则编译器 `svtool rule check` 实测验证：

```bash
$ ./build/dev/tools/svtool/svtool rule check scratch/probe_t16_adts.svfmt
Rule OK: /Users/yun/.gemini/antigravity-cli/brain/12458dc0-7cd4-40c3-b0af-86d27dcb7b62/scratch/probe_t16_adts.svfmt
```

## 参考资料

- ISO/IEC 14496-3:2019, Edition 5, Subclauses 1.6.2.1, 1.6.2.2, 1.6.2.3, Tables 1.11, 1.16, 1.17, 1.6.2.1, 1.6.2.2
- ADR-0010: C-Style Declarative Format Description Language
- ADR-0016: TOML Manifest And ZIP Rule Packages
- ADR-0040: Report Unsigned Exp-Golomb Range Violations Without Stopping Decoding
- ADR-0054: Source-Anchored Assertion Statements
- ADR-0090: Boolean Operands In Arithmetic Expressions
- ADR-0092: AAC ADTS Frame Enumeration Mechanism and Official Rule Package

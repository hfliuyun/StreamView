# ADR-0094：AudioSpecificConfig 与 Program Config Element 结构化解码

## 状态
提议中 (Proposed)

## 背景
在 MPEG-4 音频规范（ISO/IEC 14496-3:2019，第 5 版）中，高级音频编码（AAC）码流的解码器配置由 `AudioSpecificConfig`（ASC，条款 1.6.2.1）形式化标准化。虽然 ADTS 码流在每个帧头中携带基本流属性（Profile、采样率索引、声道配置；参见 ADR-0092、ADR-0093），但现代音频分发载体（包括 MPEG-4 Part 14 容器的 MP4 `esds` box、条款 1.6.6 及流媒体协议）均依赖带外 `AudioSpecificConfig` 在解析压缩 raw data block 之前初始化音频解码器。

为完成 StreamView 实施计划的阶段 4，官方 `org.streamview.aac` 规则包必须提供以下内容的结构化解码：
1. **AudioSpecificConfig (ASC)**：包括基础及扩展音频对象类型（`audioObjectType`）、标准及显式采样率（`samplingFrequencyIndex` / `samplingFrequency`）、声道配置以及通用音频配置（`GASpecificConfig`）；
2. **Program Config Element (PCE)**：当 `channelConfiguration == 0` 时用于自定义多声道布局（条款 1.6.2.1，表 1.18）；
3. **容器上下文导出**：支持下游容器格式（如阶段 5 的 ISO BMFF / MP4）通过 `streamview::core::ContextDirectory` 使用 `ContextDefinitionKind::AacAudioSpecificConfig` 解析并绑定当前有效的 `AudioSpecificConfig` 定义。

### 探测与语言能力分析
在制定语法规范前，已在 scratch 副本中通过 `svtool rule check` 进行实测探测，验证 DSL 语言边界约束：

1. **子结构实例化与作用域隔离**：
   在结构体内尝试声明嵌套结构体实例（如 `GASpecificConfig ga_specific_config;` 或 `ProgramConfigElement pce;`）会在解析器类型检查处被拦截：
   ```
   error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type
   ```
   此外，从独立子结构引用外部结构体字段会被编译器支配分析拦截：
   ```
   error: Computed dependency is not guaranteed on the current branch
   ```
   因此，遵循 H.264 VUI/HRD（`SequenceParameterSetRbsp`）、切片头（`IdrSliceLayerWithoutPartitioningRbsp`）与 SEI 消息（`SeiRbsp`）的既有成熟范式，`GASpecificConfig` 与条件性 `ProgramConfigElement` 语法必须在 `AudioSpecificConfig` 内扁平展开并内联化。

2. **PCE 变长数组与有界循环**：
   `ProgramConfigElement` 包含 6 个变长声道元素数组（`front`、`side`、`back`、`lfe`、`assoc_data`、`valid_cc`）与一段注释数据字节序列。探测确认 DSL 有界循环语法：
   ```svfmt
   repeat (num_front_channel_elements, 15) {
       bits<1> front_element_is_cpe;
       bits<4> front_element_tag_select;
   }
   ```
   能够精确解析、降级至 IR，并在 ISO/IEC 14496-3 标准最大上限（15、15、15、3、7、15 与 255）下通过静态校验。

3. **PCE 字节对齐表达式**：
   尝试使用 `align(8);` 语句无法通过语法解析：
   ```
   error: Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type
   ```
   尝试 `repeat (7) while (!byte_aligned())` 会被 while 条件解析闸门拦截：
   ```
   error: Expected 'more_rbsp_data' in while condition
   ```
   尝试三元表达式 `(cond ? val1 : val2)` 会被词法解析拦截：
   ```
   error: Invalid character in DSL source
   ```
   而利用 ADR-0090 布尔算术与整数模运算：
   ```svfmt
   computed<u64> pce_total_bits = 31
       + mono_mixdown_present * 4
       + stereo_mixdown_present * 4
       + matrix_mixdown_idx_present * 3
       + num_front_channel_elements * 5
       + num_side_channel_elements * 5
       + num_back_channel_elements * 5
       + num_lfe_channel_elements * 4
       + num_assoc_data_elements * 4
       + num_valid_cc_elements * 5;

   computed<u64> pce_rem = pce_total_bits % 8;
   computed<u64> pce_alignment_bits = (8 - pce_rem) % 8;

   repeat (pce_alignment_bits, 7) {
       bits<1> byte_alignment_zero_bit @equals(0);
   }
   ```
   可精确求出 bit 余数并校验 0 至 7 个填充零位，`svtool rule check` 输出 `Rule OK`。

## 决策

### 1. 官方包入口点与清单
我们在 `src/rules/official/org.streamview.aac/rule.toml` 中新增专用入口点 `asc`：
```toml
manifest-version = 1

[package]
id = "org.streamview.aac"
version = "0.1.1"
authors = ["StreamView Contributors"]
license = "Apache-2.0"
dependencies = []

[compatibility]
language = "0.1"
engine = ">=0.1.0 <0.2.0"

[[entrypoints]]
id = "adts"
format = "audio.aac.adts"
source = "src/aac_adts.svfmt"
profiles = ["aac-adts"]
depth = "adts-frame"

[[entrypoints]]
id = "asc"
format = "audio.aac.asc"
source = "src/aac_asc.svfmt"
profiles = ["aac-asc"]
depth = "structural"
```

### 2. AudioSpecificConfig DSL 语法规范
我们在 `src/rules/official/org.streamview.aac/src/aac_asc.svfmt` 中定义标准 ISO/IEC 14496-3:2019 条款 1.6.2.1 语法：

```svfmt
struct AudioSpecificConfig {
    bits<5> audio_object_type
        @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
        @description("Audio Object Type identifier.");
    if (audio_object_type == 31) {
        bits<6> audio_object_type_ext
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Audio Object Type extension for escape values.");
    }

    bits<4> sampling_frequency_index
        @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
        @range(0, 12)
        @description("Sampling frequency index.");
    if (sampling_frequency_index == 15) {
        bits<24> sampling_frequency
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Explicit sampling frequency in Hz.");
    }

    bits<4> channel_configuration
        @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
        @range(0, 7)
        @description("Channel configuration index (0 indicates custom PCE).");

    // GASpecificConfig 语法
    bits<1> frame_length_flag
        @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
        @description("Frame length flag (0: 1024 lines IMDCT, 1: 960 lines IMDCT).");
    bits<1> depends_on_core_coder
        @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
        @description("Core coder dependency flag.");
    if (depends_on_core_coder == 1) {
        bits<14> core_coder_delay
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Core coder delay value in samples.");
    }
    bits<1> extension_flag
        @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
        @description("Extension flag for audio object type specific extensions.");

    if (channel_configuration == 0) {
        // Program Config Element (PCE)
        bits<4> element_instance_tag
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("PCE instance identifier tag.");
        bits<2> object_type
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("PCE audio object type.");
        bits<4> pce_sampling_frequency_index
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @range(0, 12)
            @description("PCE sampling frequency index.");
        bits<4> num_front_channel_elements
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Number of front channel elements.");
        bits<4> num_side_channel_elements
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Number of side channel elements.");
        bits<4> num_back_channel_elements
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Number of back channel elements.");
        bits<2> num_lfe_channel_elements
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Number of low-frequency enhancement channel elements.");
        bits<3> num_assoc_data_elements
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Number of associated data elements.");
        bits<4> num_valid_cc_elements
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Number of valid coupling channel elements.");

        bits<1> mono_mixdown_present
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Mono mixdown present flag.");
        if (mono_mixdown_present == 1) {
            bits<4> mono_mixdown_element_number
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Mono mixdown element number.");
        }

        bits<1> stereo_mixdown_present
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Stereo mixdown present flag.");
        if (stereo_mixdown_present == 1) {
            bits<4> stereo_mixdown_element_number
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Stereo mixdown element number.");
        }

        bits<1> matrix_mixdown_idx_present
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Matrix mixdown index present flag.");
        if (matrix_mixdown_idx_present == 1) {
            bits<2> matrix_mixdown_idx
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Matrix mixdown index.");
            bits<1> pseudo_surround_enable
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Pseudo surround enable flag.");
        }

        repeat (num_front_channel_elements, 15) {
            bits<1> front_element_is_cpe
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Front channel element is channel pair element flag.");
            bits<4> front_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Front channel element instance tag selector.");
        }

        repeat (num_side_channel_elements, 15) {
            bits<1> side_element_is_cpe
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Side channel element is channel pair element flag.");
            bits<4> side_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Side channel element instance tag selector.");
        }

        repeat (num_back_channel_elements, 15) {
            bits<1> back_element_is_cpe
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Back channel element is channel pair element flag.");
            bits<4> back_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Back channel element instance tag selector.");
        }

        repeat (num_lfe_channel_elements, 3) {
            bits<4> lfe_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("LFE channel element instance tag selector.");
        }

        repeat (num_assoc_data_elements, 7) {
            bits<4> assoc_data_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Associated data element instance tag selector.");
        }

        repeat (num_valid_cc_elements, 15) {
            bits<1> cc_element_is_ind_sw
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Coupling channel element independently switched flag.");
            bits<4> valid_cc_element_tag_select
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Valid coupling channel element instance tag selector.");
        }

        computed<u64> pce_total_bits = 31
            + mono_mixdown_present * 4
            + stereo_mixdown_present * 4
            + matrix_mixdown_idx_present * 3
            + num_front_channel_elements * 5
            + num_side_channel_elements * 5
            + num_back_channel_elements * 5
            + num_lfe_channel_elements * 4
            + num_assoc_data_elements * 4
            + num_valid_cc_elements * 5;

        computed<u64> pce_rem = pce_total_bits % 8;
        computed<u64> pce_alignment_bits = (8 - pce_rem) % 8;

        repeat (pce_alignment_bits, 7) {
            bits<1> byte_alignment_zero_bit
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @equals(0)
                @description("Byte alignment stuffing zero bit.");
        }

        bits<8> comment_field_bytes
            @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
            @description("Length in bytes of comment field data.");
        repeat (comment_field_bytes, 255) {
            bits<8> comment_field_data
                @spec("ISO/IEC 14496-3:2019", "1.6.2.1")
                @description("Comment field data byte.");
        }
    }
}

entry AudioSpecificConfig;
```

### 3. 诊断与截断语义
- **截断（`DiagnosticCode::TruncatedSource`）**：若 ASC 码流在解码中途提前结束（如在 `AudioSpecificConfig` 头部、`GASpecificConfig` 或 PCE 声道列表内部），`DslExecutor::decodeStruct` 返回 `DslExecutionStatus::TruncatedSource`。部分物化结构节点附带 `DiagnosticCode::TruncatedSource` 与 `DiagnosticSeverity::Error`（`"Unable to read complete syntax field"`）。
- **对齐错误（`DiagnosticCode::InvalidSyntax`）**：若 PCE 填充对齐位非零（`@equals(0)` 校验失败），`DslExecutor` 在违规 bit 坐标处产生 `DiagnosticCode::InvalidSyntax` 与 `DiagnosticSeverity::Error` 诊断。
- **值域告警（`DiagnosticCode::InvalidSyntax`）**：若 `sampling_frequency_index` 超过 12 或 `channel_configuration` 超过 7，节点正常物化并按 ADR-0040 附带 `@range` 非致命警告诊断。

## 影响

### 正面影响
- 完整解码标准 AAC-LC `AudioSpecificConfig` 码流及自定义多声道 `ProgramConfigElement` 布局。
- 保持单遍线性流式解析性能，无子结构调用的额外开销。
- 为阶段 5 MP4 `esds` box 容器解析与上下文绑定建立形式化基础。
- 包版本升级为 `0.1.1`。

### 负面影响
- SBR/PS 向后兼容同步扩展（`syncExtensionType == 0x2b7`）及非通用音频对象类型暂不在此切片中展开，留待专用扩展处理。

## 参考引用
- ISO/IEC 14496-3:2019, Information technology — Coding of audio-visual objects — Part 3: Audio（条款 1.6.2.1, 1.6.6）。
- [ADR-0040：非致命语法警告与值域注解](0040-non-fatal-syntax-warnings-and-range-annotations.md)
- [ADR-0090：DSL 布尔算术与逻辑表达式](0090-boolean-arithmetic-and-logical-expressions-in-dsl.md)
- [ADR-0092：AAC ADTS 分帧枚举与规则包架构](0092-aac-adts-frame-enumeration-and-rule-package.md)
- [ADR-0093：ADTS 头结构化解码与官方规则包](0093-adts-header-structured-decoding-and-official-rule-package.md)

# ADR-0091：H.264 图像定时 SEI 消息解码（H.264 Picture Timing SEI Message Decoding）

## 状态

已接受 (Accepted)

## 背景

ITU-T H.264 条款 D.1.2 / D.2.2 定义了图像定时 SEI 消息（Picture Timing SEI Message，`payload_type == 1`），用于传输 HRD CPB 移除延迟、DPB 输出延迟、图像结构指示（`pic_struct`）以及用于图像呈现和时钟同步的精确定时时间戳。

结构化解码图像定时 SEI 消息涉及以下关键依赖：
1. **活跃 SPS 上下文参数**：延迟字段位宽（`cpb_removal_delay_length_minus1 + 1`、`dpb_output_delay_length_minus1 + 1`）、`time_offset_length` 以及 `pic_struct_present_flag` 均源自活跃序列参数集（SPS）中的 HRD 与 VUI 参数。由于 SEI 语法本身不携带显式 SPS ID，这些参数必须通过环境上下文导入（ADR-0086）从当前流位置之前最近的有效 SPS 中动态获取。
2. **HRD 参数层级结构**：依据 ITU-T H.264 条款 E.1.2 / E.2.1 / D.2.2，当 NAL HRD 参数存在时，以 NAL HRD 长度参数优先；当 NAL HRD 缺席且 VCL HRD 存在时，以 VCL HRD 长度参数为准；当二者均缺席时，采用条款 E.2.1 规定的默认值（延迟长度缺省 23、`time_offset_length` 缺省 24、`pic_struct_present_flag` 缺省 0）。
3. **表格 D-1 图像结构映射**：`pic_struct` 依据 ITU-T H.264 表格 D-1 映射至时钟时间戳数量 `NumClockTS`（$0..2 \to 1$、$3, 4, 7 \to 2$、$5, 6, 8 \to 3$、$9..15 \to 0$）。
4. **条件性载荷对齐**：消息载荷总位宽可能正好落在字节边界（例如无 `pic_struct` 且延迟为缺省 48 位时），也可能为非对齐位，必须通过 `byte_aligned()` 谓词（ADR-0089）实现条件性 trailing bits 对齐。

## 决策

我们在官方规则包版本 `0.1.38` 中完整实现 ITU-T H.264 D.1.2 / D.2.2 图像定时 SEI 消息解码：

### 1. SPS 上下文导出扩充
在 `SequenceParameterSetRbsp`（`src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt`）中，依据条款 E.1.2 / E.2.1 增加 4 项基于嵌套 `optional_value` 的导出字段：
```svfmt
computed<u64> effective_cpb_removal_delay_length_minus1 =
    optional_value(nal_hrd_cpb_removal_delay_length_minus1,
                   optional_value(vcl_hrd_cpb_removal_delay_length_minus1, 23)) @context_export;
computed<u64> effective_dpb_output_delay_length_minus1 =
    optional_value(nal_hrd_dpb_output_delay_length_minus1,
                   optional_value(vcl_hrd_dpb_output_delay_length_minus1, 23)) @context_export;
computed<u64> effective_time_offset_length =
    optional_value(nal_hrd_time_offset_length,
                   optional_value(vcl_hrd_time_offset_length, 24)) @context_export;
computed<u64> effective_pic_struct_present_flag =
    optional_value(pic_struct_present_flag, 0) @context_export;
```

### 2. `SeiRbsp` 环境导入与既有有键导入共存
`SeiRbsp` 声明 `@context_import("h264-sps")`（环境上下文导入），与既有的 `@context_import("h264-sps", seq_parameter_set_id)`（有键导入）共存。依据 ADR-0086 §4：
- 2 参表达式 `context_value(h264_sps, field)` 自动绑定至环境最新 SPS；
- 3 参表达式 `context_value(seq_parameter_set_id, h264_sps, field)` 绑定至有键导入。

### 3. CPB 与 DPB 延迟解码
当 `effective_nal_hrd_parameters_present_flag == 1 || effective_vcl_hrd_parameters_present_flag == 1` 时，`CpbDpbDelaysPresentFlag` 为真，解码以下字段：
- `bits<(context_value(h264_sps, effective_cpb_removal_delay_length_minus1) + 1)> cpb_removal_delay`
- `bits<(context_value(h264_sps, effective_dpb_output_delay_length_minus1) + 1)> dpb_output_delay`

### 4. 表格 D-1 映射纯函数（基于 ADR-0090 布尔算术）
定义顶层纯函数表达指示加权映射：
```svfmt
pure u64 num_clock_ts_for_pic_struct(u64 pic_struct) {
    return (pic_struct <= 2) * 1 +
           (pic_struct == 3 || pic_struct == 4 || pic_struct == 7) * 2 +
           (pic_struct == 5 || pic_struct == 6 || pic_struct == 8) * 3;
}
```

### 5. 图像结构与时钟时间戳分支逐字段解码
当 `context_value(h264_sps, effective_pic_struct_present_flag) == 1` 时：
- `bits<4> pic_struct @range(0, 8)`
- `computed<u64> num_clock_ts = num_clock_ts_for_pic_struct(pic_struct);`
- `repeat (num_clock_ts, 3)` 展开最多 3 个时钟时间戳结构：
  - `bits<1> clock_timestamp_flag`
  - 当 `clock_timestamp_flag == 1` 时：
    - `bits<2> ct_type @range(0, 2)`
    - `bits<1> nuit_field_based_flag`
    - `bits<5> counting_type @range(0, 6)`
    - `bits<1> full_timestamp_flag`
    - `bits<1> discontinuity_flag`
    - `bits<1> cnt_dropped_flag`
    - `bits<8> n_frames`
    - 完整时间戳模式（`full_timestamp_flag == 1`）：
      - `bits<6> full_seconds_value @range(0, 59)`
      - `bits<6> full_minutes_value @range(0, 59)`
      - `bits<5> full_hours_value @range(0, 23)`
    - 部分时间戳模式（`full_timestamp_flag == 0`）：
      - `bits<1> seconds_flag`
      - 若 `seconds_flag == 1`：
        - `bits<6> partial_seconds_value @range(0, 59)`
        - `bits<1> minutes_flag`
        - 若 `minutes_flag == 1`：
          - `bits<6> partial_minutes_value @range(0, 59)`
          - `bits<1> hours_flag`
          - 若 `hours_flag == 1`：
            - `bits<5> partial_hours_value @range(0, 23)`
    - `time_offset` 门控解码：
      - `computed<bool> has_time_offset = context_value(h264_sps, effective_time_offset_length) > 0;`
      - `if (has_time_offset) { bits<(context_value(h264_sps, effective_time_offset_length))> time_offset; }`

### 6. 字段命名与二补数解释
- **字段命名消歧**：遵循 DSL 结构体内字段名全局唯一性要求，完整与部分时间戳分支的对应字段分别附加 `full_` 和 `partial_` 前缀（对应标准 D.1.2 的 `seconds_value`、`minutes_value`、`hours_value`）。
- **`time_offset` 符号性解释**：按探测结论，`time_offset` 统一按原始无符号位宽 `bits<time_offset_length>` 提取，在规范文档中明确其数值解释为二补数有符号整数。

| 内置规则字段名 | ITU-T H.264 条款 D.1.2 元素名 | 出现条件 / 分支 | 取值范围 / 类型 |
|---|---|---|---|
| `cpb_removal_delay` | `cpb_removal_delay` | `CpbDpbDelaysPresentFlag == 1` | `bits<effective_cpb_removal_delay_length_minus1 + 1>` |
| `dpb_output_delay` | `dpb_output_delay` | `CpbDpbDelaysPresentFlag == 1` | `bits<effective_dpb_output_delay_length_minus1 + 1>` |
| `pic_struct` | `pic_struct` | `effective_pic_struct_present_flag == 1` | `bits<4> @range(0, 8)` |
| `num_clock_ts` | `NumClockTS` | 依表格 D-1 计算 | `computed<u64>` (0..3) |
| `clock_timestamp_flag` | `clock_timestamp_flag[i]` | Repeat 循环 `i < num_clock_ts` | `bits<1>` |
| `ct_type` | `ct_type` | `clock_timestamp_flag == 1` | `bits<2> @range(0, 2)` |
| `nuit_field_based_flag` | `nuit_field_based_flag` | `clock_timestamp_flag == 1` | `bits<1>` |
| `counting_type` | `counting_type` | `clock_timestamp_flag == 1` | `bits<5> @range(0, 6)` |
| `full_timestamp_flag` | `full_timestamp_flag` | `clock_timestamp_flag == 1` | `bits<1>` |
| `discontinuity_flag` | `discontinuity_flag` | `clock_timestamp_flag == 1` | `bits<1>` |
| `cnt_dropped_flag` | `cnt_dropped_flag` | `clock_timestamp_flag == 1` | `bits<1>` |
| `n_frames` | `n_frames` | `clock_timestamp_flag == 1` | `bits<8>` |
| `full_seconds_value` | `seconds_value` | `full_timestamp_flag == 1` | `bits<6> @range(0, 59)` |
| `full_minutes_value` | `minutes_value` | `full_timestamp_flag == 1` | `bits<6> @range(0, 59)` |
| `full_hours_value` | `hours_value` | `full_timestamp_flag == 1` | `bits<5> @range(0, 23)` |
| `seconds_flag` | `seconds_flag` | `full_timestamp_flag == 0` | `bits<1>` |
| `partial_seconds_value` | `seconds_value` | `full_timestamp_flag == 0 && seconds_flag == 1` | `bits<6> @range(0, 59)` |
| `minutes_flag` | `minutes_flag` | `full_timestamp_flag == 0 && seconds_flag == 1` | `bits<1>` |
| `partial_minutes_value` | `minutes_value` | `full_timestamp_flag == 0 && seconds_flag == 1 && minutes_flag == 1` | `bits<6> @range(0, 59)` |
| `hours_flag` | `hours_flag` | `full_timestamp_flag == 0 && seconds_flag == 1 && minutes_flag == 1` | `bits<1>` |
| `partial_hours_value` | `hours_value` | `full_timestamp_flag == 0 && seconds_flag == 1 && minutes_flag == 1 && hours_flag == 1` | `bits<5> @range(0, 23)` |
| `time_offset` | `time_offset` | `clock_timestamp_flag == 1 && effective_time_offset_length > 0` | `bits<effective_time_offset_length>`（二补数解释） |

### 7. 末尾条件性载荷对齐
依据 ADR-0089，在 `case 1` 载荷末尾使用条件性对齐：
```svfmt
if (!byte_aligned()) {
    rbsp_trailing_bits;
}
```

## 影响

### 正向收益
- 完整实现 ITU-T H.264 D.1.2 图像定时 SEI 消息结构化解码。
- 在正式生产规则中充分运用环境 SPS 上下文解析机制。
- 完整支持对齐与非对齐两种位宽组合，彻底杜绝码流错位。

### 包版本
- 升级 `rule.toml` 包版本至 `0.1.38`。

### 验证与 Fixture 测试矩阵
1. **多 SPS 环境绑定**：验证 SEI 能够跨多个历史 SPS 定义准确绑定当前流位置之前最近的活跃 SPS；
2. **无前置 SPS 隔离**：验证缺失 SPS 时产生单消息级 `WaitingDependency` / `Invalid` 诊断，后续 SEI 与 AUD NAL 保持正常解析；
3. **无 HRD 仅 `pic_struct` 路径**：覆盖无 CPB/DPB 延迟仅有图像结构的码流；
4. **对齐与非对齐载荷**：覆盖 48 位整字节对齐与非对齐两种载荷形态下的节点与坐标边界；
5. **合法 `pic_struct = 8`**：覆盖 3 组完整时钟时间戳展开与字段解码；
6. **越界 `pic_struct = 9`**：产生 `@range` Warning 诊断，`NumClockTS = 0` 跳过时间戳并保持流对齐正确；
7. **零长度 `time_offset_length = 0`**：验证当配置为 0 时跳过 `time_offset` 字段；
8. **截断载荷容错**：验证截断载荷安全回滚并保留已物化的前缀字段。

## 参考

- [ADR-0023：纯函数内联与语义检查](../adr/0023-pure-function-inlining.md)
- [ADR-0086：用于环境 SEI 与分层参数集的上下文管理](../adr/0086-context-management-for-ambient-sei-and-layered-parameter-sets.md)
- [ADR-0089：`byte_aligned()` 码流位置谓词表达式](../adr/0089-byte-aligned-predicate-expression.md)
- [ADR-0090：加法与乘法算术表达式接受布尔操作数](../adr/0090-boolean-operands-in-arithmetic-expressions.md)
- ITU-T 建议书 H.264 (08/2021) 条款 D.1.2 / D.2.2 与表格 D-1

# 解码缓冲周期 SEI 消息

状态：已接受
日期：2026-08-15

## 背景

ITU-T H.264 第 7.3.2.3 条定义了补充增强信息（SEI）RBSP 容器结构，第 7.3.2.3.1 条定义了载荷语法派发规则。第 D.1.1 与 D.2.1 条定义了缓冲周期（Buffering Period）SEI 消息（`payloadType == 0`），该消息为假想参考解码器（HRD）操作指定初始 CPB 移除延迟与偏移参数：

```text
buffering_period( payloadSize ) {
    seq_parameter_set_id                                        ue(v)
    if( NalHrdBpPresentFlag ) {
        for( SchedSelIdx = 0; SchedSelIdx <= cpb_cnt_minus1; SchedSelIdx++ ) {
            initial_cpb_removal_delay[ SchedSelIdx ]            u(v)
            initial_cpb_removal_delay_offset[ SchedSelIdx ]     u(v)
        }
    }
    if( VclHrdBpPresentFlag ) {
        for( SchedSelIdx = 0; SchedSelIdx <= cpb_cnt_minus1; SchedSelIdx++ ) {
            initial_cpb_removal_delay[ SchedSelIdx ]            u(v)
            initial_cpb_removal_delay_offset[ SchedSelIdx ]     u(v)
        }
    }
}
```

在第 D.2.1 条中：
- `seq_parameter_set_id`：指定包含序列 HRD 属性的序列参数集标识。合规取值范围为 `0..31`。
- `NalHrdBpPresentFlag`：指示当前激活 SPS 中是否存在 NAL HRD 参数（`nal_hrd_parameters_present_flag == 1`）。
- `VclHrdBpPresentFlag`：指示当前激活 SPS 中是否存在 VCL HRD 参数（`vcl_hrd_parameters_present_flag == 1`）。
- `initial_cpb_removal_delay[SchedSelIdx]`：位宽为 `initial_cpb_removal_delay_length_minus1 + 1` 的位字段，指定初始 CPB 到达延迟。当 SPS HRD 参数中缺省时，`initial_cpb_removal_delay_length_minus1` 默认为 23（即 24 比特）。
- `initial_cpb_removal_delay_offset[SchedSelIdx]`：位宽为 `initial_cpb_removal_delay_length_minus1 + 1` 的位字段，指定初始 CPB 到达延迟偏移。
- CPB 调度迭代次数：`cpb_cnt_minus1 + 1`，依据 ITU-T H.264 第 E.2.2 条有界约束在 1 到 32 之间。

依据 ITU-T H.264 第 7.3.2.3.1 条，若解码后的缓冲周期语法元素未在字节边界对齐，载荷对齐比特（`bit_equal_to_one` 及随后的 `bit_equal_to_zero`）将消息对齐到字节边界。在 StreamView DSL 中，由 `case 0:` 分支末尾的 `rbsp_trailing_bits;` 表达。

### 上下文导出与导入合同

为支持根据引用的 SPS 解码 `buffering_period` SEI 载荷：
1. `SequenceParameterSetRbsp` 导出 HRD 标量属性：
   - `effective_nal_hrd_parameters_present_flag`：`optional_value(nal_hrd_parameters_present_flag, 0)`
   - `effective_nal_hrd_cpb_count`：`optional_value(nal_hrd_cpb_cnt_minus1, 0) + 1`
   - `effective_nal_hrd_initial_cpb_removal_delay_length_minus1`：`optional_value(nal_hrd_initial_cpb_removal_delay_length_minus1, 23)`
   - `effective_vcl_hrd_parameters_present_flag`：`optional_value(vcl_hrd_parameters_present_flag, 0)`
   - `effective_vcl_hrd_cpb_count`：`optional_value(vcl_hrd_cpb_cnt_minus1, 0) + 1`
   - `effective_vcl_hrd_initial_cpb_removal_delay_length_minus1`：`optional_value(vcl_hrd_initial_cpb_removal_delay_length_minus1, 23)`
2. `SeiRbsp` 按 ADR-0084 使用声明在 `case 0:` 内的块内局部上下文导入键 `seq_parameter_set_id` 导入 `h264-sps`。
3. 在 `SeiRbsp` 内部，NAL 与 VCL HRD 字段通过前缀区分（`nal_initial_cpb_removal_delay`、`nal_initial_cpb_removal_delay_offset`、`vcl_initial_cpb_removal_delay`、`vcl_initial_cpb_removal_delay_offset`），确保结构体命名空间内字段标识符唯一。

## 决策

1. **SPS HRD 上下文导出**：
   在 `SequenceParameterSetRbsp` 中使用 `optional_value` 回退语义导出有效 HRD 标量属性：
   ```svfmt
   computed<u64> effective_nal_hrd_parameters_present_flag =
       optional_value(nal_hrd_parameters_present_flag, 0) @context_export;
   computed<u64> effective_nal_hrd_cpb_count =
       optional_value(nal_hrd_cpb_cnt_minus1, 0) + 1 @context_export;
   computed<u64> effective_nal_hrd_initial_cpb_removal_delay_length_minus1 =
       optional_value(nal_hrd_initial_cpb_removal_delay_length_minus1, 23) @context_export;

   computed<u64> effective_vcl_hrd_parameters_present_flag =
       optional_value(vcl_hrd_parameters_present_flag, 0) @context_export;
   computed<u64> effective_vcl_hrd_cpb_count =
       optional_value(vcl_hrd_cpb_cnt_minus1, 0) + 1 @context_export;
   computed<u64> effective_vcl_hrd_initial_cpb_removal_delay_length_minus1 =
       optional_value(vcl_hrd_initial_cpb_removal_delay_length_minus1, 23) @context_export;
   ```

2. **缓冲周期 SEI 结构化解码**：
   在 `SeiRbsp` 中，当 `payload_type == 0` 时解码：
   ```svfmt
   case 0: {
       ue seq_parameter_set_id @range(0, 31)
           @spec("ITU-T H.264", "D.1.1, D.2.1")
           @description("Identifies the sequence parameter set containing the HRD parameters.");
       computed<u64> nal_hrd_bp_present =
           context_value(seq_parameter_set_id, h264_sps, effective_nal_hrd_parameters_present_flag);
       if (nal_hrd_bp_present == 1) {
           computed<u64> nal_cpb_count =
               context_value(seq_parameter_set_id, h264_sps, effective_nal_hrd_cpb_count);
           computed<u64> nal_delay_length =
               context_value(seq_parameter_set_id, h264_sps, effective_nal_hrd_initial_cpb_removal_delay_length_minus1) + 1;
           repeat (nal_cpb_count, 32) {
               bits<nal_delay_length> nal_initial_cpb_removal_delay
                   @spec("ITU-T H.264", "D.1.1, D.2.1")
                   @description("Specifies the default initial arrival delay for the NAL HRD CPB.");
               bits<nal_delay_length> nal_initial_cpb_removal_delay_offset
                   @spec("ITU-T H.264", "D.1.1, D.2.1")
                   @description("Specifies the initial arrival delay offset for the NAL HRD CPB.");
           }
       }
       computed<u64> vcl_hrd_bp_present =
           context_value(seq_parameter_set_id, h264_sps, effective_vcl_hrd_parameters_present_flag);
       if (vcl_hrd_bp_present == 1) {
           computed<u64> vcl_cpb_count =
               context_value(seq_parameter_set_id, h264_sps, effective_vcl_hrd_cpb_count);
           computed<u64> vcl_delay_length =
               context_value(seq_parameter_set_id, h264_sps, effective_vcl_hrd_initial_cpb_removal_delay_length_minus1) + 1;
           repeat (vcl_cpb_count, 32) {
               bits<vcl_delay_length> vcl_initial_cpb_removal_delay
                   @spec("ITU-T H.264", "D.1.1, D.2.1")
                   @description("Specifies the default initial arrival delay for the VCL HRD CPB.");
               bits<vcl_delay_length> vcl_initial_cpb_removal_delay_offset
                   @spec("ITU-T H.264", "D.1.1, D.2.1")
                   @description("Specifies the initial arrival delay offset for the VCL HRD CPB.");
           }
       }
       rbsp_trailing_bits;
   }
   ```

3. **包版本升级**：
   `org.streamview.h264` 规则包版本由 `0.1.34` 升级至 `0.1.35`。

## 影响

- 缓冲周期 SEI 消息将根据所引用的 SPS HRD 配置进行精确结构化解码。
- 支持 NAL HRD、VCL HRD 或两者并存的 SPS 配置分支与展开。
- 当 SPS 中既无 NAL HRD 也无 VCL HRD 时，仅解码 `seq_parameter_set_id` 与载荷尾部对齐比特。
- 当所引用的 SPS 尚未解码或不可用时，导入键以 `DependencyUnavailable` 优雅失败，SEI 容器安全跳过当前消息并继续解析同 RBSP 内的后续 SEI 消息。

## 关联

- ADR-0043: 增加有界 H.264 HRD 参数
- ADR-0080: 使用 while-repeat 在 RBSP 数据上有界迭代
- ADR-0081: 解码恢复点 SEI 消息
- ADR-0084: 块内局部上下文导入键

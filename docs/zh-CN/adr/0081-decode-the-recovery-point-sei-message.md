# 解析恢复点 SEI 消息

状态：已接受
日期：2026-08-15

## 背景

ITU-T H.264 clause 7.3.2.3 规定了补充增强信息（SEI）RBSP 容器，clause 7.3.2.3.1 定义了载荷语法派发。Clause D.1.7 与 clause D.2.7 定义了恢复点（recovery point）SEI 消息（`payloadType == 6`），用于通知解码器可从当前图像开始干净地随机接入或恢复解码：

```text
recovery_point( payloadSize ) {
    recovery_frame_cnt                               ue(v)
    exact_match_flag                                 u(1)
    broken_link_flag                                 u(1)
    changing_slice_group_idc                         u(2)
}
```

Clause 7.3.2.3.1 规定非字节对齐的 SEI 消息载荷以对齐位结尾：

```text
if( !byte_aligned( ) ) {
    bit_equal_to_one /* equal to 1 */
    while( !byte_aligned( ) )
        bit_equal_to_zero /* equal to 0 */
}
```

由于 `recovery_frame_cnt` 采用无符号 Exp-Golomb（`ue(v)`）编码，其比特宽度 $W_{ue} = 2k + 1$ 恒为奇数。加上随后的 4 比特（`exact_match_flag`、`broken_link_flag` 和 `changing_slice_group_idc`），载荷比特总长度为 $2k + 5$，永非 8 的整数倍。因此，恢复点 SEI 消息载荷末尾必然包含对齐位（一个 `bit_equal_to_one` 紧跟 0..6 个 `bit_equal_to_zero`）以达到下一字节边界。

### 探测结果与 DSL 能力分析

在 scratch 副本中对恢复点结构体进行 `svtool rule check` 静态探测时，发现 DSL 编译器此前将 `rbsp_trailing_bits;` 严格限制为顶层无条件指令：
```text
probe_sei_recovery_point.svfmt:15:17: error: rbsp_trailing_bits must occur once as the final top-level item
```
为了能够干净表达 ITU-T H.264 clause 7.3.2.3.1 的载荷对齐语义，必须允许 `rbsp_trailing_bits;` 出现在条件分支（`if` / `switch`）的末尾，同时继续禁止直接出现在 repeat 循环体顶层。

### 与 `payload_size` 的关系

在标准 H.264 码流中，`sei_message()` 声明的 `payload_size` 表示载荷经由对齐位填充至字节边界后的总字节数。当 `payload_type == 6` 时，结构化语法字段与分支内的 `rbsp_trailing_bits;` 共同消费该消息并将 bit reader 对齐至消息结束处的字节边界。其余尚未结构化解析的 SEI 载荷类型继续使用 `@lazy(payload_size) bytes payload_data` 精确消费 `payload_size` 字节，确保未支持类型保持安全的不透明状态。

## 决策

1. **恢复点语法元素**：
   在 `SeiRbsp` 中，当 `payload_type == 6` 时解码恢复点字段：
   - `ue recovery_frame_cnt`
   - `bits<1> exact_match_flag`
   - `bits<1> broken_link_flag`
   - `bits<2> changing_slice_group_idc @range(0, 2)`
   - `rbsp_trailing_bits;` 用于消费 `bit_equal_to_one` 及对齐零位。

2. **其余载荷类型保持 Opaque**：
   除 6 以外的 SEI 载荷类型继续保持不透明：
   ```svfmt
   if (payload_type == 6) {
       ue recovery_frame_cnt
           @spec("ITU-T H.264", "D.1.7, D.2.7")
           @description("Specifies the recovery frame count.");
       bits<1> exact_match_flag
           @spec("ITU-T H.264", "D.1.7, D.2.7")
           @description("Indicates whether decoding provides an exact match.");
       bits<1> broken_link_flag
           @spec("ITU-T H.264", "D.1.7, D.2.7")
           @description("Indicates whether the previous reference pictures may be missing.");
       bits<2> changing_slice_group_idc @range(0, 2)
           @spec("ITU-T H.264", "D.1.7, D.2.7")
           @description("Indicates whether changing slice groups are present.");
       rbsp_trailing_bits;
   } else {
       @lazy(payload_size)
       bytes payload_data
           @spec("ITU-T H.264", "7.3.2.3.1")
           @description("Carries the raw SEI message payload bytes.");
   }
   ```

3. **DSL 条件尾部对齐支持**：
   允许 `rbsp_trailing_bits;` 作为结构体内条件分支的终止语句。编译器将其 lower 为受分支条件与迭代索引保护的 `ReadRbspTrailingBits`，VM 仅在分支激活时执行 stop bit 与对齐零位的读取。

4. **规则包版本**：
   将 `org.streamview.h264` 规则包版本从 `0.1.31` 升级至 `0.1.32`。

## 影响

- 恢复点 SEI 消息被完整解析为具有精确逐 bit source spans、规范引用与说明的类型化语法节点；
- 同一 NAL 单元中紧跟恢复点之后的 SEI 消息从正确字节边界起始，无比特漂移；
- `changing_slice_group_idc` 的保留越界值（值 3）按 ADR-0036 与 ADR-0076 产生非致命 `invalid-syntax` 警告诊断，保持载荷物化；
- 未支持的 SEI 载荷类型安全保持为不透明 lazy 字节区间。

## 后续

- ADR-0036：在字段上强制执行范围与相等性值域约束
- ADR-0076：有符号动态范围与相等性值域约束
- ADR-0079：使用 ff_coded 编码累加字节数值
- ADR-0080：在 RBSP 数据上有界迭代重复

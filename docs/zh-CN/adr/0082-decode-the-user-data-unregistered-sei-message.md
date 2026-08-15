# 解析未注册用户数据 SEI 消息

状态：已接受
日期：2026-08-15

## 背景

ITU-T H.264 clause 7.3.2.3 规定了补充增强信息（SEI）RBSP 容器，clause 7.3.2.3.1 定义了载荷语法派发。Clause D.1.6 与 clause D.2.6 定义了未注册用户数据（user data unregistered）SEI 消息（`payloadType == 5`），用于承载由标准 UUID 标识的自定义用户数据：

```text
user_data_unregistered( payloadSize ) {
    for( i = 0; i < 16; i++ )
        uuid_iso_iec_11578[ i ]                          u(8)
    for( i = 16; i < payloadSize; i++ )
        user_data_payload_byte                           b(8)
}
```

在 clause D.2.6 中：
- `uuid_iso_iec_11578[ i ]`：由 ISO/IEC 11578:1996 附录 A 规定的 16 字节（128 比特）UUID 值；
- `user_data_payload_byte`：长度为 $payload\_size - 16$ 字节的原始用户数据载荷。

由于 16 字节的 UUID 与 $(payload\_size - 16)$ 字节的用户数据均为 8 比特的整数倍，载荷本身已严格字节对齐。依 ITU-T H.264 clause 7.3.2.3.1，此消息末尾不包含载荷对齐填充位。

### 探测与语言能力分析

在 scratch 副本中探测确认：
1. `SeiRbsp` 中使用 `switch (payload_type)` 提供了与 NAL 派发表风格一致、易于扩展的结构；
2. 定长数组语法 `bits<8> uuid_iso_iec_11578[16]` 能够正确物化为 16 个索引节点（`uuid_iso_iec_11578[0]` .. `uuid_iso_iec_11578[15]`）；
3. 延迟字节表达式 `@lazy(payload_size - 16) bytes user_data_payload_byte` 动态计算剩余载荷字节长度；
4. 当 $payload\_size < 16$ 时，算术减法下溢保护自动产生 `invalid-syntax` 诊断并安全回滚事务；
5. 所有语义均可由既有 DSL 能力完整表达，无需修改 DSL 编译器或虚拟机。

## 决策

1. **未注册用户数据语法元素**：
   在 `SeiRbsp` 中，当 `payload_type == 5` 时解码：
   - `bits<8> uuid_iso_iec_11578[16]`
   - `@lazy(payload_size - 16) bytes user_data_payload_byte`

2. **基于 Switch 的 SEI 载荷派发**：
   将 `SeiRbsp` 的载荷派发由 `if/else` 重构为 `switch (payload_type)`：
   ```svfmt
   switch (payload_type) {
       case 5: {
           bits<8> uuid_iso_iec_11578[16]
               @spec("ITU-T H.264", "D.1.6, D.2.6")
               @description("Specifies the UUID identifying the syntax and semantics of the unregistered user data.");
           @lazy(payload_size - 16)
           bytes user_data_payload_byte
               @spec("ITU-T H.264", "D.1.6, D.2.6")
               @description("Carries the raw unregistered user data bytes.");
       }
       case 6: {
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
       }
       default: {
           @lazy(payload_size)
           bytes payload_data
               @spec("ITU-T H.264", "7.3.2.3.1")
               @description("Carries the raw SEI message payload bytes.");
       }
   }
   ```

3. **规则包版本**：
   将 `org.streamview.h264` 规则包版本从 `0.1.32` 升级至 `0.1.33`。

## 影响

- 未注册用户数据 SEI 消息被解析为包含 16 字节 UUID 数组与延迟用户数据字节的类型化语法节点；
- 当 $payload\_size == 16$ 时，用户数据字节区域在字节边界处物化为 0 长度节点；
- $payload\_size < 16$ 的畸变数据因减法下溢保护被正确拒绝为非法语法；
- SEI 载荷派发表组织为清晰的 `switch` 结构，为后续载荷类型（T9–T12）奠定基础；
- 未支持的载荷类型继续通过 `default` 分支解析为不透明 lazy 字节区间。

## 后续

- ADR-0036：在字段上强制执行范围与相等性值域约束
- ADR-0079：使用 ff_coded 编码累加字节数值
- ADR-0080：在 RBSP 数据上有界迭代重复
- ADR-0081：解析恢复点 SEI 消息

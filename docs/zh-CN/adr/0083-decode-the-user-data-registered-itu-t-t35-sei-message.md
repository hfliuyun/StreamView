# 解析 ITU-T T.35 注册用户数据 SEI 消息

状态：已接受
日期：2026-08-15

## 背景

ITU-T H.264 clause 7.3.2.3 规定了补充增强信息（SEI）RBSP 容器，clause 7.3.2.3.1 定义了载荷语法派发。Clause D.1.5 与 clause D.2.5 定义了由 ITU-T T.35 建议书注册的用户数据（user data registered by Recommendation ITU-T T.35）SEI 消息（`payloadType == 4`），用于承载由国家代码标识的自定义或标准扩展数据：

```text
user_data_registered_itu_t_t35( payloadSize ) {
    itu_t_t35_country_code                          b(8)
    if( itu_t_t35_country_code != 0xFF )
        i = 1
    else {
        itu_t_t35_country_code_extension_byte       b(8)
        i = 2
    }
    do {
        itu_t_t35_payload_byte                      b(8)
        i++
    } while( i < payloadSize )
}
```

在 clause D.2.5 中：
- `itu_t_t35_country_code`：8 比特字段，按 ITU-T T.35 建议书附录 A 规定国家代码。当其值为 `0xFF`（255）时，紧随其后的 `itu_t_t35_country_code_extension_byte` 指定扩展国家代码；
- `itu_t_t35_country_code_extension_byte`：8 比特字段，在 `itu_t_t35_country_code == 0xFF` 时指定扩展国家代码；
- `itu_t_t35_payload_byte`：承载已注册载荷的字节序列，其具体语法与语义由对应国家代码机构（如 ATSC、CTA、DVB、HDR10+）定义。

由于消息头部由 1 或 2 个完整字节构成，载荷本身为整字节序列（$payload\_size - 1$ 或 $payload\_size - 2$ 字节），整个消息在语法上严格字节对齐。依 ITU-T H.264 clause 7.3.2.3.1，此消息末尾不包含载荷对齐填充位。

### 探测与语言能力分析

在 scratch 副本中探测确认：
1. `SeiRbsp` 中新增 `case 4` 能够直接融入既有 `switch (payload_type)` 派发表；
2. DSL 要求同一结构体内字段名全局唯一，因此扩展分支定义 `bits<8> itu_t_t35_country_code_extension_byte` 与 `@lazy(payload_size - 2) bytes itu_t_t35_extension_payload_byte`，标准分支定义 `@lazy(payload_size - 1) bytes itu_t_t35_payload_byte`；
3. 当 `payload_size < 1`（或扩展分支中 `payload_size < 2`）时，算术减法下溢保护自动产生 `invalid-syntax` 诊断并安全回滚事务；
4. 所有语义均可由既有 DSL 能力完整表达，无需修改 DSL 编译器或虚拟机。

## 决策

1. **ITU-T T.35 注册用户数据语法元素**：
   在 `SeiRbsp` 中，当 `payload_type == 4` 时解码：
   ```svfmt
   case 4: {
       bits<8> itu_t_t35_country_code
           @spec("ITU-T H.264", "D.1.5, D.2.5")
           @description("Specifies the ITU-T Recommendation T.35 country code.");
       if (itu_t_t35_country_code == 255) {
           bits<8> itu_t_t35_country_code_extension_byte
               @spec("ITU-T H.264", "D.1.5, D.2.5")
               @description("Specifies the ITU-T Recommendation T.35 country code extension byte.");
           @lazy(payload_size - 2)
           bytes itu_t_t35_extension_payload_byte
               @spec("ITU-T H.264", "D.1.5, D.2.5")
               @description("Carries the raw ITU-T Recommendation T.35 payload bytes when an extension byte is present.");
       } else {
           @lazy(payload_size - 1)
           bytes itu_t_t35_payload_byte
               @spec("ITU-T H.264", "D.1.5, D.2.5")
               @description("Carries the raw ITU-T Recommendation T.35 payload bytes.");
       }
   }
   ```

2. **规则包版本**：
   将 `org.streamview.h264` 规则包版本从 `0.1.33` 升级至 `0.1.34`。

## 影响

- ITU-T T.35 注册用户数据 SEI 消息被解析为包含国家代码与延迟载荷字节的类型化语法节点；
- 标准国家代码（`country_code != 255`）物化 `itu_t_t35_country_code` 与 `itu_t_t35_payload_byte`；
- 扩展国家代码（`country_code == 255`）物化 `itu_t_t35_country_code`、`itu_t_t35_country_code_extension_byte` 与 `itu_t_t35_extension_payload_byte`；
- 0 长度载荷配置（标准 1 字节或扩展 2 字节）在字节边界处物化为 0 长度节点；
- 长度不足的畸变数据（标准 0 字节或扩展 1 字节）因减法下溢保护被正确拒绝为非法语法。

## 后续

- ADR-0036：在字段上强制执行范围与相等性值域约束
- ADR-0079：使用 ff_coded 编码累加字节数值
- ADR-0080：在 RBSP 数据上有界迭代重复
- ADR-0081：解析恢复点 SEI 消息
- ADR-0082：解析未注册用户数据 SEI 消息

# 使用 ff_coded 编码累加字节数值

状态：已接受
日期：2026-08-14

## 背景

ITU-T H.264 clause 7.3.2.3.1（以及 HEVC、VVC 和 AV1 中的类似规范）使用变长字节累加循环
定义补充增强信息（SEI）消息头的 `payloadType` 与 `payloadSize`：

```text
while( next_bits( 8 ) == 0xFF ) {
    ff_byte /* equal to 0xFF */
    payloadType += 255
}
last_payload_type_byte
payloadType += last_payload_type_byte
```

在 StreamView DSL 中使用 `svtool rule check` 进行实测探测确认，现有语法结构无法表达该语义：

1. Sentinel repeat 循环（`repeat (N) { ... } until (sentinel == value)`）仅支持针对字面常量的等值比对（ADR-0070）。尝试编写 `until (ff_byte != 255)` 会报错：
   ```text
   error: Expected '==' after sentinel field name
   ```
2. DSL 不支持跨 repeat 重复元素的规约/求和表达式（如 `sum(ff_byte)`）。尝试在计算字段中引用重复元素会报错：
   ```text
   error: Computed field dependency must be declared earlier
   error: Pure function is not declared before this call
   ```

`payloadType` 与 `payloadSize` 在语法概念上是承载无符号整数标量值的单个语法元素。如果通过
数组 repeat 和规约算子来表达，会引入不必要的 AST 碎片化和非局部的求和算术。

## 决策

在 StreamView DSL 中引入原生的标量字段编码 `ff_coded<max_bytes>`：

1. **语法**：
   ```svfmt
   ff_coded<8> payload_type
       @description("Carries the SEI message payload type.");
   ff_coded<8> payload_size
       @description("Carries the SEI message payload size in bytes.");
   ```
   `max_bytes` 是必须的编译期正整数字面量，用于约束该字段允许读取的最大字节数（`1 <= max_bytes <= 64`）。

2. **解码语义**：
   - 解码器按顺序连续读取 8-bit 字节；
   - 对于每个取值为 `0xFF`（255）的字节，将 255 累加至累加器并继续读取下一字节；
   - 首个数值小于 `0xFF` 的字节将其值加至累加器并结束该字段的解码；
   - 若已读取 `max_bytes` 个字节但最后一个字节仍为 `0xFF`，解码失败并生成 `invalid-syntax` 诊断；
   - 若在读取到终止字节之前剩余数据少于 8 bit，解码失败并生成截断诊断。

3. **AST 节点呈现**：
   - 该字段生成单个标量 `u64` AST 节点，其值为累加求和结果（`255 * N + last_byte`）；
   - 该字段的 source span 完整覆盖所消费的连续字节范围（所有 `0xFF` 前缀字节加上最终终止字节），与 `ue` 和 `se` 的 mapped span 行为完全一致。

## 影响

- H.264 SEI 消息（clause 7.3.2.3.1）中的 `payloadType` 与 `payloadSize` 语法元素可以声明式地定义为单个标量字段；
- DSL 避免引入通用的循环累加或数组规约复杂性；
- 字节消费保持绝对有界与确定性。

## 非目标

- 本决策不引入任意进制的字节累加或通用的循环规约算术；
- 外层消息循环重复（`while( more_rbsp_data() )`）在独立的设计决策（ADR-0080）中解决。

## 后续

- ADR-0070：使用哨兵重复匹配列表修改终止符
- ADR-0080：在 RBSP 数据上有界迭代重复

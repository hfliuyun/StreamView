# ADR-0089：字节对齐谓词表达式（Byte-Aligned Predicate Expression）

## 状态

已接受 (Accepted)

## 背景

在 ITU-T H.264 条款 7.3.2.3.1（`sei_payload`）中，SEI 消息末尾的对齐位被明确规定为**条件性**语法：

```text
if( !byte_aligned( ) ) {
    bit_equal_to_one /* equal to 1 */
    while( !byte_aligned( ) )
        bit_equal_to_zero /* equal to 0 */
}
```

在先前已实现的 SEI 消息（如 `buffering_period`、`recovery_point`、`display_orientation`）中，消息载荷总位宽由于包含奇数宽度的 `ue` 元素或奇数个标志位，在数学上**恒非 8 的整数倍**。因此 `!byte_aligned()` 恒为真，在分支末尾无条件书写 `rbsp_trailing_bits;` 即可准确消费 1..7 个填充位。

然而，在诸如 `pic_timing`（ITU-T H.264 D.1.2 / D.2.2）等消息中：当 `CpbDpbDelaysPresentFlag` 有效且使用缺省 24 位延迟（24 + 24 = 48 位），同时 `pic_struct_present_flag == 0` 时，载荷总位宽恰好为 48 位（即 6 字节，为 8 的整数倍）。在合规码流中，此时**没有任何对齐位**。若在规则末尾无条件放置 `rbsp_trailing_bits;`，虚拟机字节码 `ReadRbspTrailingBits` 将强制读取 8 位（`10000000`），从而错误吞食后续 SEI 消息的 `payload_type` 或 NAL 单元末尾的 trailing bits，造成码流严重错位。

规则引擎探测表明：
1. 分支内的条件性 `rbsp_trailing_bits`（`if (condition) { rbsp_trailing_bits; }`）在解析器（`src/rules/dsl.cpp:1360-1372`）、IR 降级（`src/rules/dsl_ir.cpp:3853-3861`）和 VM 执行（`src/rules/dsl_vm.cpp:2912-2939`）中**已获完整支持**。当条件为 false 时，VM 干净跳过所有对齐位，不从 reader 读取任何 bit。
2. 当前表达式语言仅有用于查询码流结束状态的 `more_rbsp_data()`，尚无查询当前 bit 是否字节对齐的位置谓词 `byte_aligned()`。

## 决策

我们在 DSL 表达式语法、静态类型 IR 与虚拟机运行时中引入内置零参布尔表达式 `byte_aligned()`：

1. **语法与解析**：
   - `byte_aligned()` 解析为零参调用表达式，返回类型为 `DslScalarType::Bool`。
   - 禁止在纯函数（`pure`）内部使用（出处 `src/rules/dsl.cpp`），因为纯函数只对传入的标量求值，不持有外层 `BitReader` / 码流上下文（与 `more_rbsp_data()` 限制一致）。
2. **类型 IR 降级**：
   - 降级为 `DslTypedExpressionKind::ByteAligned`（`src/rules/dsl_ir.cpp`），标量类型为 `DslScalarType::Bool`。
3. **VM 求值与坐标系定义**：
   - 求值逻辑为 `(logicalStart + reader.position()) % 8 == 0`（`src/rules/dsl_vm.cpp`）。
   - `logicalStart` 为当前结构体在 `BitReader` / `AnalysisSource` 逻辑坐标系中的绝对起始 bit 地址。
   - 在经过 EBSP $\to$ RBSP 映射的切片或子区域中，`logicalStart + reader.position()` 精确表达当前逻辑 bit 坐标。
   - 对于 `@lazy` 字节区域，区域按 ADR-0026 规范本身保证绝对字节对齐，因此在 lazy 区域内外均维持坐标系一致性。
4. **格式无关性**：
   - 表达式属于通用语言层能力，核心与 DSL 运行时严禁内嵌任何 H.264 或 SEI 专属语义。
5. **规则层消费形态**：
   - 格式规则统一通过布尔计算字段中间变量（出处 `src/rules/official/org.streamview.h264/src/h264_annex_b.svfmt:894-898`）实现条件对齐：
     ```svfmt
     computed<bool> is_aligned = byte_aligned();
     computed<bool> needs_trailing_bits = !is_aligned;
     if (needs_trailing_bits) {
         rbsp_trailing_bits;
     }
     ```

## 影响

### 正向收益
- 忠实映射 ITU-T H.264 条款 7.3.2.3.1 的对齐语义（通过 `computed<bool> is_aligned = byte_aligned(); computed<bool> needs_trailing_bits = !is_aligned; if (needs_trailing_bits) { rbsp_trailing_bits; }` 落地）。
- 使 `pic_timing` 及未来格式消息能够安全应对对齐与非对齐两种位宽形态，彻底避免码流吞字节与错位。
- 复用既有 VM `ReadRbspTrailingBits` 执行逻辑，无需更改指令集架构。

### 负向影响 / 代价
- 表达式语言新增一个保留内置标识符 `byte_aligned`。

### 能力测试矩阵
- `byte_aligned()` 在 bit 偏移 0, 8, 16, 24, 32, 40, 48 等位置求值为 true。
- 在 bit 偏移 1..7, 9..15 等位置求值为 false。
- 在 `repeat`、`switch`、`if` 条件块内部正确求值。
- 在紧随 `@lazy` 字节区域之后的位置正确求值。
- 验证直接条件形态 `if (!byte_aligned())` 会被条件文法解析闸门拦截（`src/rules/dsl.cpp:1180`，报错原文 `Conditions require a field or context_value equality`），而通过中间布尔计算变量的规范形态（`computed<bool> is_aligned = byte_aligned(); computed<bool> needs_trailing_bits = !is_aligned; if (needs_trailing_bits) { rbsp_trailing_bits; }`）可无缝编译并精准执行。


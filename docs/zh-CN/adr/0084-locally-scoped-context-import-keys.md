# 块内局部上下文导入键

状态：已接受
日期：2026-08-15

## 背景

ITU-T H.264 规定了若干依赖激活序列参数集（SPS）的补充增强信息（SEI）载荷类型。例如，clause D.1.1 与 clause D.2.1 规定了缓冲周期（Buffering Period）SEI 消息（`payload_type == 0`）：

```text
buffering_period( payloadSize ) {
    seq_parameter_set_id                            ue(v)
    if( !HrdParamPresentFlag )
        // 无 HRD 字段
    if( NalHrdBpPresentFlag ) {
        for( SchedSelIdx = 0; SchedSelIdx <= cpb_cnt_minus1; SchedSelIdx++ ) {
            initial_cpb_removal_delay[ SchedSelIdx ]          u(v)
            initial_cpb_removal_delay_offset[ SchedSelIdx ]   u(v)
        }
    }
    if( VclHrdBpPresentFlag ) {
        for( SchedSelIdx = 0; SchedSelIdx <= cpb_cnt_minus1; SchedSelIdx++ ) {
            initial_cpb_removal_delay[ SchedSelIdx ]          u(v)
            initial_cpb_removal_delay_offset[ SchedSelIdx ]   u(v)
        }
    }
}
```

在 `buffering_period` 中，`seq_parameter_set_id` 在 SEI 消息载荷内部解析，其值用于向已激活的 SPS 查询 HRD 参数（`nal_hrd_parameters_present_flag`、`nal_hrd_cpb_cnt_minus1`、`nal_hrd_initial_cpb_removal_delay_length_minus1` 等），以驱动：
1. 循环迭代次数（`repeat(cpb_cnt_minus1 + 1)`）；
2. 动态字段比特宽度（`bits<(initial_cpb_removal_delay_length_minus1 + 1)>`）。

### 探测与语言局限

在 `SeiRbsp` 中尝试表达此逻辑时：
```svfmt
@context_import("h264-sps", seq_parameter_set_id)
struct SeiRbsp {
    repeat (64) while (more_rbsp_data()) {
        ff_coded<8> payload_type;
        ff_coded<64> payload_size;
        switch (payload_type) {
            case 0: {
                ue seq_parameter_set_id;
                computed<u64> imported_nal_hrd_present =
                    context_value(seq_parameter_set_id, h264_sps, nal_hrd_parameters_present_flag);
            }
            default: {
                @lazy(payload_size) bytes payload_data;
            }
        }
    }
    rbsp_trailing_bits;
}
```

执行 `svtool rule check` 产生以下编译器报错：
```text
probe_t10_import.svfmt:8:1: error: Context import key field is not a top-level scalar field
probe_t10_import.svfmt:17:35: error: context_value import key must be an earlier context-eligible field
```

分析表明，DSL IR 编译器（`src/rules/dsl_ir.cpp`）存在两道位置闸门限制：
1. **注解闸门（`dsl_ir.cpp:3101-3134`）**：强制要求 `@context_import("...", keyFieldName)` 声明的键字段必须是结构体顶层无条件声明的标量字段；
2. **表达式闸门（`dsl_ir.cpp:1527-1537`）**：强制要求 `context_value(keyName, ...)` 的 `keyName` 必须解析为顶层已声明字段，而非分支保证的局部字段。

此外，控制流分支内部嵌套结构体实例化不是 DSL 的支持语法（`Expected bits<...>, ue, se, or ff_coded field type`），而为 SEI 定制专属核心机制则违反格式中立性原则。

## 决策

我们引入**块内局部上下文导入键（Locally Scoped Context Import Keys）**，在保持所有类型安全与作用域不变量的前提下，放宽上下文导入键的位置约束：

1. **声明语法保持不变**：
   结构体级别仍使用 `@context_import("h264-sps", seq_parameter_set_id)`。
   
2. **仅放宽导入角色闸门**：
   - 放宽注解闸门（`dsl_ir.cpp`），允许导入键字段声明在控制流分支（含 `repeat` 循环和 `switch`/`if` 块）内部；
   - 相比之下，`@context` 定义键与 `@context_dependency` 的顶层无条件约束保持不变，以确保目录注册的确定性。
   
3. **分支保证的静态绑定**：
   - 每个 `context_value(keyName, contextKind, fieldName)` 表达式静态绑定到当前执行路径上有保证且最近的更早声明（复用 `dsl_ir.cpp:1443-1497` 既有的分支保证分析）；
   - 若 `keyName` 在当前路径上无保证，或声明在互斥分支中，编译报错 `DslDiagnosticCode::InvalidContext`。

4. **键类型不变量**：
   - 导入键必须为无符号标量类型：无符号 `bits<N>`、`ue`、`ff_coded<N>` 或 `computed<u64>`；
   - 严格禁止有符号字段（`se`）和数组作为上下文键。

5. **每次迭代重新绑定**：
   - 在 `repeat` 展开循环中，每个展开迭代绑定各自局部的 key 槽位。同一迭代内的后续表达式针对该迭代 key 所选定的 SPS 代进行求值。

6. **故障隔离与局部失败**：
   - 若局部上下文键不可用（如 SPS ID 不存在或 generation 未命中），仅当前 SEI 消息转入 `waiting-dependency` 或 `invalid` 状态；SEI 容器依 `payload_size` 继续解析后续消息，不使整个 `SeiRbsp` 失败。

7. **范围边界（非目标）**：
   - 本能力严格要求载荷内存在明确的局部解析键字段。对于载荷内无 key 字段的消息（如 `pic_timing`），无法直接依赖本机制，须在 T11 前单独探测。

## 影响

- SEI 载荷如 `buffering_period`（`payload_type == 0`）能够在循环和 switch 分支内解析 `ue seq_parameter_set_id`，并直接查询 SPS HRD 上下文参数；
- 上下文目录查找、按流位置选代与缓存机制完全保持原样；
- 未引入新的语法解析维度或运行时动态作用域，所有绑定在 IR 编译期静态确定。

## 后续

- ADR-0028：上下文目录与 SPS/PPS 生命周期
- ADR-0078：在参数集重定义中按流位置选择上下文代
- ADR-0080：在 RBSP 数据上有界迭代重复
- ADR-0081：解析恢复点 SEI 消息
- ADR-0082：解析未注册用户数据 SEI 消息
- ADR-0083：解析 ITU-T T.35 注册用户数据 SEI 消息

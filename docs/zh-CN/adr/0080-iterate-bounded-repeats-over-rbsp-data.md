# 在 RBSP 数据上有界迭代重复

状态：已接受
日期：2026-08-14

## 背景

ITU-T H.264 clause 7.3.2.3 将补充增强信息（SEI）RBSP 定义为只要仍有 RBSP 数据就重复
执行 SEI 消息的一组序列：

```text
sei_rbsp( ) {
    do
        sei_message( )
    while( more_rbsp_data( ) )
    rbsp_trailing_bits( )
}
```

在 StreamView DSL 中使用 `svtool rule check` 进行实测探测确认，现有循环结构无法表达该语义：

1. Sentinel repeat 循环（`repeat (N) { ... } until (field == value)`）要求哨兵字段必须是
   直接定义在 repeat 体内的 source-backed scalar 字段（ADR-0070）。
2. 尝试在 sentinel `until` 子句中使用 `more_rbsp_data()` 会报错：
   ```text
   error: Sentinel field must be declared directly in the repeat body
   ```
3. 尝试在 sentinel `until` 子句中使用计算布尔字段（`computed<bool> is_done = more_rbsp_data() == false;`）
   同样会报错：
   ```text
   error: Sentinel field must be declared directly in the repeat body
   ```

为了支持解析 SEI 消息容器（以及类似由 RBSP 边界终止的重复结构），DSL 需要一种由码流
剩余 RBSP 数据状态驱动的有界循环结构。

## 决策

在 StreamView DSL 中引入由 RBSP 数据驱动的有界 repeat 循环：

1. **语法**：
   ```svfmt
   repeat (64) while (more_rbsp_data()) {
       SeiMessage message;
   }
   ```
   `max_iterations`（如 64）是必须的编译期正整数字面量，用于约束最大迭代次数（`1 <= max_iterations <= 1024`）。`while (more_rbsp_data())` 指定循环条件。

2. **执行语义**：
   - 在执行每次迭代之前（包括首次迭代），VM 评估 `more_rbsp_data()`；
   - 若 `more_rbsp_data()` 评估为 `true`，按序解码该次迭代的子字段；
   - 若 `more_rbsp_data()` 评估为 `false`，循环干净终止，执行流进入后续字段（如 `rbsp_trailing_bits`）；
   - 若已执行 `max_iterations` 次迭代且 `more_rbsp_data()` 仍为 `true`，解码失败并生成 `invalid-syntax` 诊断，表明循环超出了声明的上限；
   - 若迭代体内发生截断或语法错误，循环终止并向上冒泡错误。

3. **Typed IR 与字节码**：
   - 在类型化 IR 中表示为独立的 typed repeat 结构（`DslTypedWhileRepeat` 或扩展的 `DslTypedRepeat`），携带 `maximumIterations` 与条件表达式 `more_rbsp_data()`；
   - 编译器在每次循环迭代前生成条件评估与跳转字节码，在 `more_rbsp_data()` 为 false 时跳过 repeat 块。

## 影响

- H.264 SEI RBSP（clause 7.3.2.3）消息循环可以声明式表达，无需人造哨兵字段或手动计数；
- 执行保持由 `max_iterations` 绝对有界，防止死循环或无节制的节点物化；
- DSL 类型系统与 VM 执行模型保持确定性与取消安全性。

## 非目标

- 本决策不引入任意无界 while 循环或通用运行时 while 条件。`more_rbsp_data()` 是当前唯一支持的循环条件谓词。

## 后续

- ADR-0070：使用哨兵重复匹配列表修改终止符
- ADR-0079：使用 ff_coded 编码累加字节数值

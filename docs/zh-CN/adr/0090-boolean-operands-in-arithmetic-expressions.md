# ADR-0090：加法与乘法算术表达式接受布尔操作数（Boolean Operands in Additive and Multiplicative Arithmetic Expressions）

## 状态

已接受 (Accepted)

## 背景

在视频编码规范中（如 ITU-T H.264 表格 D-1 关于 `pic_struct` 到 `NumClockTS` 的映射），分段表格函数通常采用指示函数加权和（indicator-weighted sums）表达：

$$\text{NumClockTS} = (pic\_struct \le 2) \cdot 1 + (pic\_struct \in \{3, 4, 7\}) \cdot 2 + (pic\_struct \in \{5, 6, 8\}) \cdot 3$$

在格式描述语言中，书写如下纯函数或计算字段：
```svfmt
pure u64 num_clock_ts_for_pic_struct(u64 pic_struct) {
    return (pic_struct <= 2) * 1 +
           (pic_struct == 3 || pic_struct == 4 || pic_struct == 7) * 2 +
           (pic_struct == 5 || pic_struct == 6 || pic_struct == 8) * 3;
}
```
目前会被编译器两道类型检查闸门严格拦截：
1. **解析器层检查点**：`src/rules/dsl.cpp:2051-2058` 在 `requireOperands` 中校验 `scalarType(operand) != DslScalarType::U64`，并在 `src/rules/dsl.cpp:2066` 报出 `Arithmetic operators require u64 operands`；
2. **类型 IR 层检查点**：`src/rules/dsl_ir.cpp:897` 强制 `typesValid = left->type == DslScalarType::U64 && right->type == DslScalarType::U64;`，并在 `src/rules/dsl_ir.cpp:930` 报出 `Expression operand types do not match the operator`。

由于关系和逻辑运算符（`<=`、`==`、`||`）产出 `DslScalarType::Bool`，而类型系统先前禁止 `Bool` 与 `U64` 参与算术运算，导致无法在语言内直接表达分段指示算术。

## 决策

我们严格针对具有交换律和单调性的加法 `+`（`Add`）与乘法 `*`（`Multiply`）运算符放宽操作数类型限制，允许 `DslScalarType::Bool` 操作数并自动强转为 `0ULL` 或 `1ULL`：

1. **放宽范围（严格限定为 `+` 与 `*`）**：
   - `+`（`Add`）与 `*`（`Multiply`）的操作数各自独立接受 `DslScalarType::U64` 或 `DslScalarType::Bool`；
   - 运算结果类型恒为 `DslScalarType::U64`；
   - **不放宽其余算术运算符的理由**：
     - 减法 `-`（`Subtract`）：若允许布尔操作数，`false - 1` 或 `false - true` 将直接导致无符号 64 位整型下溢（回绕至 $2^{64}-1$），破坏数值安全性；
     - 除法 `/`（`Divide`）与取模 `%`（`Remainder`）：若允许布尔操作数，当除数为 `false` 时将产生运行时除零崩溃（`/ 0`）。
     - 因此仅有 `+` 与 `*` 在语义与数值上绝对安全。
2. **强转规则与边界语义**：
   - `true` 强转为 `1ULL`；`false` 强转为 `0ULL`；
   - `bool + bool` $\to$ `u64`（例如 `true + true == 2`、`true + false == 1`、`false + false == 0`）；
   - `bool * bool` $\to$ `u64`（例如 `true * true == 1`、`true * false == 0`）；
   - `bool + u64` / `u64 + bool` $\to$ `u64`；
   - `bool * u64` / `u64 * bool` $\to$ `u64`。
3. **双闸门实现要点**：
   - **解析器层**（`src/rules/dsl.cpp:2221-2235`）：
     - 对 `Add` 与 `Multiply`，校验 `leftType` 与 `rightType` 均在 `{U64, Bool}` 集合中，类型非法时发出 `Add and multiply operators require u64 or bool operands`；
     - 对 `Subtract`、`Divide`、`Remainder`，保留对 `U64` 的严格要求并输出既有的共享诊断 `Arithmetic operators require u64 operands`（保持与既有算术诊断文本一致，无需引入冗余的新错误文本）；
   - **类型 IR 层**（`src/rules/dsl_ir.cpp:923-930`）：
     - 对 `Add` 与 `Multiply`，当 `left` 与 `right` 的类型为 `U64` 或 `Bool` 时置 `typesValid = true`，`resultType = DslScalarType::U64`；
     - 对 `Subtract`、`Divide`、`Remainder`，保留 `typesValid = left->type == U64 && right->type == U64`；
   - **VM 求值层**（`src/rules/dsl_vm.cpp:845-850`）：
     - 在 `evaluateTypedExpression` 中，执行加法与乘法前将 `Bool` 标量转换为 `1ULL`（true）或 `0ULL`（false）。

## 影响

### 正向收益
- 直接赋能纯函数和计算字段表达标准规范中的分段指示加权和（如 ITU-T H.264 表格 D-1）。
- 严禁减法和除法混用布尔类型，完整保全无符号算术与除零防护安全。
- 保持对既有 `.svfmt` 规则的 100% 向后兼容。

### 负向影响 / 代价
- 在加法和乘法上下文中引入从 `Bool` 到 `U64` 的隐式数值提升。

### 能力测试矩阵
- 纯函数映射覆盖 ITU-T H.264 表格 D-1 全部 16 个 `pic_struct` 值（$0 \le \text{pic\_struct} \le 15$）：
  - 0, 1, 2 $\to$ 1
  - 3, 4, 7 $\to$ 2
  - 5, 6, 8 $\to$ 3
  - 9..15 $\to$ 0
- 单元测试覆盖 `bool + bool`、`bool * bool`、`bool * u64`、`u64 * bool`、`bool + u64`、`u64 + bool`；
- 负向测试断言 `bool - u64`、`u64 - bool`、`bool / u64`、`u64 / bool`、`bool % u64` 在编译阶段被正确拒绝。

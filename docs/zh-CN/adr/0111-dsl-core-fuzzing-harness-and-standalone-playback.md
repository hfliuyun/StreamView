# ADR-0111: DSL 核心模糊测试框架、独立回放与不变式验证

- **状态**：Proposed
- **日期**：2026-09-07
- **作者**：StreamView 贡献者

---

## 背景

StreamView 深度依赖其自研声明式领域特定语言（DSL）来描述和解析复杂的媒体容器与底层码流格式（ISO BMFF MP4、H.264 Annex B、AAC ADTS/ASC）。DSL 核心引擎由三个紧密耦合的处理阶段组成：
1. **词法与语法解析器（`DslLexer`, `DslParser`）**：摄入原始 `.svfmt` 文本，产出语法抽象语法树（`DslProgram`）或语法诊断信息（`DslDiagnostic`）；
2. **编译器与 IR 下沉（`DslCompiler`）**：摄入 `DslProgram`，执行符号消歧、类型推断与检查、字段有效性验证及依赖分析，将合法构造下沉为携带执行字节码的类型化程序（`DslTypedProgram`）；
3. **虚拟机与执行器（`DslVirtualMachine`, `DslExecutor`）**：通过 `core::BitReader` 针对未经验证的比特流解释执行字节码指令，并严格约束内存分配、指令预算和循环上限。

非信任的媒体文件、外部用户传入的 `.svrule` 扩展包以及任意网络/磁盘字节流直接接触上述三大引擎边界。任何潜在的安全脆弱点（如递归深度过大导致的栈溢出、堆越界读取、无限死循环或未捕获异常）都会引发严重的拒绝服务或内存破坏风险。

依照 ADR-0110 的架构规划，阶段 7 必须在这三大核心信任边界上确立坚固、持续运行的模糊测试（Fuzzing）框架。此外，StreamView 的跨平台 CI 矩阵涵盖 Ubuntu 24.04、macOS 15 与 Windows 2022。虽然 LLVM `libFuzzer` 引擎在基于 Clang 的 Linux/macOS 环境中非常适合进行大规模变异探索，但在 Windows MSVC 或日常开发预设中并不默认集成 `libFuzzer` 运行时。因此，必须设计双模模糊测试架构，确保所有 fuzz 目标在三大平台上均能作为确定性测试在 CTest 中无缝运行。

---

## 架构决策

### 1. 双模架构：libFuzzer 变异引擎与独立确定性回放

**决策**：实现统一的 fuzz 目标结构，根据 CMake 配置项 `STREAMVIEW_ENABLE_LIBFUZZER`（默认 `OFF`）条件编译为 LLVM `libFuzzer` 目标或独立确定性测试运行器。

1. **标准 `libFuzzer` 入口**：
   每个 fuzz 目标均暴露标准 libFuzzer C 入口：
   ```cpp
   extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
   ```
2. **可复用独立驱动（`standalone_fuzz_driver.h`）**：
   当 `STREAMVIEW_ENABLE_LIBFUZZER` 关闭时：
   - 驱动提供通用 `main(int argc, char** argv)` 入口；
   - 若命令行传入文件或目录路径，驱动将遍历全部指定文件，将其字节内容逐一送入 `LLVMFuzzerTestOneInput` 执行；
   - 若未传入命令行参数，默认遍历该目标对应的种子语料目录（通过编译宏 `STREAMVIEW_FUZZ_CORPUS_DIR` 注入），或在目录不存在时自动执行内置合成种子用例；
   - 任何崩溃、内存泄漏或未捕获异常都会直接导致 CTest 判定失败。在 `cmake --preset sanitize` 下，ASan 与 UBSan 会对每一次语料回放执行全量插桩监控。

---

### 2. DSL 语法解析器 Fuzz 目标（`fuzz_dsl_parser`）

**决策**：实现 `fuzz_dsl_parser`，被测接口为 `streamview::rules::DslParser::parse(const QString& source)`。

- **输入摄入**：接收任意原始字节缓冲区，通过 `QString::fromUtf8` 安全转换为 `QString`（采用有损容错解码，杜绝转换层自身崩溃）。
- **威胁模型与不变式**：
  1. **零崩溃保证**：任意字符序列、控制字符、未闭合字符串、未闭合块注释、超长数字字面量绝不崩溃，严禁 panic；
  2. **递归深度受限**：多层深层嵌套的括号、花括号与运算符链必须能被语法诊断机制优雅捕获并拒绝，严禁耗尽栈空间；
  3. **确定性输出**：对同一输入重复执行解析，其产出的诊断列表与 AST 结构必须严格一致。

---

### 3. DSL 编译器 Fuzz 目标（`fuzz_dsl_compiler`）

**决策**：实现 `fuzz_dsl_compiler`，被测接口为 `streamview::rules::DslCompiler::compile` 与 `compileForTarget`。

- **输入摄入**：首先通过 `DslParser::parse` 将输入解析为 `DslProgram`。不论解析是否完全成功或是否包含语法诊断，均无条件将所得 `program` 送入 `DslCompiler::compile(parseResult.program)` 及目标特化编译接口。
- **威胁模型与不变式**：
  1. **语义检查安全**：循环结构体引用、重名冲突、非法位宽、不支持的注解及类型不匹配必须清晰通过 `DslCompileResult::diagnostics` 报告；
  2. **零中断与零异常**：无论 AST 如何畸变，编译器绝不触发 `assert`、绝不抛出未捕获异常、绝不对 `std::nullopt` 执行解引用；
  3. **字节码合法性**：编译成功时，产出的字节码操作码及操作数必须严格落在合法枚举与索引范围内。

---

### 4. DSL 虚拟机与执行器 Fuzz 目标（`fuzz_dsl_vm`）

**决策**：实现 `fuzz_dsl_vm`，被测接口为 `streamview::rules::DslVirtualMachine::execute` 与 `streamview::rules::DslExecutor::decodeStruct`。

- **输入摄入**：
  - 输入字节流作为非信任的比特流载荷；
  - 目标构建并在全功能测试 Schema 上执行，覆盖全部 DSL 核心特性：
    - 定长与动态位宽字段（`bits<N>`, `endian = little/big`）；
    - 指数哥伦布编码字段（`unsigned_exp_golomb`, `signed_exp_golomb`）；
    - 字节对齐序列编码（`ff_coded`）；
    - 计算字段与算术/逻辑表达式；
    - 条件分支（`if/else`）、`switch` 分发与多 case 匹配；
    - 有界循环：`repeat(N)`、哨兵循环（`until sentinel`）与 `while(condition)` 循环；
    - Lazy 字节区间与压缩载荷节点；
    - 源码断言与 unsupported 标记。
  - 同时在官方规则包（H.264、AAC、MP4）的编译后 Schema 上执行合成切片解码。
- **威胁模型与不变式**：
  1. **严格限制指令预算**：一旦执行步数超过 `maximumInstructions`，必须以 `DslExecutionStatus::ResourceLimit` 优雅终止，防范死循环与 CPU 耗尽；
  2. **循环迭代上限**：While 循环与哨兵循环必须强力实施编译期与运行期上限（`maximumWhileRepeatIterations = 1024`, `maximumSentinelRepeatIterations = 64`）；
  3. **零越界位读取**：`core::BitReader` 边界检查必须拦截一切越界读取；截断码流必须安全返回 `TruncatedSource` 或 `InvalidSyntax`，严禁内存越界访问；
  4. **资源分配上限**：节点物化深度与总节点数限制必须严格遵循配置阈值。

---

### 5. 种子语料组织与 CMake/CTest 集成

**决策**：在 `tests/fixtures/fuzz/` 中构建种子语料，并将 fuzz 可执行目标直接接入 CTest。

1. **目录结构**：
   - `tests/fuzz/`：Fuzz 驱动源码（`standalone_fuzz_driver.h`, `fuzz_dsl_parser.cpp`, `fuzz_dsl_compiler.cpp`, `fuzz_dsl_vm.cpp`）；
   - `tests/fixtures/fuzz/dsl_parser/`：解析器种子语料（合法规则片段、畸变片段、极值边界）；
   - `tests/fixtures/fuzz/dsl_compiler/`：编译器种子语料（类型边界配置、递归结构、多 entry 规则）；
   - `tests/fixtures/fuzz/dsl_vm/`：虚拟机种子语料（零长、单字节、截断头、高熵随机块、病态比特模式）。
2. **CMake 构建集成**：
   - 生成三个可执行目标：`streamview_fuzz_dsl_parser`、`streamview_fuzz_dsl_compiler`、`streamview_fuzz_dsl_vm`；
   - 链接 `StreamView::Core` 与 `StreamView::Rules`；
   - 在所有构建预设（`dev`、`ci`、`sanitize`）中作为常规自动化测试执行，全量测试用例数由 53 个增至 56 个。

---

## 影响评估

### 正向收益
- 在 Ubuntu、macOS、Windows 三平台上实现核心 DSL 组件在每次本地构建与 CI 推送中的全自动模糊回归验证；
- 在启用 `STREAMVIEW_ENABLE_LIBFUZZER` 时完全兼容 LLVM libFuzzer 高吞吐量变异探索；
- 确保全部种子语料在 AddressSanitizer 与 UndefinedBehaviorSanitizer 插桩下零告警、零泄漏；
- 保持 fuzz 测试逻辑与桌面 GUI 应用代码的清晰解耦。

### 代价
- CTest 测试总数增加 3 项，整体构建与测试耗时微增约 1-2 秒。

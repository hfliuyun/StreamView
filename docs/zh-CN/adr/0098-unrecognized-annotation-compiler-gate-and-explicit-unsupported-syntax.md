# ADR-0098：未识别注解编译闸门与显式不支持语法

- **状态**：Accepted
- **日期**：2026-08-17
- **作者**：StreamView 贡献者

---

## 背景与动机

在 DSL 编译器与运行时的格式覆盖扩展及安全审计过程中，确认了两项关键语言层能力需求：

1. **未识别注解静默通过缺陷（N2 漏洞）**：
   DSL 编译器此前在字段上仅校验已知注解（`@equals`, `@range`, `@enum`, `@spec`, `@description`, `@context_export`），对未识别或拼写错误的注解静默忽略。例如 `bits<12> syncword @equalss(4095);` 能够正常通过 `Rule OK` 编译，且对错误码流执行产出 `Materialized` 且 0 条诊断，使约束静默失效。
2. **显式非致命不支持能力上报需求**：
   StreamView 格式规则均在声明的有界 Profile 子集内工作（如 H.264 `baseline`/`main`/`high`、AAC `lc`）。当码流包含规范合法但在当前规则包中未予支持的特性时（例如 AAC 中的非 GA 音频对象类型 SBR/AOT 5、PS/AOT 29、转义 AOT >= 31，或 MP4 容器中未处理的扩展 Box），分析器必须：
   - 立即终止该不支持语法子树的后续解码；
   - 保留已成功物化的头部字段及其有效坐标；
   - 将节点发布为 `MaterializationState::Unsupported` 状态并附加非致命的 `DiagnosticCode::UnsupportedSyntax` 警告；
   - 保持核心引擎格式中立，禁止在 C++ 分析器中硬编码格式专属字符串合成（Gemini 规则 5.1）。

---

## 决策

### 1. 统一编译期注解注册表与校验闸门

在 [`src/rules/dsl.cpp`](file:///Users/yun/code/streamview/src/rules/dsl.cpp) 中建立查表驱动的静态注册表 `knownAnnotations` 与统一校验函数 `validateAnnotations`：

1. **当前激活的 11 个合法注解**：
   - `@spec("standard", "clause")`：标量字段、计算字段、lazy 区域、compressed payload、结构体、enum、sequence 扫描、payload 派发；
   - `@description("text")`：标量字段、计算字段、lazy 区域、compressed payload、结构体、enum、sequence 扫描、payload 派发；
   - `@equals(integer)`：标量字段（`bits`, `ue`）；
   - `@range(minimum, maximum)`：标量字段（`bits`, `ue`, `se`）；
   - `@enum(EnumType)`：标量字段（`bits`, `ue`）；
   - `@lazy(expression)`：专有字段引入引导符（`@lazy(expr) bytes name;`），非通用注解；
   - `@index(progressive)`：sequence 扫描声明（`sequence<T> s = scan(scanner);`）；
   - `@context("kind", key)`：结构体声明；
   - `@context_export`：标量字段与计算字段；
   - `@context_import("kind"[, key])`：结构体声明；
   - `@context_dependency("kind", key)`：结构体声明。
2. **预留注解（1 个）**：
   - `@target_format(...)`：预留给后续跨层格式委托（任务 P5h），当前注册 `allowedTargets = 0`（在所有宿主上均禁止使用）。
3. **编译期闸门拦截**：
   - 任何未注册注解名称直接报错 `DslDiagnosticCode::InvalidAnnotation`（`"Unknown annotation '@<name>'"`）；
   - 将注解放置于不支持的声明宿主上时报错 `DslDiagnosticCode::InvalidAnnotation` 并给出宿主专有白名单文案；
   - 纯函数（`pure`）与入口声明（`entry`）严格禁止携带任何注解。

### 2. 显式 `unsupported` 语法语句

在 DSL 文法中引入格式中立的声明语句：

```svfmt
unsupported("reason text") at anchor_field;
```

1. **解析与编译规则**：
   - 锚点字段必须是在当前分支早先声明的 source-backed 标量字段；
   - 禁止置于 `repeat` 循环体内部（`DslDiagnosticCode::InvalidCondition: "Unsupported statements cannot be repeat-local items"`）。循环展开过程中产生的重复诊断在 [`src/rules/dsl_ir.cpp:28-36`](file:///Users/yun/code/streamview/src/rules/dsl_ir.cpp#L28-L36) 的 `addDiagnostic` 中按 `(code, message, range)` 统一去重；
   - 单个结构体至多声明 1024 条 unsupported 语句。
2. **Bytecode 与 VM 执行**：
   - 降级为操作码 `DslOpcode::MarkUnsupported`；
   - 执行到该操作码时，VM 停止当前结构体的后续解码，将状态置为 `DslExecutionStatus::Unsupported`，节点状态置为 `core::MaterializationState::Unsupported`，并在锚点字段位置生成 `core::DiagnosticCode::UnsupportedSyntax`（Severity: Warning）诊断。
3. **AAC Profile 行为对齐**：
   - 在官方 `org.streamview.aac` 规则包（`profiles = ["lc"]`）中，非 GA AOT（AOT 5, 29）与转义 AOT 配置（`audio_object_type == 31`）在完成公共前缀解码后立即执行 `unsupported`。所有转义 AOT 取值（AOT 32, 34, 40, 41, 42 等）均统一判定为 Unsupported，不进行逐值分支细分解码。

---

## 影响与结论

### 正向收益
- 彻底消除未识别注解静默通过导致的约束失效风险；
- 规则包可原生在 DSL 内表达 Profile 边界与未支持扩展，不再依赖 C++ 分析器侵入式处理；
- 编译期循环展开诊断实现按位置去重。

### 负向代价 / 约束
- 未来新增注解必须在中央 `knownAnnotations` 注册表中显式登记其合法宿主集合。

---

## 被否决方案

1. **在解析器各分支散落 if 校验**：
   因容易出现遗漏、不同宿主行为漂移以及维护负担过重而被否决。
2. **对不支持 Profile 报致命 `InvalidSyntax`**：
   将不支持 Profile 报致命错误会导致正常媒体文件无法展示任何已解析元数据，与非致命分析器设计目标冲突，故否决。
3. **在 C++ 分析器中硬编码诊断字符串合成**：
   违反 Gemini 规则 5.1（格式语义必须留在 DSL/规则层），故否决。

---

## 验证矩阵与证据

| 探针 / 测试用例 | 执行命令 / 测试符号 | 预期输出 / 断言结果 | 验证结论 |
| :--- | :--- | :--- | :--- |
| **未识别注解闸门（N2）** | `scratch/probe_annotation_gate` | `diag code=14 msg="Unknown annotation '@equalss'"` | 证实拼写错误注解被编译期拦截。 |
| **宿主白名单校验** | `tests/rules/dsl_test.cpp:2171`（`rejectsUnrecognizedAnnotationsAndEnforcesHostWhitelist`） | 5 大宿主位置 `@bogus(1)` 均返回 `InvalidAnnotation` | 证实前置/后置字段、struct、enum、sequence 均受拦截。 |
| **字段错误恢复守卫** | `tests/rules/dsl_test.cpp:2242`（`recoversFieldSyntaxErrorWithoutDroppingClosingBrace`） | 仅 1 条诊断（`MissingToken: Expected field name`） | 证实结构体闭合花括号不被误吞。 |
| **循环 Unsupported 去重** | `tests/rules/dsl_ir_test.cpp:3562`（`deduplicatesUnsupportedDiagnosticsInsideRepeats`） | 仅 1 条诊断（`Unsupported statements cannot be repeat-local items`） | 证实循环展开诊断已正确去重。 |
| **AAC 非 GA AOT Unsupported** | `tests/rules/aac_adts_analyzer_test.cpp:1960-2467` | `MaterializationState::Unsupported`, `UnsupportedSyntax` | 证实 AOT 5、29、39 与转义 AOT 31 均输出 Unsupported。 |
| **官方规则包校验** | `svtool rule check` 检查 H.264 与 AAC 规则包 | 所有 `.svfmt` 文件均输出 `Rule OK` | 证实既有官方规则包零回归。 |

---

## 参考资料

- [ADR-0040：非致命语法警告与值域注解](0040-non-fatal-syntax-warnings-and-range-annotations.md)
- [ADR-0094：音频特定配置（ASC）与声道配置元素（PCE）解析](0094-audio-specific-config-and-program-config-element.md)
- [ADR-0095：AAC 原始数据块压缩载荷与 Profile 处理边界](0095-aac-raw-data-block-compressed-payload-and-profile-handling.md)
- [ADR-0096：MP4 ISOBMFF 容器架构、Box 遍历与跨层导航引用模型](0096-mp4-isobmff-container-architecture-box-traversal-and-cross-layer-navigation.md)

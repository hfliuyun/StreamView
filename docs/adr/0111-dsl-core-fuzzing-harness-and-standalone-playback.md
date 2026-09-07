# ADR-0111: DSL Core Fuzzing Harness, Standalone Playback, and Invariant Verification

- **Status**: Proposed
- **Date**: 2026-09-07
- **Authors**: StreamView Contributors

---

## Context

StreamView relies on its custom declarative Domain-Specific Language (DSL) to specify and decode complex media container and bitstream formats (ISO BMFF MP4, H.264 Annex B, AAC ADTS/ASC). The DSL core engine comprises three distinct stages:
1. **Lexer and Parser (`DslLexer`, `DslParser`)**: Ingests raw `.svfmt` source text and produces a syntactic abstract syntax tree (`DslProgram`) or syntax diagnostics (`DslDiagnostic`);
2. **Compiler and IR Lowering (`DslCompiler`)**: Ingests `DslProgram`, performs symbol resolution, type checking, field validation, and dependency analysis, and lowers valid constructs into typed programs with bytecode (`DslTypedProgram`);
3. **Virtual Machine and Executor (`DslVirtualMachine`, `DslExecutor`)**: Interprets bytecode instructions against untrusted bitstreams using `core::BitReader`, enforcing memory bounds, instruction budgets, and loop limits.

Because untrusted media files, external user-provided `.svrule` packages, and arbitrary input streams directly interact with these engine boundaries, any vulnerability (e.g., stack overflow, heap out-of-bounds reads, infinite loops, or unhandled exceptions) represents a critical denial-of-service or security risk.

Per ADR-0110, Phase 7 requires establishing a robust, continuous fuzzing harness across these core trust boundaries. Furthermore, StreamView's multi-platform continuous integration environment spans Ubuntu 24.04, macOS 15, and Windows 2022. While LLVM's `libFuzzer` engine is optimal for exploratory fuzzing campaigns on Clang-based Linux/macOS systems, MSVC on Windows and standard developer presets do not link `libFuzzer` runtime libraries by default. Therefore, a dual-mode fuzzing architecture is required to guarantee that all fuzz targets can also run deterministically under standard CTest across all target platforms.

---

## Decisions

### 1. Dual-Mode Architecture: libFuzzer Engine vs. Standalone Deterministic Playback

**Decision**: Implement a unified fuzz target structure that conditionally compiles either as an LLVM `libFuzzer` target or as a standalone deterministic test runner based on the CMake configuration option `STREAMVIEW_ENABLE_LIBFUZZER` (default: `OFF`).

1. **Standard `libFuzzer` Interface**:
   Every fuzz target defines the canonical libFuzzer entry point:
   ```cpp
   extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
   ```
2. **Reusable Standalone Driver (`standalone_fuzz_driver.h`)**:
   When `STREAMVIEW_ENABLE_LIBFUZZER` is disabled:
   - The driver provides `main(int argc, char** argv)`.
   - If executed with file paths or directory paths as command-line arguments, it iterates over all specified files and passes their byte contents to `LLVMFuzzerTestOneInput`.
   - If executed without arguments, it defaults to running all files located in the target's designated seed corpus directory (configured via compile definition `STREAMVIEW_FUZZ_CORPUS_DIR`), or falls back to an embedded synthetic seed battery if no corpus directory is present.
   - Any crash, memory leak, or uncaught exception triggers test failure under CTest. Under `cmake --preset sanitize`, AddressSanitizer and UndefinedBehaviorSanitizer continuously instrument every execution.

---

### 2. DSL Parser Fuzz Target (`fuzz_dsl_parser`)

**Decision**: Implement `fuzz_dsl_parser` targeting `streamview::rules::DslParser::parse(const QString& source)`.

- **Input Ingestion**: Ingests arbitrary raw byte buffers, safely converting them to `QString` using `QString::fromUtf8` (lossy decoding, ensuring arbitrary byte sequences never crash the conversion layer).
- **Threat Model & Invariants**:
  1. **Zero Crash Guarantee**: Arbitrary character sequences, control characters, unterminated strings, unterminated block comments, and malformed numeric literals must never crash or cause panics;
  2. **Bounded Recursion**: Deeply nested parentheses, braces, and operator chains must be gracefully detected and rejected via syntax diagnostics without exhausting the stack;
  3. **Deterministic Output**: Parsing the same input twice must yield identical diagnostics and AST state.

---

### 3. DSL Compiler Fuzz Target (`fuzz_dsl_compiler`)

**Decision**: Implement `fuzz_dsl_compiler` targeting `streamview::rules::DslCompiler::compile` and `compileForTarget`.

- **Input Ingestion**: Parses the input bytes via `DslParser::parse`. The resulting `DslProgram`—whether fully valid, partially parsed, or diagnostic-bearing—is unconditionally submitted to `DslCompiler::compile(parseResult.program)` as well as target-specific compilation queries.
- **Threat Model & Invariants**:
  1. **Semantic Safety**: Circular struct references, duplicate names, invalid bit widths, unsupported annotations, and type mismatches must be cleanly reported through `DslCompileResult::diagnostics`;
  2. **Zero Aborts/Panics**: The compiler must never assert, throw unhandled exceptions, or dereference null optional structures, regardless of AST malformation;
  3. **Bytecode Integrity**: When compilation succeeds, the emitted bytecode and instruction operands must strictly adhere to valid opcode ranges.

---

### 4. DSL Virtual Machine and Executor Fuzz Target (`fuzz_dsl_vm`)

**Decision**: Implement `fuzz_dsl_vm` targeting `streamview::rules::DslVirtualMachine::execute` and `streamview::rules::DslExecutor::decodeStruct`.

- **Input Ingestion**:
  - The input byte stream represents untrusted bitstream payloads.
  - The target executes against a comprehensive multi-feature test schema comprising all project DSL constructs:
    - Fixed and dynamic width bitfields (`bits<N>`, `endian = little/big`);
    - Exponential-Golomb integer encodings (`unsigned_exp_golomb`, `signed_exp_golomb`);
    - Byte-aligned sequence encodings (`ff_coded`);
    - Computed fields and arithmetic/logical expressions;
    - Conditional branches (`if/else`), `switch` dispatch, and multi-arm cases;
    - Bounded loops: `repeat(N)`, sentinel loops (`until sentinel`), and `while(condition)` repeats;
    - Lazy byte regions and compressed payload leaves;
    - Source assertions and unsupported markers.
  - The target also executes against compiled official rule packages (H.264, AAC, MP4) using synthetic slices.
- **Threat Model & Invariants**:
  1. **Instruction Budget Enforced**: Execution must terminate with `DslExecutionStatus::ResourceLimit` whenever the instruction budget (`maximumInstructions`) is exceeded, preventing CPU exhaustion;
  2. **Loop Iteration Bound**: While-loops and sentinel repeats must enforce compile-time and runtime upper iteration bounds (`maximumWhileRepeatIterations = 1024`, `maximumSentinelRepeatIterations = 64`);
  3. **Zero Out-of-Bounds Memory Reads**: `core::BitReader` bounds checking must prevent any read past the source buffer; truncated streams must cleanly report `DslExecutionStatus::TruncatedSource` or `InvalidSyntax`;
  4. **Resource Bounds**: Node materialization depth and node count limits must be strictly respected.

---

### 5. Seed Corpus Organization and CMake/CTest Integration

**Decision**: Structure seed corpora in `tests/fixtures/fuzz/` and wire fuzz executables directly into CTest.

1. **Directory Layout**:
   - `tests/fuzz/`: Fuzz harness source files (`standalone_fuzz_driver.h`, `fuzz_dsl_parser.cpp`, `fuzz_dsl_compiler.cpp`, `fuzz_dsl_vm.cpp`);
   - `tests/fixtures/fuzz/dsl_parser/`: Seed corpus for parser (valid format schemas, malformed schemas, boundary cases);
   - `tests/fixtures/fuzz/dsl_compiler/`: Seed corpus for compiler (edge-case type configurations, recursive structs, multi-entry rules);
   - `tests/fixtures/fuzz/dsl_vm/`: Seed corpus for VM (empty streams, truncated headers, random byte sequences, pathological bit patterns).
2. **CMake Build Integration**:
   - Three executable targets: `streamview_fuzz_dsl_parser`, `streamview_fuzz_dsl_compiler`, `streamview_fuzz_dsl_vm`;
   - Linked against `StreamView::Core` and `StreamView::Rules`;
   - Registered as automated CTest test cases under all build presets (`dev`, `ci`, `sanitize`). Total test count increases from 53 to 56.

---

## Consequences

### Positive
- Continuous, automated fuzz testing of core DSL components on every local build and CI push across Ubuntu, macOS, and Windows.
- Full compatibility with LLVM `libFuzzer` for high-throughput mutation fuzzing when enabled.
- Guaranteed zero regressions under AddressSanitizer and UndefinedBehaviorSanitizer on all seed corpus inputs.
- Clear separation between fuzz targets and application GUI logic.

### Negative
- CTest test count expands by 3 tests; additional build time for fuzz targets (~1-2 seconds).

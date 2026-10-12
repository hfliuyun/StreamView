# P7d-2：Clang-Tidy 静态检查任务书

状态：2026-10-13，本地实现与完整 LLVM 18.1.8 分析已通过，314/314 构建步骤完成。
P7d-1 的跨平台告警修复已提交为 `076e6f2`；Hosted CI 仍需在推送后确认。
普通 dev/ci 构建不依赖本机安装 Clang-Tidy。

## 1. 合同

- 工具：LLVM Clang-Tidy **18.x**；启用时必须能查询到 18.x 版本，缺失工具或版本
  不符时 CMake 配置直接失败。
- 检查集：仓库根目录 `.clang-tidy` 冻结为高信号集合：
  `bugprone-empty-catch`、`bugprone-use-after-move`、
  `bugprone-unsafe-functions`、`clang-analyzer-core.*`、
  `clang-analyzer-cplusplus.*`、`performance-move-const-arg`、
  `readability-avoid-const-params-in-decls` 和
  `readability-identifier-naming`。其余整族检查不作为本门禁的默认阻断项，
  仍可由开发者通过临时配置单独启用调查。
  测试目录通过 `tests/.clang-tidy` 仅排除
  `clang-analyzer-core.CallAndMessage`：LLVM 18 不理解 QTest::qVerify 在断言
  失败时返回 false 并终止测试，因此在已经有 QVERIFY 的路径产生空指针/未初始化
  误报。产品目录继续启用此检查，测试的其他检查和编译告警门禁均保留。
- 范围：项目自身 `src/`、`tools/`、`tests/` 和启用时的 `benchmarks/` 编译单元；`.clang-tidy` 的
  `HeaderFilterRegex` 排除 Qt 与其他外部头文件。生成的 Qt 资源目标不单独纳入。
- 失败策略：启用检查集产生任一诊断即失败（通过
  `--warnings-as-errors=*`）；不使用全局告警屏蔽或将诊断降级为提示。
- 普通 `dev`、`ci`、`sanitize` preset 默认关闭 Clang-Tidy；专用 `clang-tidy`
  preset 显式开启。该 preset 关闭 C++ module dependency scanning（项目不使用
  C++ modules），使 compile database 不依赖尚未生成的 module 响应文件。

## 2. 实现

`cmake/StreamViewClangTidy.cmake` 在启用时查找并验证 Clang-Tidy 18.x，
再由 `streamview_set_warnings(target)` 将 `CXX_CLANG_TIDY` 只应用到项目自有
target。`CMakePresets.json` 新增 `clang-tidy` 配置和构建 preset；未安装工具的
机器保持普通构建可用，但运行该 preset 会以明确错误停止。Qt 自动生成的内部
resource target 不通过该 helper，因此不会被这项门禁隐式纳入。

## 3. 验证

具备 LLVM 18 工具链后运行：

```sh
cmake --preset clang-tidy \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@18/bin/clang++ \
  -DSTREAMVIEW_CLANG_TIDY_EXECUTABLE=/opt/homebrew/opt/llvm@18/bin/clang-tidy \
  -DSTREAMVIEW_BUILD_BENCHMARKS=ON
cmake --build --preset clang-tidy -j2
```

同时记录 `clang-tidy --version`、完整构建输出和 `git diff --check`。本机若没有
Clang-Tidy，不伪造静态分析通过结论；应记录配置失败原因为工具缺失，并在 Hosted
或专用工具环境中完成实证。

2026-10-13 本地完整输出保留在 `build/clang-tidy/closeout-full-analysis.log`。
CI 集成在单独提交中增加 LLVM 18 job；P7d-3 使用独立任务书，不混入该切片。

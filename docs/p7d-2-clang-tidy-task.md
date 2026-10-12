# P7d-2：Clang-Tidy 静态检查任务书

状态：2026-10-12，已开始。P7d-1 的跨平台告警修复已提交为 `076e6f2`；
对应 Hosted CI 仍需在推送后确认。本任务先冻结 Clang-Tidy 合同并接入可选的
CMake 静态分析 preset，不把普通 dev/ci 构建变成依赖本机工具安装的流程。

## 1. 合同

- 工具：LLVM Clang-Tidy **18.x**；启用时必须能查询到 18.x 版本，缺失工具或版本
  不符时 CMake 配置直接失败。
- 检查集：沿用仓库根目录 `.clang-tidy` 的现有集合：
  `bugprone-*`、`clang-analyzer-*`、`performance-*`、`portability-*`、
  `readability-avoid-const-params-in-decls` 和 `readability-identifier-naming`。
- 范围：项目自身 `src/`、`tools/`、`tests/` 编译单元；`.clang-tidy` 的
  `HeaderFilterRegex` 排除 Qt 与其他外部头文件。生成的 Qt 资源目标不单独纳入。
- 失败策略：启用检查集产生任一诊断即失败（通过
  `--warnings-as-errors=*`）；不使用全局告警屏蔽或将诊断降级为提示。
- 普通 `dev`、`ci`、`sanitize` preset 默认关闭 Clang-Tidy；专用 `clang-tidy`
  preset 显式开启。

## 2. 实现

`cmake/StreamViewClangTidy.cmake` 在启用时查找并验证 Clang-Tidy 18.x，
再由 `streamview_set_warnings(target)` 将 `CXX_CLANG_TIDY` 只应用到项目自有
target。`CMakePresets.json` 新增 `clang-tidy` 配置和构建 preset；未安装工具的
机器保持普通构建可用，但运行该 preset 会以明确错误停止。Qt 自动生成的内部
resource target 不通过该 helper，因此不会被这项门禁隐式纳入。

## 3. 验证

具备 LLVM 18 工具链后运行：

```sh
cmake --preset clang-tidy
cmake --build --preset clang-tidy -j2
```

同时记录 `clang-tidy --version`、完整构建输出和 `git diff --check`。本机若没有
Clang-Tidy，不伪造静态分析通过结论；应记录配置失败原因为工具缺失，并在 Hosted
或专用工具环境中完成实证。

本任务不修改 Hosted CI 的普通三平台构建、不上传 artifact、不开始 P7d-3。

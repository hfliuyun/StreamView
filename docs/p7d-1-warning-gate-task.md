# P7d-1：项目编译告警门禁任务书

状态：2026-10-12，待派发。UI 整改已提交为 `f42f54e`，阶段文档与本任务书已提交为
`35bf605`，但尚无覆盖整改提交的新 ci/sanitize 与三平台 Hosted CI。
主代理完成该交付门禁并在本文固定通过验证的代码 SHA 后，本文才成为可执行任务。
当前 HEAD `35bf605` 是已提交的文档后代；P7d-1 的干净实现基线仍待整改交付门禁通过后固定。

本任务只完成 P7d 的第一个切片：让项目编译告警导致构建失败。
主代理负责方案、最终复核、提交和跨平台交付；快速模型负责以下有界实现和本地验证。

## 1. 开工门禁

先完整读取：

- `docs/implementation-plan.md` 顶部状态区与 P7d 切片说明；
- 双语 ADR-0110 的 `2026-10-12` 修订，以英文为准；
- 本文。

记录以下原始输出：

```sh
git status --short --branch
git rev-parse HEAD
git rev-parse origin/main
git log -6 --oneline
```

只有本文顶部已由主代理固定完整代码 SHA、权威状态明确整改交付通过、HEAD 匹配且
受控源码没有其他进行中差异时才能实施。否则报告现场差异并停止。
旧对话的本地测试通过或历史 Hosted run 不替代该基线门禁。

`.omo/`、`scratch/`、`pauches_6.md` 为既有资产，保留，不读取、修改、删除或提交。
你不是独占代码库，遇到新增他人差异先判断是否影响本任务，保留原内容；
无法兼容时停止并报告证据。单个实现进程完成本任务，不派生子代理。

## 2. 目标与改动范围

新增 `STREAMVIEW_WARNINGS_AS_ERRORS` CMake cache option，默认 ON；
通过 `streamview_set_warnings(target)` 设置 target 的
`COMPILE_WARNING_AS_ERROR` 属性。dev、ci 和 sanitize 的配置均保持 ON。
显式本地 OFF 配置用于诊断，不进入交付验证或 Hosted CI。

首先亲自完整读取即将修改的代码：

- `CMakeLists.txt`；
- `CMakePresets.json`；
- `cmake/StreamViewWarnings.cmake`。

检查各 target 调用 helper 的位置，覆盖 core、rules、app、可执行程序、已有测试和
六个 standalone fuzz targets。保留现有告警与 sanitizer 选项以及 PRIVATE 作用域。
Qt imported targets、Qt 内部独立 resource targets 和 SYSTEM 第三方头文件不纳入
新增的全局编译策略；项目 target 内的 AUTOMOC 编译遵循其所属 target。

主要改动范围为上述三个构建文件。若实际项目构建出现告警，只允许在诊断指向的
项目源码或测试中做保持行为的最小修复，并在报告逐项列出诊断、修改和验证。
如果没有需要改变的 preset 配置，不为凑齐文件清单而修改它。

本片不改 `.github/workflows/`，不接入 Clang-Tidy，不新增依赖，不改格式规则、
公共 API、session/cache schema 或解析行为。若告警必须通过行为或合同变化才能修复，
停在具体诊断与 `file:line`，交由主代理决定。

## 3. 先证明告警门禁有效

在临时目录建立最小 CMake 工程，包含仓库里的真实
`cmake/StreamViewWarnings.cmake`，创建可执行 target 并调用 helper。
探针源码使用一个会触发现有告警级别的未使用局部变量：

```cpp
int main() {
    int unused;
    return 0;
}
```

按顺序记录命令、编译器版本、退出码和关键诊断：

1. 修改前设置 `STREAMVIEW_WARNINGS_AS_ERRORS=ON`，探针产生告警但构建成功。
   这证明旧 helper 未满足新门禁，不宣称旧构建本来应该失败。
2. 实现门禁后，保持同一探针与 ON，构建必须因该告警返回非零。
   这是故意触发的门禁通过证据。
3. 仅移除未使用局部变量，保持 ON，探针构建成功。
4. 在独立构建目录保留告警源码、显式 OFF，构建成功且仍报告告警；
   切回 ON 后再次确认构建失败，排除 cache 或增量构建造成的假通过。

若编译器没有报出预期的 unused-variable 诊断，先报告实际诊断并调整临时探针，
不得把成功编译误写为告警已被拦截。探针留在临时目录，不污染生产 target，
不新增仅断言 flag 字符串的 CTest。

使用 CMake 原生 target property，不设置全局 `CMAKE_CXX_FLAGS` 或
`CMAKE_COMPILE_WARNING_AS_ERROR` 初始化变量，不通过
`--compile-no-warning-as-error`、全局告警屏蔽或关闭测试制造门禁通过。
对确需局部修正的告警给出语义依据；转换边界保持 checked arithmetic 和既有输入验证。

## 4. 验证真实项目

先完整重新配置，再构建，测试总数使用本次 CTest discovery：

```sh
cmake --preset dev
cmake --build --preset dev -j2
ctest --preset dev --parallel 2 --output-on-failure
```

完成 dev 后，串行运行 ci、sanitize：

```sh
cmake --preset ci
cmake --build --preset ci -j2
ctest --preset ci --parallel 2 --output-on-failure
cmake --preset sanitize
cmake --build --preset sanitize -j2
ctest --preset sanitize --parallel 2 --output-on-failure
```

逐一确认三套 cache 中 `STREAMVIEW_WARNINGS_AS_ERRORS=ON`，记录实际编译命令
和编译器版本。全量构建成功、CTest 通过与故意失败探针共同构成门禁证据。
按指导手册对官方 `.svfmt` 执行 `svtool rule check`，全部通过后运行：

```sh
git diff --check
git diff --stat
git status --short --branch
```

本机无法证明其他平台的 GCC/MSVC 结果；报告如实列为主代理后续 Hosted CI 项，
不安装额外工具链、不伪造三平台结论。

## 5. 交付与停止条件

报告包含：

- 开工 SHA、实际 HEAD 和工作树原始状态；
- 修改文件及每项告警修复的理由；
- 四步探针的命令、退出码与关键诊断；
- dev/ci/sanitize 实际完整结果、官方规则检查与 diff check；
- `Review Gate: Conditional` 自检结论，或升级为 Required 的触发原因；
- 未执行事项、遗留诊断和需要主代理裁定的问题。

沿用指导手册的自动升级条件，尤其注意超出声明范围、公共合同/行为变化及各平台
结果不一致。保留未提交差异，主代理复核后负责 commit、push、对应提交的三平台 CI
及计划回写。没有本任务的额外 Git 交付授权。

完成后停止：不修改 implementation-plan 或 ADR 来自签验收，不开始 P7d-2/3、
P7e 或发布流程，也不把 P7d 总任务标为完成。

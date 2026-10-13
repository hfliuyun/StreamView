# StreamView 格式扩展开发契约

状态：已接受的项目契约。英文版本是规范性来源，本文件是中文伴随说明。凡是
新增或扩展容器、编解码器、基本流或媒体子格式，代理在修改前必须完整阅读本契约。

## 1. 适用范围

以下任何变化都属于格式扩展任务：

- 新增基本流或编解码格式；
- 新增容器或文件家族；
- 新增检测器、扫描器、入口点或跨层 payload 视图；
- 新增官方规则包或规则包入口点；
- 改变已有格式的语法、诊断、字段、分帧、导航或声明覆盖范围。

只有实现、规范、测试、验证证据和实施计划记录全部齐全，任务才算完成。

## 2. 固定执行顺序

后一步不能替代前一步。

### 第一步：分类格式

先写清目标是：

- 基本流还是容器；
- 长度分隔、哨兵分隔，还是 chunk/box 分隔；
- 编解码器、传输格式，还是两者兼有；
- 独立格式入口，还是由已有容器选择的跨层 payload。

以 WAV 为例，必须在 ADR 中明确 RIFF/WAVE 容器范围、支持的 chunk 类型、
对齐规则、支持的编码族，以及 RF64 或压缩 WAVE 等延期项。不能只根据文件扩展名
推断语义。

### 第二步：先写规范，再写代码

修改 C++ 或 `.svfmt` 之前，先创建连续编号的双语 ADR：

```text
docs/adr/NNNN-<format-or-decision>.md
docs/zh-CN/adr/NNNN-<format-or-decision>.md
```

ADR 必须写明：

- 规范名称、版本/版次和条款引用；
- package ID、entry-point ID、format 标识符和初始包版本；
- 检测与仲裁证据，包括误报边界；
- 分帧规则和 source-span 规则；
- 支持子集与明确延期项；
- 畸形、截断、不支持、取消和资源耗尽语义；
- 最大读取量、节点量、递归/循环上限和大文件行为；
- 是否需要 scanner/detector/analyzer，还是现有 DSL 已足够；
- 兼容性和规则包版本后果；
- 用于证明决策的确切测试与 fixture。

如果当前 DSL 或运行时无法表达目标语义，完成探测后立即停下，保留原始错误输出；
不得悄悄扩大核心合同。

### 第三步：加入格式规范

除非 ADR 明确证明这是格式无关能力，否则使用官方规则包。新包通常包含：

```text
src/rules/official/<package-id>/rule.toml
src/rules/official/<package-id>/src/*.svfmt
src/rules/official/<package-id>/docs/en/*.md
src/rules/official/<package-id>/docs/zh-CN/*.md
src/rules/official/<package-id>/tests/*
```

在 `src/rules/CMakeLists.txt` 和相关 Qt resource 清单中注册规则包。manifest、
entry point、detector 名称、文档路径和包版本必须与 ADR、测试一致。

如果任务改变公共 DSL 合同，必须同步更新：

```text
docs/format-language/README.md
docs/zh-CN/format-language/README.md
```

语言参考必须覆盖语法与静态规则、运行时和 source 坐标、诊断与恢复、资源限制与
取消、兼容性/弃用规则，以及合法/非法示例。只有 C++ 行为或示例不构成公共语言合同。

### 第四步：只有需要时才增加检测或集成

把行为放到真正拥有它的最小层：

- `.svfmt` 和官方规则包拥有格式语法与字段语义；
- `src/rules/` 在 DSL 无法发现边界时拥有格式专属检测、分帧、扫描和 analyzer 编排；
- `src/app/` 只拥有格式中立的选择、呈现和用户界面标签，不解析格式字节；
- `src/core/` 只拥有 source、坐标、有界执行、取消、诊断和格式中立模型；
- `src/tools/` 拥有 `svtool` 命令与探测工具。

发现能力不足时，先在 scratch 副本中运行 `svtool rule check`。核心、DSL 或公共 API
变更必须有单独 ADR 范围，不能藏在格式消费者提交中。

### 第五步：用 fixture 和测试建立证据

fixture 必须由提交到仓库的脚本或确定性 builder 生成；禁止手工计算 bit 偏移，
也不能只凭图示声称覆盖。至少覆盖：

- 一个最小合法样例；
- 每个声明支持 profile 的代表性合法样例；
- 截断 header 和截断 payload/chunk；
- 非法长度、偏移、对齐或嵌套；
- 未知或可跳过的 chunk/box；
- 可识别但明确不支持的变体；
- 检测器误报与候选歧义；
- 触发资源上限的大文件或稀疏源；
- 如果承诺恢复，则加入“畸形单元之后仍有合法单元”的样例。

ADR 和对应测试必须逐一写出作为证据的 fixture 名称。新行为触及哪一层，就补哪一层
的规则检查、analyzer 测试、应用测试和必要的 fuzz 覆盖。

## 3. 版本与身份规则

- 新格式使用新的稳定 package ID；不能复用已有包或 format 标识符。
- 任何解码输出变化，或 `rule.toml` 任意字段（包括 `depth`）变化，都会改变包内容，
  必须升级包版本。
- session 和诊断合同必须保留完整的 package ID、版本、内容 hash 和 entry-point 身份。
- 请求身份缺失或冲突时，不能静默回退到其他已安装版本。
- 格式语义放在规则和格式专属 runner；不能把 FourCC、magic value、chunk taxonomy
  或编解码器分支写进格式中立的 core。

## 4. 允许与禁止的修改

### 允许

- 新建或修订官方规则包、manifest、DSL、双语包文档和包测试；
- 在 `src/rules/` 中增加格式专属 detector、scanner 或 analyzer；
- 为暴露新入口增加必要的格式中立应用 wiring；
- 在单独能力 ADR 和探测证据支持下修改 DSL/core 能力；
- 增加确定性 fixture 生成器、fuzz target、CMake/resource 注册和回归测试；
- 增加双语 ADR、格式语言参考和实施计划记录；
- 增加验证新格式但不削弱既有门禁的 CI 步骤。

### 禁止

- 在 `src/core/` 或 Qt widget 中解析格式字节；
- 向格式中立 core API 加入格式名、FourCC、magic number 或容器分类；
- 在规则包中加入原生可执行代码、脚本、网络/进程访问、动态依赖或符号链接；
- 没有 ADR、包版本升级、回归 fixture 和迁移说明就改变已有格式行为；
- 用仅路径、仅 package ID 或仅版本的查找替代完整规则身份；
- 让 FFmpeg 或其他外部工具成为运行时依赖；
- 删除或放宽既有测试、警告门禁、sanitizer 门禁、资源上限、取消语义或 hosted 矩阵；
- 因已有中文说明而跳过英文规范；
- ADR、伴随文档、fixture、测试、验证结果或计划记录任一缺失时标记任务完成。

## 5. 必须完成的验证

根据修改层次运行并记录真实输出：

```bash
svtool rule check <每个新增或修改的规则>
svtool analyze <代表性 fixture> ...
ctest --preset dev -R "<定向测试>" --output-on-failure
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --preset ci
cmake --build --preset ci
ctest --preset ci --output-on-failure
cmake --preset sanitize
cmake --build --preset sanitize
ctest --preset sanitize --output-on-failure
git diff --check
```

还必须运行 fixture 生成器和相关 fuzz target 或确定性 fuzz 回放。受影响的平台必须
通过 Hosted CI。将 run ID、job ID、测试数量、sanitizer 状态及非阻塞平台提示写回
`docs/implementation-plan.md`。

## 6. 提交与评审形状

沿用现有主干顺序：

1. `docs(<scope>): specify ...`：双语 ADR 与规范文档；
2. `feat(<scope>): ...`：实现与测试一起提交；
3. `docs(<scope>): record ...`：验证证据和计划更新。

改变 DSL/core 合同、跨层导航、session/schema 身份、资源/取消语义或固定审计门禁时，
必须独立评审。评审是门禁，不替代测试。

## 7. 可直接复制给 Agent 的提示词

分派新格式任务时使用：

> 按 `docs/format-extension-contract.md` 为 `<FORMAT>` 增加支持，并把该契约作为完成
> 门禁。先检查当前实施计划、已有规则包、ADR 编号和真实源码/测试边界。写代码前，
> 先新增下一连续编号的双语 ADR 以及中英文格式文档，说明规范依据、package/entry-point
> 身份、检测/分帧模型、支持子集、延期项、畸形/截断语义、资源限制和完整 fixture/测试矩阵。
> 如果当前 DSL 或运行时无法表达目标语义，先做 scratch `svtool rule check` 探测并报告
> 原始错误及 `file:line`，不得隐式修改 core 或 DSL。
>
> 格式语义必须留在官方规则包和 `src/rules/`。只有格式中立集成与标签可以改
> `src/app/`；只有在单独能力 ADR 下才可改 `src/core/`。加入确定性 fixture、定向测试、
> 必要的 fuzz 覆盖、CMake/resource 注册和包版本升级。完成 dev/ci/sanitize 全矩阵、
> Hosted CI、`git diff --check`，并把真实证据写回 `docs/implementation-plan.md`。
> ADR、双语文档、测试、验证记录和计划条目任一缺失，任务都未完成。

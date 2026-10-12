# StreamView

StreamView 是一个严格只读的跨平台桌面分析器，用于从媒体容器逐层查看到规范定义的编解码 bit 字段。首个里程碑面向 Windows、macOS 和 Linux，支持 H.264 Annex B、AAC ADTS 与普通非分片 MP4/MOV。

- [中文产品需求](docs/zh-CN/product-requirements.md)
- [实施计划与进度](docs/implementation-plan.md)
- [中文格式语言草案](docs/zh-CN/format-language/README.md)
- [英文说明](README.md)

## 开发状态

项目正在实施中。当前阶段、验证证据、阻塞项和下一项可恢复动作统一记录在[实施计划](docs/implementation-plan.md)中。

## 构建要求

- CMake 3.28 或更高版本
- Ninja
- 支持 C++20 的编译器
- Qt 6.11.x，包含 Core、Gui、Widgets、Concurrent、Sql 和 Test

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

## 内部 CLI

开发构建包含 `svtool`，用于规则校验和当前的 Annex B 分析纵切面：

```sh
svtool --version
svtool rule check <rule.svfmt>
svtool analyze <source>
```

退出码 `0` 表示成功，`1` 表示规则或分析产生了诊断/部分结果，`2` 表示用法无效或无法
打开输入。该 CLI 是内部开发工具，不是计划中延期的正式公共 CLI 契约。

## 发布包

与 CMake 项目版本一致的正式版本 tag 会构建 Windows x64 ZIP、macOS ARM64
`.app.zip` 和 Linux x86_64 AppImage，并上传到 GitHub Release。Linux 最低要求
glibc 2.35（Ubuntu 22.04）。包内包含许可证材料及部署 SBOM；Release 另附
平台 SPDX 清单和归档校验码。签名与 macOS notarization 仍后置，不要求
alpha/beta/rc 前置流程。

实现合同及当前验证状态见[打包任务书](docs/p7f-h-release-task.md)和实施计划。

StreamView 自有代码采用 [MIT License](LICENSE)，Qt 及其他依赖保留各自许可证。

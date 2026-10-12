# Windows Qt CI 构建产物临时回退

状态：已接受
日期：2026-07-19

## 背景

项目产品与开发基线为 Qt 6.11.x，CI 最初固定使用 Qt 6.11.1。但 Qt 官方
在线仓库目前对 `aqtinstall` 使用的 Windows 元数据路径返回 HTTP 404：

`windows_x86/desktop/qt6_6111/qt6_6111/Updates.xml`

同一版本的 Linux 元数据可用；Windows 仓库则提供带所需 MSVC 2022 64 位
目标的 Qt 6.10.1。这是上游构建产物发布缺口，不是源码或编译器错误。

## 决策

- 产品与开发基线继续使用 Qt 6.11.x。
- Ubuntu 和 macOS CI 继续使用 Qt 6.11.1。
- 在官方 Windows Qt 6.11.1 元数据和安装包发布前，Windows CI 仅使用 Qt 6.10.1。
- CMake 默认最低 Qt 版本仍为 6.11；仅 Windows CI 可为此 fallback 显式设置
  `STREAMVIEW_MINIMUM_QT_VERSION=6.10`。
- Windows fallback 必须在 workflow 矩阵中显式声明，不能用于发布包。
- 当以下公开元数据路径成功返回并包含 `win64_msvc2022_64` 后，立即把 Windows
  矩阵恢复到 6.11.1：
  `https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6111/qt6_6111/Updates.xml`

## 影响

阶段 0 的 hosted build gate 可以立即在三个平台运行，但 Windows job 暂时没有
验证首选的精确 Qt patch 版本。在发布包被视为统一的 Qt 6.11.1 矩阵前，必须
移除这个 fallback；发布构建不得降低 `STREAMVIEW_MINIMUM_QT_VERSION`。

## 2026-10-13 恢复基线

MSVC 2022 x64 官方仓库已位于
`windows_x86/desktop/qt6_6111/qt6_6111_msvc2022_64/`。
旧的不含架构名的 `qt6_6111/Updates.xml` 子目录仍返回 404；仓库布局已经变化，
应在新的架构专属目录确认可用性。

已发布的 aqtinstall 3.3.0 不支持该 Windows 布局。普通 CI 与发布打包固定使用上游
aqtinstall 提交 `076e1659807d0b362a3ed684d54c2e9c775eb9c7`、Python 3.12 和
py7zr 1.1.4。本地 Python 3.11 对这个确切安装器进行 dry-run，成功解析 Qt 6.11.1
`win64_msvc2022_64` 的 qtbase、qtsvg、qttools 及其他桌面归档。
这证明仓库解析成功，不代替 Windows 构建证据。

普通矩阵恢复 Qt 6.11.1 并删除最低版本覆盖。平台门禁关闭前仍需 Hosted 编译与测试
确认恢复；发布包不得使用 fallback。

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
- Windows fallback 必须在 workflow 矩阵中显式声明，不能用于发布包。
- 当以下公开元数据路径成功返回并包含 `win64_msvc2022_64` 后，立即把 Windows
  矩阵恢复到 6.11.1：
  `https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6111/qt6_6111/Updates.xml`

## 影响

阶段 0 的 hosted build gate 可以立即在三个平台运行，但 Windows job 暂时没有
验证首选的精确 Qt patch 版本。在发布包被视为统一的 Qt 6.11.1 矩阵前，必须
移除这个 fallback。

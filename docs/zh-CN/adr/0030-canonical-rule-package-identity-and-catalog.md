# Canonical 规则包 Identity 与 Catalog

状态：已接受
日期：2026-07-28

## 背景

ADR-0014 把 application、language 与 package version 分开；ADR-0015 限定本地分发并要求
session 精确 pinning；ADR-0016 暂定 TOML 与 deterministic ZIP。M5 现在需要把 manifest、
content hash、compatibility range、catalog identity、本地目录导入、`.svrule` 扩展名与 hostile
package 边界固化成可执行契约。

这些选择必须早于 persistent cache 与 session 接入。package path 或 version 都不是充分
identity：目录和 archive 形式必须一致，两份不同 payload 不能静默占用同一 version，旧 session
也不能向前绑定新安装规则。若 hash ZIP 原始 byte，无关的 ZIP 表示差异会改变 identity，目录
导入也没有自然的等价输入。

Qt Core 提供 SHA-256，但没有公开的通用 ZIP API 或 TOML parser。首个 package slice 若为了
压缩引入 runtime dependency，会在压缩本身产生产品价值之前增加部署与 supply-chain 工作。
同时，输入格式从第一次实现开始就必须具备严格、跨平台的 path 与 size 规则。

## 决策

规范性的 version 1 契约维护在[规则包格式](../../rule-packages.md)中。它固定 `.svrule` 为 package
扩展名，并定义：

- 封闭的 TOML 1.0 `rule.toml` schema，包括 package metadata、精确 `MAJOR.MINOR` language
  contract、一个 `>=LOWER <UPPER` engine range、entry-point coverage、detector metadata、
  本地化文档与显式为空的首版 dependency list；
- 不带 build metadata 的严格 canonical semantic version；
- 只由 bounded regular file 与 canonical portable ASCII path 构成的 logical package tree；
- 对排序 logical path 和精确文件 byte 进行 domain-separated、length-framed SHA-256；
- package identity `(package ID, package version, content hash)`，以及额外包含 entry-point ID
  的 entry-point identity；
- exact catalog lookup、不同 version 并存、相同完整 identity 幂等，以及同 ID/version 不同
  content 时的 transactional conflict；
- header、timestamp、permission、顺序都固定且没有 optional field 的 deterministic store-only
  ZIP32 表示；
- bounded validation 与 staging、只读、content-addressed installation。

store-only archive 是有意选择。它是合法 ZIP，不依赖压缩库即可确定生成，并且不会膨胀超过
已经受限的输入 byte。importer 只接受 canonical 表示。未来的 compressed archive version
必须使用新的 manifest/package-format version，并由 ADR 定义 dependency、streaming
decompression、ratio 与 canonical encoder 行为。

version 1 package path 使用 ASCII，不做 Unicode normalization。non-ASCII path、case-fold alias、
traversal、link 与特殊文件都会被拒绝；package content 仍可包含任意 UTF-8 或受限 binary data。
这让 Windows、macOS 与 Linux 上的 identity 一致，同时允许未来通过显式格式修订加入 Unicode
path profile。

即使 package 与当前 engine 不兼容，installed 或 bundled package 仍可进入 catalog，以便升级后
保留精确历史内容。activation、analysis 与 session restore 会用 diagnostic 拒绝不兼容的精确
identity，绝不会猜测选择另一个 version。

source fingerprint、cache namespace framing、versioned cache payload 与 `.svsession`
serialization 将在后续 M5 slice 消费完整 package identity。本决策不允许只按 path 或 package
version 复用 cache。

## 后果

目录与 `.svrule` import 汇合到同一棵 validated tree 和同一个 content hash。catalog conflict
可见，不同 version 不覆盖地并存，session 也获得可精确 pin 的 identity。bounded store-only ZIP
reader/writer 可以仅用 Qt Core 与少量本地 binary framing code 实现。

只修改 manifest 空白也会改变 hash，因为 manifest 精确 byte 属于 package tree；这是有意行为：
digest 标识 supplied content，而不只是解析后的行为。对未改变的 tree 重新打包不会改变 hash。

严格的首版不包含 dependency graph、compressed archive、Unicode path、signature、trust
elevation、remote index 或 automatic update。增加其中任何一项都会改变安全或 identity 策略，
必须使用 versioned contract，不能靠宽松解析混入。

## 考虑过的方案

- hash ZIP 原始 byte：目录 import 没有稳定等价形式，metadata 或 compressor 差异也会改变
  package identity。
- 只 hash 解析后的 manifest 和 entry-point source：允许未追踪的 package 文档或测试数据在同一
  identity 下变化。
- 用 package ID 与 version 作为 catalog key：会静默 alias 被重新发布的 content，也无法 pin
  可复现 session。
- 允许不同 hash 在同一 ID/version 下并存：让文本 version 有歧义；显式 version conflict 更安全，
  并要求作者发布新版本。
- 接受任意安全 ZIP 并在 import 后 normalize：扩大 parser，同时让分发 artifact 本身仍不可复现。
- 使用 Qt private ZIP class：会在混合 Qt 6.10/6.11 CI matrix 上绑定 private API。
- 立即加入 compressed ZIP：为了小型、以文本为主的规则包增加 dependency 与更多 zip-bomb policy，
  暂无收益。
- normalize Unicode path：normalization、case behavior 与 filesystem alias 在支持平台间不同；
  ASCII version 1 profile 明确且可移植。

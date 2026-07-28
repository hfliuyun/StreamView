# StreamView 规则包格式

状态：已接受的 version 1 契约。英文
[规则包格式](../rule-packages.md) 是规范文本；本文是持续维护的中文伴随说明。

## Package 模型

一个规则包是一棵经过验证、只含普通文件的 logical tree。同一棵树既可以来自本地目录，也
可以来自一个确定性的 `.svrule` ZIP 容器；两种形式必须产生相同的 content hash 与 package
identity。

version 1 package 是自包含数据，只包含 DSL source、文档和可分发测试数据，不包含原生可执行
代码、symbolic link、运行时依赖解析或网络引用。package content 永远不会获得 file、process、
environment、network、pointer 或 native-library access。

package 根目录必须且只能有一个 `rule.toml` manifest。其他可接受文件位于 `src/`、`docs/`
或 `tests/` 下。manifest source path 必须指向 `src/` 下的 `.svfmt` 普通文件；documentation
path 必须指向 `docs/` 下的普通文件。未被 manifest 引用的数据仍然参与验证与 content hash。

## Manifest Version 1

`rule.toml` 使用不带 BOM 的 UTF-8 TOML 1.0。version 1 schema 如下；除了 `detector` 外，
展示的 scalar 与 table key 都是必需项，完整 `documentation` array 可省略；未知 key 或 table
会被拒绝。

```toml
manifest-version = 1

[package]
id = "org.streamview.h264"
version = "0.1.0"
authors = ["StreamView contributors"]
license = "MIT"
dependencies = []

[compatibility]
language = "0.1"
engine = ">=0.1.0 <0.2.0"

[[entrypoints]]
id = "annex-b"
format = "video.h264.annex-b"
source = "src/h264_annex_b.svfmt"
profiles = ["baseline", "main", "high"]
depth = "nal-header"
detector = "h264-annex-b"

[[documentation]]
language = "en"
path = "docs/en/h264-annex-b.md"

[[documentation]]
language = "zh-CN"
path = "docs/zh-CN/h264-annex-b.md"
```

字段含义如下：

- `manifest-version` 选择本 schema，值必须是整数 `1`。
- `package.id` 是稳定 package 名，由 2 到 8 个小写 ASCII 点分 segment 组成。segment
  以 `a-z` 开头，后续只含 `a-z`、`0-9` 或 `-`；每段最多 32 字符，完整 ID 最多 128
  字符。
- `package.version` 使用 SemVer 2.0.0 precedence，语法为
  `MAJOR.MINOR.PATCH[-PRERELEASE]`。core 数字分量除零自身外不能有 leading zero。
  prerelease 由一个或多个点分 identifier 组成，只含 ASCII 字母、数字或 `-`；纯数字
  identifier 也不能有 leading zero。空 identifier 与 `+` build metadata 都被拒绝。canonical
  文本原样作为 catalog version。
- `package.authors` 包含 1 到 16 个非空 display string，每项最多 256 UTF-8 byte。作者声明
  不会授予 trust 或额外权限。
- `package.license` 是非空 SPDX license-expression 文本，最多 256 ASCII 字符；version 1
  只验证这种表示，不声称进行 registry lookup、expression canonicalization 或 license approval。
- `package.dependencies` 必须是空数组；version 1 不支持非空依赖图或 runtime dependency
  resolution。
- `compatibility.language` 声明一个 canonical `MAJOR.MINOR` DSL contract。language major
  为零时，entry point 只有在它等于运行引擎的 `languageVersion()` 时才能激活；major 一及
  以后，运行 contract 必须 major 相同且 minor 不早于声明 minor。
- `compatibility.engine` 固定为 `>=LOWER <UPPER`，两个 canonical stable semantic version
  中间只有一个 ASCII 空格，且 `LOWER` 必须早于 `UPPER`。运行时
  `streamview::core::version()` 按 SemVer precedence 落入这个半开区间才兼容。它是 engine
  API contract version，不是 artifact 的 alpha/beta/release-candidate tag；只有 engine contract
  自身变化时，这些 tag 才影响兼容性。prerelease string 不是有效的 running engine-contract
  version。
- `entrypoints` 包含 1 到 64 项。每个 `id` 在 package 内唯一，由 1 到 64 个小写 ASCII
  字母、数字和连字符组成，且以字母开头。`format` 使用与 package ID 相同的点分语法。
  `source` 是 `src/` 下以 `.svfmt` 结尾的 canonical package path。`profiles` 包含 1 到
  64 个使用 entry-ID 语法的互异 token。`depth` 是一个描述覆盖深度的同类 token。可选
  `detector` 用 entry-ID 语法命名 host detector；它只是 metadata，不是 package 内可执行代码。
  version 1 已知 `h264-annex-b`；未知 detector 不妨碍 package 进入 catalog，但该 entry 的自动
  detection 会明确报告 unsupported。没有 detector 的 entry 仍可手动选择。entry-point ID 与
  source path 都必须唯一。
- `documentation` 包含 0 到 64 项。`language` 是互异、匹配
  `[a-z]{2,3}(-[A-Z]{2})?` 的 ASCII tag；`path` 是 `docs/` 下互异的 canonical package path。

TOML table 顺序与不影响语义的写法不会改变解析结果，但 manifest 原始 byte 是 logical tree
的一部分。因此只修改空白也会有意改变 package content hash。

## Path 与 Tree 上限

每个 logical path 都是相对、使用 `/` 分隔的 ASCII string。每个 component 只含 `A-Z`、
`a-z`、`0-9`、`.`、`_` 或 `-`，不能是空、`.` 或 `..`，也不能以点或空格结尾。
backslash、drive/UNC prefix、leading slash、control character 与非 ASCII path byte 都会直接
被拒绝，不会先 normalize。`%` 不在允许字符集中，name 永远不会被 percent-decode。version 1
的保守字符集从入口消除
Unicode normalization 与平台 case-folding alias；任意两个 path 在 ASCII case-fold 后也
必须唯一。

installer 只接受普通文件与真实目录，绝不跟随 symbolic link、junction、reparse-point alias
或其他特殊 filesystem entry。目录 import 以已打开的 root 或 parent handle 为锚、用 no-follow
语义打开每个 component，复核打开对象仍是预期且受限的普通文件或目录，再从稳定 handle 把 byte
复制进私有 staging。它不会先验证一个 path，之后又为 hash 或 install 重新打开该 path。
hard-link 关系不会保留；每个 logical path 都贡献并安装自己的 copied byte。

archive entry 只能表示文件；directory entry 以及 Unix file type 不是只读普通文件的条目都会
被拒绝。package data 永不执行。source path 必须以 `.svfmt` 结尾。filename 经 ASCII case fold
后若以 `.exe`、`.com`、`.dll`、`.dylib`、`.bundle`、`.app`、`.msi`、`.sys` 或 `.drv`
结尾，或者以 `.so` 加零个或多个点分十进制 ABI 分量结尾，就会被拒绝；filesystem 能暴露
executable permission bit 时，带任意此类 bit 的目录文件也会被拒绝。这些检查界定 declared
package format；即使任意 byte 使用无害 suffix，StreamView 也绝不执行 package file。

version 1 在安装前应用以下全部上限：

- 最多 1,024 个文件；
- 最多 16 个 path component，每个 component 最多 80 byte，完整 path 最多 240 byte；
- 单文件最多 8 MiB；
- `rule.toml` 最多 64 KiB；
- 全部文件解压后总计最多 64 MiB；
- 输入 `.svrule` container 最多 64 MiB。

所有上限计算都要 checked。archive 声明的 size 不能授权超过这些上限的分配或提取。

## Content Hash 与 Identity

content hash 是通过验证的 logical tree 的 SHA-256，不是目录 metadata 或 ZIP byte 的 hash。
文件按区分大小写的 ASCII path byte 排序。hash 输入为：

```text
ASCII("StreamViewRulePackage\0")
u32be(1)
u32be(file_count)
对每个文件：
    u32be(path_byte_count)
    path_ascii_bytes
    u64be(file_byte_count)
    exact_file_bytes
```

permission、timestamp、archive offset 与 source directory 名不参与 hash。显示形式是
`sha256:` 加 64 个小写 hexadecimal character。

package identity 是 `(package.id, package.version, content hash)` tuple；entry-point
identity 再加入 manifest 的 `entrypoints.id`。saved session、cache namespace 与 diagnostic
必须保留完整 tuple；path、package ID 或 version 单独都不能证明内容可以复用。

## Catalog 语义

catalog 让同一 package ID 的不同 semantic version 并存，不会静默替换或自动更新某个
version。重复注册完全相同的 identity 是 idempotent；用另一个 content hash 注册相同 package
ID 与文本 version 是显式 version conflict，并且不改变既有 catalog。persistent registration
在 `(package.id, package.version)` uniqueness constraint 下原子执行；并发注册要么观察到相同
hash，要么其中一个得到 conflict。

analysis catalog lookup 必须精确匹配：先解析 package ID、version 和 content hash，再解析
entry-point ID，最后检查 language contract 与当前 engine range。请求的 hash 不存在、但同一
package ID/version 已安装另一个 hash 时报告 `version-conflict`；没有该 version slot 时报告
`missing-content`。entry point 未知与 language/engine 不兼容各自有明确 diagnostic；selection
不会向前绑定另一个 installed version。推荐与 UI 排序使用 SemVer 2.0.0 precedence，但不能
改变 session 的精确 pinning。

## Deterministic `.svrule` Container

version 1 `.svrule` 是 canonical、single-disk ZIP32 archive，只使用 stored、未压缩的普通文件
entry。store-only encoding 让读取不依赖压缩库，也消除了 zip-bomb 边界中的压缩率歧义。

canonical writer 按 content hash 相同的 path 顺序输出文件，并固定所有表示选择：

- 每个文件一个 local header 和一个 central-directory record；
- local signature `0x04034b50`、version-needed `20`、flags `0`、method `0`、相互匹配的
  CRC-32 与 32-bit size、等于 canonical path 的 filename length、extra length `0`；
- DOS time `00:00:00`、date `1980-01-01`；
- 不允许 data descriptor、encryption、ZIP64、extra field、directory entry、entry comment、
  archive comment 或 trailing byte；
- central signature `0x02014b50`、version-made-by `0x0314`（Unix、ZIP 2.0）、
  version-needed `20`、与 local 相同的 flags/method/time/date/CRC/size/name、extra/comment
  length `0`、start disk `0`、internal attribute `0`、external attribute
  `(0100444 << 16)`，以及精确 local-header offset；
- end signature `0x06054b50`、disk field `0`、相等的 record count、精确 central-directory
  size/offset、comment length `0`。

ZIP64 sentinel count、size 与 offset 都是非法值。importer 从 byte zero 按生成顺序解析：每个
local header 都从当前 offset 开始，file data 紧随 name；central directory 从最后一个 data byte
后开始；每个 central record 指向对应的已记录 local offset；end record 紧随 central directory
并恰好到达 container length。local 与 central field 必须一致，name/data range 不能重叠或
alias，CRC-32 必须匹配。所有 offset、length、count 与 cumulative-size 运算使用 checked、至少
64-bit 的算术，并在 allocation 或 seek 前同时落入实际 container 与 version 1 上限。即使 ZIP
安全但不 canonical，也会被拒绝；它可以由其他工具解包后再用 StreamView 重新打包。同一
logical tree 的 canonical byte sequence 在所有支持平台上必须确定；重复打包结果必须逐 byte
相同。

## Content-Addressed 安装

通过验证的内容安装在 rule store 的
`sha256/<digest 前两字符>/<完整 digest>/` 下。installer 在同一 filesystem 独占创建不可预测的
sibling staging directory，只写入 copied byte，再次验证 staged logical tree 与 hash，把文件
设为只读，然后通过原子 rename 放入不存在的最终位置；它绝不 merge 或替换既有 destination。
并发 installer 要么只创建一次 destination，要么重新验证胜者。stale private staging directory
没有 cataloged package，可以清理。

已经存在的 destination 只有在使用相同 no-link/no-reparse、path、type、size、manifest 与 hash
规则重新验证得到同一 digest 时才接受；corruption 会被报告，不会被修复或静默替换。store
ownership 必须防止不受信 writer 在验证与 catalog 使用之间修改 tree。

content-addressed directory 是 immutable package data。catalog metadata 可以引用它；只有当
没有 saved session 或 installed catalog record pin 某个 hash 时，未来的垃圾回收才能删除它。

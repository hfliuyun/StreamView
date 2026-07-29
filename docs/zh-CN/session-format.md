# StreamView 会话格式

状态：已接受的 version 1 契约。英文
[会话格式](../session-format.md) 是规范文本；本文是持续维护的中文伴随说明。

## 目的与边界

`.svsession` 是一份紧凑记录，保存用户围绕一个只读本地媒体文件和一个精确 rule entry
point 完成的工作。它不包含媒体 byte、SQLite cache page、live analyzer checkpoint 或已安装
rule-package content。可重建的 progressive index 与 materialized result 留在本文档之外。

文档中的 path 只用于重新打开媒体文件，保存的 source identity 只作描述；两者都不能授权
复用。StreamView 只有在新打开文件 handle 上重新计算的 fingerprint 与完整 saved
fingerprint 相等后，才会应用 bookmark、annotation、expanded path 或 view state。

## JSON Version 1

Version 1 使用 UTF-8 JSON，编码后最多 1 MiB，最大嵌套深度为 256。每个 object 都使用闭合
字段集合；缺失字段、未知字段、重复 key、错误 JSON type、malformed value，以及不支持的
schema 或 fingerprint version 都会被拒绝。重复比较使用解码后的 JSON string，因此
`"schemaVersion"` 与 `"\u0073chemaVersion"` 同样冲突。

canonical writer 使用如下结构：

```json
{
    "schemaVersion": 1,
    "source": {
        "path": "/media/fixture.264",
        "identity": "/media/fixture.264",
        "fingerprint": {
            "version": 1,
            "mode": "full-content-sha256",
            "sizeBytes": "5",
            "sha256": "1111111111111111111111111111111111111111111111111111111111111111"
        }
    },
    "rule": {
        "packageId": "org.example.packet",
        "packageVersion": "1.2.3",
        "contentSha256": "2222222222222222222222222222222222222222222222222222222222222222",
        "entryPointId": "packet"
    },
    "bookmarks": [
        { "label": "Header", "sourceBitOffset": "8" }
    ],
    "annotations": [
        { "text": "Check this flag", "sourceBitOffset": "9", "bitLength": "3" }
    ],
    "expandedPaths": ["root/packet"],
    "view": {
        "rawPageIndex": "0",
        "rawDisplayMode": "combined",
        "selectedSourceBitOffset": "10",
        "selectedAnalysisPath": "root/packet/value"
    }
}
```

所有 unsigned 64-bit quantity 都是 canonical decimal string：`0`，或一个非零数字后接十进制
数字，不带 sign 或 leading zero。sampled fingerprint 的 modification time 是 canonical signed
decimal string；`+`、`-0` 与 leading zero 都非法。string 表示避免经过 JSON binary64 number
model 时丢失精度。SHA-256 value 必须恰好是 64 个小写 hexadecimal character，不带
`sha256:` prefix。

## Source Fingerprint

`source.fingerprint` object 是完整的 version 1 `SourceFingerprint`：

- `version` 是 JSON number `1`；
- `mode` 是 `full-content-sha256` 或 `sampled-sha256`；
- `sizeBytes` 是声明的 source size；
- `sha256` 是 fingerprint digest；
- `modificationTimeNanoseconds` 只在 sampled mode 必需，在 full-content mode 禁止。

mode、size、modification-time presence 与 digest 必须满足
[ADR-0031](adr/0031-versioned-file-source-fingerprints.md) 的 versioned fingerprint 契约。
fingerprint 从 analysis 使用的同一个只读 `FileSource` handle 计算。virtual source 不能仅仅
返回一个看似 path 的 `identity()` 就获得持久化资格。

## 精确 Rule Pin

`rule` object 是完整的 `RuleEntryPointIdentity`：package ID、canonical package version、
package logical-tree content hash 与 entry-point ID。恢复时会结合当前 DSL 和 engine version
做精确 catalog lookup。content 缺失、同一 ID/version 被另一个 hash 占据、未知 entry point、
DSL 不兼容与 engine 不兼容都是不同失败；恢复绝不会选择更新或其他相近 version。

bundled H.264 analyzer 走同一路径：package 通过 catalog 契约加载、解析，analyzer 保留完整
identity。因此 catalog 已解析的 installed package 也可在同一 exact-pin 规则下重建 analyzer。

## 用户状态

Version 1 保存：

- 最多 4,096 个 bookmark；每个含一个非空且最多 64 KiB UTF-8 的 label，以及 pinned source
  中真实存在的一个 source bit；
- 最多 4,096 个 annotation；每个含非空且最多 64 KiB UTF-8 的 text，以及包含在 source
  内、非空且不溢出的 source-bit range；
- 最多 16,384 个互异、非空的 expanded analysis path，每个最多 16 KiB UTF-8；
- 一个 view state：有效的 64 KiB raw page index、`hex`/`binary`/`combined` 之一，以及可为
  null 的 source-bit 与 analysis-path selection。

source path 与 identity 都必须非空且最多 16 KiB UTF-8。selected source bit 必须存在；
selected analysis path 若存在，必须非空且最多 16 KiB UTF-8。selection 不存在时必须写 JSON
`null`，不能省略字段。

## 保存与恢复

保存使用关闭 direct-write fallback 的 `QSaveFile`。StreamView 写 sibling temporary file，只有
`commit()` 成功才替换 destination；无法提供这种原子替换时必须失败。

恢复相对于 active application session 按如下顺序事务式执行：

1. 解析并验证完整 session document；
2. 以只读方式打开 `source.path`；
3. 计算并比较完整 source fingerprint；
4. 在 catalog 中解析完整 rule identity 与兼容性；
5. 从该 resolved package entry 编译并构造 analyzer；
6. 把经过验证的 user state 绑定到新 session。

任一较早步骤失败都不会构造 replacement session，也不会应用任何 saved user coordinate。
cache lookup、cache payload restoration、Save As UI、dirty-state prompt 与 source relocation 属于
独立契约。

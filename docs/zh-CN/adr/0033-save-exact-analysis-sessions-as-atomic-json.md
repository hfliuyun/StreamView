# 以原子 JSON 保存精确 Analysis Session

状态：已接受
日期：2026-07-29

## 背景

ADR-0007 把持久 user work 与不可变 media 分离；ADR-0030 定义完整 package 与 entry-point
identity，ADR-0031 定义持久 local-file fingerprint，ADR-0032 把这些 value 绑定进 cache
namespace。M5 仍需要紧凑 `.svsession` 表示，并固定 saved source coordinate 与 rule selection
可以安全复用的时点。

path identity 不够：file 可以原地改变，package version 也可能被另一份 content 重新发布。
JSON number 同样不适合任意 64-bit bit coordinate，因为常见 JSON implementation 以 binary64
保存它。允许 unknown 或 duplicate field 的宽松文档会使 migration 与 hostile-file diagnostic
产生歧义；直接覆盖唯一副本则可能在 short write 或 crash 时毁掉 user work。

当前 H.264 analyzer 会验证 bundled package，但随后只编译一份 detached source string，并丢弃
package identity。因此恢复 exact pin 还要求 analyzer construction path 消费一份经过精确兼容
catalog lookup 的结果。

## 决策

规范 version 1 format 维护在[会话格式](../session-format.md)，固定：

- 由 `schemaVersion: 1` 选择的闭合、有界 UTF-8 JSON schema；
- 所有 64-bit source coordinate 与 size 使用 canonical decimal string；
- 完整 version 1 `SourceFingerprint`，包括 sampled-mode nanosecond modification time；
- 完整 `RuleEntryPointIdentity`，包括 package logical-tree hash；
- 有界 bookmark、annotation、expanded analysis path 与 raw/selection view state；
- 在暴露 user state 前拒绝 missing、unknown、duplicate、mistyped、out-of-range 或 unsupported
  value；
- 通过关闭 direct-write fallback 的 `QSaveFile` 原子替换。

恢复会打开 saved source path，从同一只读 `FileSource` handle 计算 fingerprint，并要求完整相等
后才做精确 catalog lookup。随后不带 fallback 地解析 package ID、version、content hash、entry
point、DSL compatibility 与 engine compatibility。只有 resolved package entry 可以构造 analyzer。
bundled H.264 package 走同一路径，其 analyzer 保留完整 resolved identity。

只有 source comparison、catalog lookup 与 analyzer construction 全部成功后，validated user
state 才绑定到 session。失败不会返回 replacement session。saved source identity 只作描述；它
和 path 都不能替代 fingerprint。virtual source 不能仅返回 path-like identity 就持久化。

文档不包含 media byte、rule package byte、cache page 或 live analyzer state。这些数据要么是
immutable external input，要么是 rebuildable owner data，使用独立 versioning 与 lifecycle 契约。

## 后果

- old session 要么找到其命名的 exact package content，要么返回明确 catalog status；绝不会
  fall forward 到另一个 rule version。
- 同一路径上的 media 变化会在应用 saved coordinate 前被发现；ADR-0031 下 small-file
  metadata-only 变化仍然有效。
- 大整数在所有受支持 Qt 平台上 round-trip，不会丢失 JSON 精度。
- session write 要求 sibling temporary file 的原子替换；filesystem 无法提供时保存失败，不会
  静默削弱 durability。
- parser 与 collection bounds 限制 hostile document work；闭合 object 使未来 schema change
  必须显式发生。
- Save/Save As action、dirty-state prompt、source relocation、rule-management UI、background
  cache owner 与 live analyzer restoration 留作后续工作；cache owner body format 由
  ADR-0034 单独规定。

## 放弃的方案

- 只保存 source 和 package path：两者都不能证明 byte identity。
- 保存不带 content hash 的 package ID/version：重新发布的 content 可能静默重解释旧工作。
- 以 JSON number 保存 64-bit coordinate：binary64 不能精确表示所有 value。
- 为 forward compatibility 接受 unknown field：versioned closed schema 更利于明确 migration，
  也能拒绝拼错的安全相关 field。
- 在 `.svsession` 保存 cache page：它可重建、独立 versioned，而且可能远大于紧凑 user state。
- 用 `QFile` 直接写 destination：partial replacement 可能毁掉最后一份有效 session。

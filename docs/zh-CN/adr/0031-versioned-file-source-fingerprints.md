# 版本化文件 Source Fingerprint

状态：已接受
日期：2026-07-29

## 背景

保存位置与可重建 analysis page 绝不能只按 `RandomAccessSource::identity()` 重新绑定。
`FileSource` 的该 identity 类似 path，同一个字符串可以指向已经替换的内容。小文件全文 hash
足够精确，但对 100 GB media 全文 hash 会破坏有界初始工作。M5 因此必须在 cache namespace
或 `.svsession` 允许复用之前，先固定一个显式且版本化的文件 fingerprint 契约。

实现必须 fingerprint 已经打开用于分析的文件。重新打开 path 可能 hash 到替换后的文件，而
session 仍在读取旧 handle。metadata 或 content 也可能在计算期间变化，此时必须显式诊断，
不能把混合状态序列化为一致 identity。

## 决策

`SourceFingerprint` version 1 是经过验证的 core value，包含 algorithm version、封闭 mode、
文件大小、可选的纳秒 modification time 和一个 32-byte SHA-256 digest。它不包含 filesystem
path，也不包含 `RandomAccessSource::identity()`。

`FileSource::fingerprint()` 在分析所用的同一个只读 OS handle 上计算，并与分析读取共用同一个
seek/read mutex。hash 前后都获取 file size 与 modification time。size 与打开时记录值不同，
或前后 snapshot 有变化时，返回 `source-changed`；不支持的 handle metadata 与 I/O error
保持为不同状态。

version 1 使用两种 mode：

- 小于等于 3 MiB 的文件使用 `full-content-sha256`。digest 是对全部文件 byte 计算的
  SHA-256；durable value 包含 version、mode、size 与 digest，不保存 modification time，
  因此只 touch 未改变的小文件不会让 session 失效。
- 更大的文件使用 `sampled-sha256`。durable value 包含 version、mode、size、Unix epoch
  纳秒 modification time，以及三个 1 MiB window 拼接后的 SHA-256。三个 window 从 byte 0、
  `floor((size - 1 MiB) / 2)` 和 `size - 1 MiB` 开始。

3 MiB 分界保证全文 hash 的读取量不会超过大文件的三个 sample。hash 使用 64 KiB 工作
buffer，内存不依赖 source size。POSIX timestamp 来自 `fstat` 的纳秒字段；Windows
`FILETIME` 从 100 ns tick 转换为 Unix epoch 纳秒。构造时会拒绝不支持的 version、与
mode 不匹配的 size/mtime 形态和非 SHA-256 digest。

只有本地 regular `FileSource` 自动获得该契约。virtual source 或未来 remote source 必须定义
显式、可信的 fingerprint 策略；不得回退使用调用方给出的 identity 文本。

## 后果

小文件获得 content-exact durable identity，并避免 metadata-only 的误报。大文件只需读取
3 MiB 加两次 metadata snapshot；size 与纳秒 mtime 按已接受的产品算法覆盖 sample window
之外的变化。sampled fingerprint 用于变化检测，并不是对每个 byte 的密码学承诺。

cache namespace framing 与 `.svsession` serialization 现在可以消费一个经过验证的 value
及其 algorithm version。它们仍必须绑定精确 rule package/entry-point identity，以及自身的
schema 与 payload version；本决策本身不授权 persistent cache 复用。

## 考虑过的方案

- 只使用 path、size 与 mtime：替换内容仍可保留三者。
- hash 每个文件的全文：打开或恢复大型 media 会依赖一次完整顺序读取。
- 为小文件保存 mtime：仅修改 metadata 也会让 content-exact fingerprint 失效。
- 为 hash 重新打开 path：可能 fingerprint 到与当前 analysis handle 不同的文件。
- 给所有 `RandomAccessSource` 增加 `fingerprint()`：会允许 virtual source 在没有显式
  provenance 契约时自行声称 durable identity。

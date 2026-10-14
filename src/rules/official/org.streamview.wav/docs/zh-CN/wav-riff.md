# WAV (RIFF/WAVE) 官方规则包

包 `org.streamview.wav` 版本 0.1.0 —— 入口点 `wave`，格式 `audio.wav`，
探测器 `riff-wave`（ADR-0115）。

## 范围

RIFF/WAVE 容器文件的 bit 级结构视图：

- 12 字节表单头（`'RIFF'` + 表单长度 + `'WAVE'`），由 `riff_chunk` 扫描器
  作为第一条记录发出；以及
- chunk 链（`chunk_id` + 小端 `chunk_size` + 载荷 + 奇数载荷后的一个
  pad 字节），按长度链语义步进，以表单终点为界。

## 解码的 chunk

| Chunk | 解码 |
| :--- | :--- |
| `fmt `（`0x666D7420`） | 16 字节公共 WAVEFORMATEX 总是解码：`audio_format`、`num_channels`、`sample_rate`、`byte_rate`、`block_align`、`bits_per_sample`（全小端）。`chunk_size == 18` 额外解码 `fmt_cb_size`；`chunk_size == 40` 额外解码 `extensible_cb_size`、`valid_bits_per_sample`、`channel_mask`、`subformat_guid_data1`、`subformat_guid_tail[12]`（WAVE_FORMAT_EXTENSIBLE）。其余长度把剩余字节登记为 opaque 惰性区域 `fmt_tail`。 |
| `fact`（`0x66616374`） | `chunk_size == 4` 解码 `sample_length`；其余长度保持 opaque。 |
| 其他全部 | opaque 惰性载荷（`data`、`LIST`、`JUN K`、未知 chunk）。 |

`audio_format` 刻意是**不带 `@enum` 的普通 16 位字段**：tag 空间开放，
封闭枚举会致命拒绝合法文件（ADR-0040/0059 二分法）。常见 tag 写在字段
说明里。PCM 采样字节永不解码；`data` 保持 lazy opaque 区域。

## 延期项

- `RIFX`（`0x52494658`）、`RF64`（`0x52463634`）与 `ds64` chunk 在 v0.1
  之外，不产生 WAV 检测候选。
- 压缩 WAVE 载荷（ADPCM 等）保持 opaque；格式 tag 在 `fmt ` 中可见。
- 非规范长度的 fmt chunk 退化为 opaque 尾部。

## 帧定说明

- 越过表单终点或源终点的 chunk 载荷长度在边界钳制并给出
  truncated-source 诊断；没有重同步。
- 表单终点之后的字节不被平铺认领。
- 扫描为渐进式，取消后可原地恢复。

## 规范参考

- Microsoft & IBM，"Multimedia Programming Interface and Data
  Specifications 1.0"（1991 年 8 月）
- [MS-RIFF] —— Microsoft RIFF File Format
- Microsoft Learn —— WAVEFORMATEX / WAVEFORMATEXTENSIBLE
- RFC 2361（信息性，tag 含义）

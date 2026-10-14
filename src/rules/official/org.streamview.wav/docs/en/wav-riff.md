# WAV (RIFF/WAVE) Official Rule Package

Package `org.streamview.wav` version 0.1.0 — entry point `wave`, format
`audio.wav`, detector `riff-wave` (ADR-0115).

## Scope

Structural bit-level view of RIFF/WAVE container files:

- the 12-byte form header (`'RIFF'` + form size + `'WAVE'`), emitted by the
  `riff_chunk` scanner as the first record; and
- the chunk chain (`chunk_id` + little-endian `chunk_size` + payload + one
  pad byte for odd payload sizes), walked with length-chain semantics and
  bounded by the form end.

## Decoded chunks

| Chunk | Decoding |
| :--- | :--- |
| `fmt ` (`0x666D7420`) | The 16 common WAVEFORMATEX bytes always decode: `audio_format`, `num_channels`, `sample_rate`, `byte_rate`, `block_align`, `bits_per_sample` (all little-endian). `chunk_size == 18` additionally decodes `fmt_cb_size`; `chunk_size == 40` additionally decodes `extensible_cb_size`, `valid_bits_per_sample`, `channel_mask`, `subformat_guid_data1`, and `subformat_guid_tail[12]` (WAVE_FORMAT_EXTENSIBLE). Every other size registers the remaining bytes as the opaque lazy region `fmt_tail`. |
| `fact` (`0x66616374`) | `chunk_size == 4` decodes `sample_length`; other sizes stay opaque. |
| everything else | Opaque lazy payload (`data`, `LIST`, `JUN K`, unknown chunks). |

`audio_format` is deliberately a plain 16-bit field without `@enum`: the tag
space is open-ended and a closed enumeration would fatally reject valid
files (ADR-0040/0059 dichotomy). Common tags are documented in the field
description. PCM sample bytes are never decoded; `data` remains a lazy
opaque region.

## Postponements

- `RIFX` (`0x52494658`), `RF64` (`0x52463634`), and `ds64` chunks are outside
  v0.1 and produce no WAV detection candidate.
- Compressed WAVE payloads (ADPCM and friends) keep an opaque payload; the
  format tag remains visible in `fmt `.
- fmt chunks with non-canonical sizes degrade to an opaque tail.

## Framing notes

- Chunk payload sizes that run past the form end or the source end clamp at
  the bound with a truncated-source diagnostic; there is no resynchronization.
- Bytes after the form end stay unclaimed by the tiling.
- The scan is progressive and resumable in place after cancellation.

## Normative references

- Microsoft & IBM, "Multimedia Programming Interface and Data Specifications
  1.0" (August 1991)
- [MS-RIFF] — Microsoft RIFF File Format
- Microsoft Learn — WAVEFORMATEX / WAVEFORMATEXTENSIBLE
- RFC 2361 (informational, tag meanings)

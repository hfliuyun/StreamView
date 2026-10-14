#!/usr/bin/env python3
"""Generates the deterministic WAV (RIFF/WAVE) fixtures from ADR-0115.

Every fixture is assembled from explicit byte construction; bit offsets are
never hand-computed in tests. Run from the repository root:

    python3 tests/fixtures/generate_wav_fixtures.py
"""

import struct
from pathlib import Path
from typing import Optional

FIXTURE_DIR = Path(__file__).resolve().parent


def chunk(chunk_id: bytes, payload: bytes) -> bytes:
    """A RIFF chunk with the even-alignment pad byte when the size is odd."""
    data = chunk_id + struct.pack("<I", len(payload)) + payload
    if len(payload) % 2 == 1:
        data += b"\x7e"
    return data


def wave(chunks: bytes, riff_size: Optional[int] = None) -> bytes:
    """A RIFF/WAVE form; riff_size overrides the declared form size."""
    if riff_size is None:
        riff_size = 4 + len(chunks)  # form size counts the 'WAVE' type too
    return b"RIFF" + struct.pack("<I", riff_size) + b"WAVE" + chunks


def fmt_pcm(bits_per_sample: int, channels: int, sample_rate: int) -> bytes:
    block_align = channels * bits_per_sample // 8
    return struct.pack(
        "<HHIIHH",
        1,  # WAVE_FORMAT_PCM
        channels,
        sample_rate,
        sample_rate * block_align,
        block_align,
        bits_per_sample,
    )


def write(name: str, data: bytes) -> None:
    (FIXTURE_DIR / name).write_bytes(data)
    print(f"wrote {name} ({len(data)} bytes)")


def main() -> None:
    # W2-1: minimal valid PCM file (8-bit mono 8 kHz, 16-byte fmt, 4-byte data).
    write(
        "wav_w2_minimal_pcm.wav",
        wave(chunk(b"fmt ", fmt_pcm(8, 1, 8000)) + chunk(b"data", b"\x00\x01\x02\x03")),
    )

    # W2-2: 16-bit stereo 44.1 kHz with fact, LIST, and an odd-size unknown
    # chunk exercising the pad byte.
    odd_unknown = chunk(b"JUNK", b"\xaa\xbb\xcc")
    write(
        "wav_w2_pcm16_stereo.wav",
        wave(
            chunk(b"fmt ", fmt_pcm(16, 2, 44100))
            + chunk(b"fact", struct.pack("<I", 44100))
            + odd_unknown
            + chunk(b"LIST", b"INFOISFT" + struct.pack("<I", 6) + b"StreamView")
            + chunk(b"data", b"\x00\x00" * 64)
        ),
    )

    # W2-3: WAVE_FORMAT_EXTENSIBLE (fmt chunk of exactly 40 bytes).
    extensible = struct.pack(
        "<HHIIHH",
        0xFFFE,
        2,
        48000,
        48000 * 8,
        8,
        32,
    ) + struct.pack("<HHI", 22, 32, 0x3) + b"\x01\x00\x00\x00" + bytes(12)
    assert len(extensible) == 40
    write(
        "wav_w2_extensible.wav",
        wave(chunk(b"fmt ", extensible) + chunk(b"data", b"\x00" * 32)),
    )

    # W2-4: truncated header (RIFF + size, no WAVE form type).
    write("wav_w2_truncated_header.wav", b"RIFF" + struct.pack("<I", 16))

    # W2-5: data chunk payload extending past EOF.
    write(
        "wav_w2_truncated_chunk.wav",
        wave(chunk(b"fmt ", fmt_pcm(8, 1, 8000)) + chunk(b"data", b"\x00" * 4), riff_size=8 + 16 + 8 + 64),
    )

    # W2-6: malformed sizes (riff_size < 4, and a 0xFFFFFFFF chunk size).
    write("wav_w2_malformed_riff_size.wav", wave(b"", riff_size=2) + chunk(b"fmt ", fmt_pcm(8, 1, 8000)))
    write(
        "wav_w2_oversized_chunk.wav",
        wave(chunk(b"fmt ", fmt_pcm(8, 1, 8000)) + b"data" + struct.pack("<I", 0xFFFFFFFF)),
    )

    # W2-7: unsupported-but-recognizable compressed codec (IMA ADPCM, tag 17).
    adpcm = struct.pack("<HHIIHH", 17, 1, 8000, 4042, 505, 4)
    write(
        "wav_w2_adpcm.wav",
        wave(chunk(b"fmt ", adpcm) + chunk(b"fact", struct.pack("<I", 8000)) + chunk(b"data", b"\x00" * 16)),
    )

    # W2-8: trailing bytes after the form end.
    write(
        "wav_w2_trailing_bytes.wav",
        wave(chunk(b"fmt ", fmt_pcm(8, 1, 8000)) + chunk(b"data", b"\x00" * 4)) + b"STREAMVIEWTRAILING",
    )

    # W2-9: valid MP4 whose mdat carries RIFF/WAVE bytes (detector containment).
    ftyp = struct.pack(">I", 20) + b"ftypisom" + struct.pack(">I", 512) + b"isom"
    free = struct.pack(">I", 8) + b"free"
    riff_payload = wave(chunk(b"fmt ", fmt_pcm(8, 1, 8000)) + chunk(b"data", b"\x00" * 8))
    mdat = struct.pack(">I", 8 + len(riff_payload)) + b"mdat" + riff_payload
    write("wav_w2_riff_in_mp4.mp4", ftyp + free + mdat)


if __name__ == "__main__":
    main()

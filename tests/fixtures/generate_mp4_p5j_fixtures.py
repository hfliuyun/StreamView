#!/usr/bin/env python3
"""
Generate reproducible MP4 binary test fixtures for Task P5j-1:
- stss (sync sample table, sparse and absent)
- ctts (composition offset table, version 0 unsigned and version 1 signed)
- stz2 (compact sample size table with field_size 4, 8, and 16)
"""

import struct
from pathlib import Path

FIXTURES_DIR = Path(__file__).resolve().parent

def make_box(fourcc_str: str, payload: bytes) -> bytes:
    return struct.pack(">I4s", len(payload) + 8, fourcc_str.encode("ascii")) + payload

def make_large_box(fourcc_str: str, payload: bytes) -> bytes:
    return struct.pack(">I4sQ", 1, fourcc_str.encode("ascii"), len(payload) + 16) + payload

def make_eof_box(fourcc_str: str, payload: bytes) -> bytes:
    return struct.pack(">I4s", 0, fourcc_str.encode("ascii")) + payload

def make_full_box(fourcc_str: str, version: int, flags: int, payload: bytes) -> bytes:
    v_flags = ((version & 0xFF) << 24) | (flags & 0x00FFFFFF)
    return make_box(fourcc_str, struct.pack(">I", v_flags) + payload)

def make_large_full_box(fourcc_str: str, version: int, flags: int, payload: bytes) -> bytes:
    v_flags = ((version & 0xFF) << 24) | (flags & 0x00FFFFFF)
    return make_large_box(fourcc_str, struct.pack(">I", v_flags) + payload)

def make_eof_full_box(fourcc_str: str, version: int, flags: int, payload: bytes) -> bytes:
    v_flags = ((version & 0xFF) << 24) | (flags & 0x00FFFFFF)
    return make_eof_box(fourcc_str, struct.pack(">I", v_flags) + payload)

def make_ftyp() -> bytes:
    payload = b"isom" + struct.pack(">I", 512) + b"isom"
    return make_box("ftyp", payload)

def wrap_stbl(stbl_children: bytes) -> bytes:
    stbl = make_box("stbl", stbl_children)
    minf = make_box("minf", stbl)
    mdia = make_box("mdia", minf)
    trak = make_box("trak", mdia)
    return make_box("moov", trak)

def write_fixture(name: str, data: bytes) -> Path:
    out_path = FIXTURES_DIR / name
    out_path.write_bytes(data)
    print(f"Generated {out_path} ({len(data)} bytes)")
    return out_path

def stss_box(sample_numbers, version: int = 0) -> bytes:
    payload = struct.pack(">I", len(sample_numbers))
    payload += b"".join(struct.pack(">I", n) for n in sample_numbers)
    return make_full_box("stss", version, 0, payload)

def ctts_box(entries, version: int) -> bytes:
    """entries: list of (sample_count, sample_offset). Offsets are packed as raw
    32-bit big-endian words; negative values are written in two's complement so
    that version 1 tables carry the exact wire bytes a real muxer emits."""
    payload = struct.pack(">I", len(entries))
    for sample_count, sample_offset in entries:
        payload += struct.pack(">I", sample_count)
        payload += struct.pack(">I", sample_offset & 0xFFFFFFFF)
    return make_full_box("ctts", version, 0, payload)

def stz2_box(sizes, field_size: int, version: int = 0) -> bytes:
    payload = struct.pack(">I", field_size & 0xFF)  # 24-bit reserved + 8-bit field_size
    payload += struct.pack(">I", len(sizes))
    if field_size == 4:
        # Two 4-bit sizes per byte; an odd sample_count pads the final low nibble.
        packed = bytearray()
        for index in range(0, len(sizes), 2):
            high = sizes[index] & 0x0F
            low = sizes[index + 1] & 0x0F if index + 1 < len(sizes) else 0
            packed.append((high << 4) | low)
        payload += bytes(packed)
    elif field_size == 8:
        payload += b"".join(struct.pack(">B", s) for s in sizes)
    elif field_size == 16:
        payload += b"".join(struct.pack(">H", s) for s in sizes)
    else:
        payload += b""
    return make_full_box("stz2", version, 0, payload)

def generate_sync_and_composition_v0() -> Path:
    """stts + stss (sparse) + ctts version 0, the common non-B-frame layout."""
    stts_payload = struct.pack(">I", 1) + struct.pack(">II", 6, 1000)
    stts = make_full_box("stts", 0, 0, stts_payload)

    # Sparse sync samples: only samples 1 and 4 of 6 are random access points.
    stss = stss_box([1, 4])

    # Version 0 offsets are unsigned: a constant positive presentation delay.
    ctts = ctts_box([(3, 2000), (3, 1000)], version=0)

    moov = wrap_stbl(stts + stss + ctts)
    mdat = make_box("mdat", b"\xAA" * 16)
    data = make_ftyp() + moov + mdat
    return write_fixture("mp4_p5j_stss_ctts_v0.mp4", data)

def generate_composition_v1_negative() -> Path:
    """ctts version 1 carrying a negative offset in two's complement.

    ADR-0105 section 3.2: the rule decodes sample_offset as unsigned 32-bit and the
    sign reinterpretation happens downstream, so the fixture asserts the raw
    unsigned wire value (0xFFFFFC18 == -1000 signed).
    """
    stts_payload = struct.pack(">I", 1) + struct.pack(">II", 4, 512)
    stts = make_full_box("stts", 0, 0, stts_payload)
    ctts = ctts_box([(1, -1000), (1, 0), (2, 3000)], version=1)

    moov = wrap_stbl(stts + ctts)
    mdat = make_box("mdat", b"\xBB" * 16)
    data = make_ftyp() + moov + mdat
    return write_fixture("mp4_p5j_ctts_v1_negative.mp4", data)

def generate_stss_absent() -> Path:
    """A track with no stss at all: every sample is a sync sample (ISO 14496-12
    section 8.6.2.1). The rule cannot express that default, so this fixture exists
    to prove the container still analyzes cleanly and no stss node is produced."""
    stts_payload = struct.pack(">I", 1) + struct.pack(">II", 5, 1024)
    stts = make_full_box("stts", 0, 0, stts_payload)
    stsz_payload = struct.pack(">II", 0, 5) + struct.pack(">IIIII", 10, 20, 30, 40, 50)
    stsz = make_full_box("stsz", 0, 0, stsz_payload)

    moov = wrap_stbl(stts + stsz)
    mdat = make_box("mdat", b"\xCC" * 16)
    data = make_ftyp() + moov + mdat
    return write_fixture("mp4_p5j_stss_absent.mp4", data)

def generate_compact_sample_sizes() -> Path:
    """stz2 with all three legal field_size widths in one stbl."""
    # field_size 4, odd sample_count: 5 samples pack into 3 bytes with a padded nibble.
    stz2_4 = stz2_box([1, 2, 3, 4, 5], field_size=4)
    # field_size 8: one byte per sample.
    stz2_8 = stz2_box([200, 100, 50], field_size=8)
    # field_size 16: two bytes per sample.
    stz2_16 = stz2_box([4096, 8192], field_size=16)

    moov = wrap_stbl(stz2_4 + stz2_8 + stz2_16)
    mdat = make_box("mdat", b"\xDD" * 16)
    data = make_ftyp() + moov + mdat
    return write_fixture("mp4_p5j_stz2_field_sizes.mp4", data)

def generate_unsupported_versions() -> Path:
    """Version 1 stss and version 2 ctts / stz2: deterministic Unsupported diagnostics."""
    stss = stss_box([1], version=1)
    ctts = ctts_box([(1, 100)], version=2)
    stz2 = stz2_box([1, 2], field_size=8, version=1)

    moov = wrap_stbl(stss + ctts + stz2)
    mdat = make_box("mdat", b"\x00" * 16)
    data = make_ftyp() + moov + mdat
    return write_fixture("mp4_p5j_unsupported_versions.mp4", data)

def generate_unsupported_field_size() -> Path:
    """stz2 with an illegal field_size (12): version is supported but the width is not."""
    stz2 = stz2_box([1, 2], field_size=12)

    moov = wrap_stbl(stz2)
    mdat = make_box("mdat", b"\x00" * 16)
    data = make_ftyp() + moov + mdat
    return write_fixture("mp4_p5j_stz2_bad_field_size.mp4", data)

def generate_largesize_and_eof() -> Path:
    """Exercise the 64-bit largesize and size==0 framing branches of the new boxes.
    size == 0 must be terminal within the stbl payload, so stz2 stays last."""
    stss = make_large_full_box(
        "stss", 0, 0, struct.pack(">I", 2) + struct.pack(">II", 2, 7)
    )
    ctts = make_large_full_box(
        "ctts", 0, 0, struct.pack(">I", 1) + struct.pack(">II", 9, 4500)
    )
    stz2_payload = struct.pack(">I", 8) + struct.pack(">I", 2) + bytes([77, 88])
    stz2 = make_eof_full_box("stz2", 0, 0, stz2_payload)

    moov = wrap_stbl(stss + ctts + stz2)
    mdat = make_box("mdat", b"\x00" * 16)
    data = make_ftyp() + moov + mdat
    return write_fixture("mp4_p5j_largesize_and_eof_new_tables.mp4", data)

def generate_realistic_combined_track() -> Path:
    """A realistic B-frame track: all six sample table boxes in one stbl.

    6 samples across 4 chunks with 64-bit chunk offsets, sparse sync samples and a
    reordering ctts. This is the fixture that proves the three new dispatch branches
    coexist with the P5g boxes rather than shadowing them.
    """
    # 6 samples, constant duration.
    stts = make_full_box("stts", 0, 0, struct.pack(">I", 1) + struct.pack(">II", 6, 1000))

    # Sync samples 1 and 4: a 3-sample GOP cadence.
    stss = stss_box([1, 4])

    # IBBP-style reordering: only the anchor frames carry a presentation offset.
    ctts = ctts_box([(1, 2000), (2, 0), (1, 1000), (2, 0)], version=0)

    # Multi-chunk: entry 1 covers chunks 1-2 at 2 samples each, entry 2 covers
    # chunks 3-4 at 1 sample each. 2+2+1+1 = 6 samples.
    stsc_payload = struct.pack(">I", 2)
    stsc_payload += struct.pack(">III", 1, 2, 1)
    stsc_payload += struct.pack(">III", 3, 1, 1)
    stsc = make_full_box("stsc", 0, 0, stsc_payload)

    stsz_payload = struct.pack(">II", 0, 6) + struct.pack(">IIIIII", 100, 50, 40, 60, 30, 20)
    stsz = make_full_box("stsz", 0, 0, stsz_payload)

    # co64: chunk offsets past the 4 GiB boundary, which is why co64 exists.
    co64_payload = struct.pack(">I", 4)
    for offset in (0x1_0000_0000, 0x1_0000_0100, 0x1_0000_0200, 0x1_0000_0300):
        co64_payload += struct.pack(">Q", offset)
    co64 = make_full_box("co64", 0, 0, co64_payload)

    moov = wrap_stbl(stts + stss + ctts + stsc + stsz + co64)
    mdat = make_box("mdat", b"\xEE" * 16)
    data = make_ftyp() + moov + mdat
    return write_fixture("mp4_p5j_realistic_bframe_track.mp4", data)

def generate_count_size_mismatch() -> Path:
    """entry_count larger than the box actually carries.

    stss declares 100 entries but the box is sized for 2, and ctts declares 50 while
    carrying 1. The declared table overruns the box, so the rule must fault
    deterministically instead of reading neighbouring boxes as table entries.
    """
    stss = make_full_box(
        "stss", 0, 0, struct.pack(">I", 100) + struct.pack(">II", 1, 4)
    )
    ctts = make_full_box(
        "ctts", 0, 0, struct.pack(">I", 50) + struct.pack(">II", 1, 2000)
    )

    moov = wrap_stbl(stss + ctts)
    mdat = make_box("mdat", b"\x11" * 16)
    data = make_ftyp() + moov + mdat
    return write_fixture("mp4_p5j_count_size_mismatch.mp4", data)

def generate_oversized_table() -> Path:
    """A table whose declared entry_count would address far more bytes than exist.

    entry_count 0xFFFFFFFF implies a ~16 GiB stss table inside a 20-byte box; the
    declared size must be rejected against a resource bound, never allocated.
    """
    stss = make_full_box(
        "stss", 0, 0, struct.pack(">I", 0xFFFFFFFF) + struct.pack(">II", 1, 2)
    )

    moov = wrap_stbl(stss)
    mdat = make_box("mdat", b"\x22" * 16)
    data = make_ftyp() + moov + mdat
    return write_fixture("mp4_p5j_oversized_table.mp4", data)

def main():
    generate_sync_and_composition_v0()
    generate_composition_v1_negative()
    generate_stss_absent()
    generate_compact_sample_sizes()
    generate_unsupported_versions()
    generate_unsupported_field_size()
    generate_largesize_and_eof()
    generate_realistic_combined_track()
    generate_count_size_mismatch()
    generate_oversized_table()

if __name__ == "__main__":
    main()

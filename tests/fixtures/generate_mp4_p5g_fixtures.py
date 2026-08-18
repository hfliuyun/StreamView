#!/usr/bin/env python3
"""
Generate reproducible MP4 binary test fixtures for Task P5g:
- stts (time-to-sample with window pagination)
- stsc (sample-to-chunk with window pagination)
- stsz (sample size: uniform scalar vs variable window pagination)
- stco (32-bit chunk offset with window pagination)
- co64 (64-bit chunk offset with window pagination)
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

def generate_sample_tables_v0() -> Path:
    # stts: 2 entries
    stts_payload = struct.pack(">I", 2) + struct.pack(">II", 10, 1000) + struct.pack(">II", 5, 2000)
    stts = make_full_box("stts", 0, 0, stts_payload)

    # stsc: 2 entries
    stsc_payload = struct.pack(">I", 2) + struct.pack(">III", 1, 5, 1) + struct.pack(">III", 2, 10, 1)
    stsc = make_full_box("stsc", 0, 0, stsc_payload)

    # stsz: sample_size=0, 3 entries
    stsz_payload = struct.pack(">II", 0, 3) + struct.pack(">III", 100, 200, 300)
    stsz = make_full_box("stsz", 0, 0, stsz_payload)

    # stco: 2 entries (32-bit offsets)
    stco_payload = struct.pack(">I", 2) + struct.pack(">II", 1000, 2000)
    stco = make_full_box("stco", 0, 0, stco_payload)

    # co64: 2 entries (64-bit offsets)
    co64_payload = struct.pack(">I", 2) + struct.pack(">QQ", 0x100000000, 0x200000000)
    co64 = make_full_box("co64", 0, 0, co64_payload)

    stbl = make_box("stbl", stts + stsc + stsz + stco + co64)
    minf = make_box("minf", stbl)
    mdia = make_box("mdia", minf)
    trak = make_box("trak", mdia)
    moov = make_box("moov", trak)
    mdat = make_box("mdat", b"\xAA" * 16)

    data = make_ftyp() + moov + mdat
    out_path = FIXTURES_DIR / "mp4_p5g_sample_tables_v0.mp4"
    out_path.write_bytes(data)
    print(f"Generated {out_path} ({len(data)} bytes)")
    return out_path

def generate_stsz_uniform() -> Path:
    # stsz: uniform sample_size=1024, sample_count=50 (no entries table)
    stsz_payload = struct.pack(">II", 1024, 50)
    stsz = make_full_box("stsz", 0, 0, stsz_payload)

    stbl = make_box("stbl", stsz)
    minf = make_box("minf", stbl)
    mdia = make_box("mdia", minf)
    trak = make_box("trak", mdia)
    moov = make_box("moov", trak)
    mdat = make_box("mdat", b"\x00" * 16)

    data = make_ftyp() + moov + mdat
    out_path = FIXTURES_DIR / "mp4_p5g_stsz_uniform.mp4"
    out_path.write_bytes(data)
    print(f"Generated {out_path} ({len(data)} bytes)")
    return out_path

def generate_large_sample_table() -> Path:
    # 200 entries in stsz, stco, stts (fits within 4KB detection probe window)
    count = 200

    # stts: 200 entries of 8 bytes = 1600 bytes
    stts_entries = b"".join(struct.pack(">II", i + 1, 1000) for i in range(count))
    stts = make_full_box("stts", 0, 0, struct.pack(">I", count) + stts_entries)

    # stsz: 200 entries of 4 bytes = 800 bytes
    stsz_entries = b"".join(struct.pack(">I", 500 + i) for i in range(count))
    stsz = make_full_box("stsz", 0, 0, struct.pack(">II", 0, count) + stsz_entries)

    # stco: 200 entries of 4 bytes = 800 bytes
    stco_entries = b"".join(struct.pack(">I", 10000 + i * 100) for i in range(count))
    stco = make_full_box("stco", 0, 0, struct.pack(">I", count) + stco_entries)

    stbl = make_box("stbl", stts + stsz + stco)
    minf = make_box("minf", stbl)
    mdia = make_box("mdia", minf)
    trak = make_box("trak", mdia)
    moov = make_box("moov", trak)
    free = make_box("free", b"\x00" * 8)
    mdat = make_box("mdat", b"\x00" * 16)

    data = make_ftyp() + free + moov + mdat
    out_path = FIXTURES_DIR / "mp4_p5g_large_sample_table.mp4"
    out_path.write_bytes(data)
    print(f"Generated {out_path} ({len(data)} bytes)")
    return out_path

def generate_unsupported_stbl_versions() -> Path:
    # Version 1 on all sample table boxes (only version 0 is supported in ISO/IEC 14496-12)
    stts = make_full_box("stts", 1, 0, struct.pack(">I", 1) + struct.pack(">II", 1, 1000))
    stsc = make_full_box("stsc", 1, 0, struct.pack(">I", 1) + struct.pack(">III", 1, 1, 1))
    stsz = make_full_box("stsz", 1, 0, struct.pack(">II", 0, 1) + struct.pack(">I", 100))
    stco = make_full_box("stco", 1, 0, struct.pack(">I", 1) + struct.pack(">I", 1000))
    co64 = make_full_box("co64", 1, 0, struct.pack(">I", 1) + struct.pack(">Q", 1000))

    stbl = make_box("stbl", stts + stsc + stsz + stco + co64)
    minf = make_box("minf", stbl)
    mdia = make_box("mdia", minf)
    trak = make_box("trak", mdia)
    moov = make_box("moov", trak)
    mdat = make_box("mdat", b"\x00" * 16)

    data = make_ftyp() + moov + mdat
    out_path = FIXTURES_DIR / "mp4_p5g_unsupported_stbl_versions.mp4"
    out_path.write_bytes(data)
    print(f"Generated {out_path} ({len(data)} bytes)")
    return out_path

def generate_largesize_and_eof_sample_tables() -> Path:
    # Exercise the shared sample-table box framing branches without hand-written offsets.
    stts_payload = struct.pack(">I", 2) + struct.pack(">II", 7, 480) + struct.pack(">II", 3, 960)
    stts = make_large_full_box("stts", 0, 0, stts_payload)

    # size == 0 must be terminal within the stbl payload, so keep stsz last.
    stsz_payload = struct.pack(">II", 0, 2) + struct.pack(">II", 111, 222)
    stsz = make_eof_full_box("stsz", 0, 0, stsz_payload)

    stbl = make_box("stbl", stts + stsz)
    minf = make_box("minf", stbl)
    mdia = make_box("mdia", minf)
    trak = make_box("trak", mdia)
    moov = make_box("moov", trak)
    mdat = make_box("mdat", b"\x00" * 16)

    data = make_ftyp() + moov + mdat
    out_path = FIXTURES_DIR / "mp4_p5g_largesize_and_eof_tables.mp4"
    out_path.write_bytes(data)
    print(f"Generated {out_path} ({len(data)} bytes)")
    return out_path

def main():
    generate_sample_tables_v0()
    generate_stsz_uniform()
    generate_large_sample_table()
    generate_unsupported_stbl_versions()
    generate_largesize_and_eof_sample_tables()

if __name__ == "__main__":
    main()

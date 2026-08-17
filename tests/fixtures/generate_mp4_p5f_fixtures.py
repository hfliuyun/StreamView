#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generator for MP4 P5f test fixtures:
- mp4_p5f_full_hierarchy_v0.mp4
- mp4_p5f_time_headers_v1.mp4
"""

import os
import struct

FIXTURES_DIR = os.path.dirname(os.path.abspath(__file__))


def make_box(fourcc_str: str, payload_bytes: bytes) -> bytes:
    size = len(payload_bytes) + 8
    return struct.pack(">I4s", size, fourcc_str.encode("ascii")) + payload_bytes


def make_full_box(fourcc_str: str, version: int, flags: int, payload_bytes: bytes) -> bytes:
    v_flags = ((version & 0xFF) << 24) | (flags & 0x00FFFFFF)
    return make_box(fourcc_str, struct.pack(">I", v_flags) + payload_bytes)


def make_ftyp() -> bytes:
    payload = struct.pack(
        ">4sI4s4s4s4s",
        b"isom",
        512,
        b"isom",
        b"iso2",
        b"avc1",
        b"mp41",
    )
    return make_box("ftyp", payload)


def make_mvhd_v0(creation_time: int, modification_time: int, timescale: int, duration: int, next_track_id: int) -> bytes:
    payload = struct.pack(">IIII", creation_time, modification_time, timescale, duration)
    payload += struct.pack(">IH", 0x00010000, 0x0100)  # rate 1.0, volume 1.0
    payload += struct.pack(">HQ", 0, 0)  # reserved (16-bit) + reserved_2 (64-bit)
    matrix = [0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000]
    payload += struct.pack(">9I", *matrix)
    pre_defined = [0, 0, 0, 0, 0, 0]
    payload += struct.pack(">6I", *pre_defined)
    payload += struct.pack(">I", next_track_id)
    return make_full_box("mvhd", 0, 0, payload)


def make_mvhd_v1(creation_time: int, modification_time: int, timescale: int, duration: int, next_track_id: int) -> bytes:
    payload = struct.pack(">QQIQ", creation_time, modification_time, timescale, duration)
    payload += struct.pack(">IH", 0x00010000, 0x0100)
    payload += struct.pack(">HQ", 0, 0)
    matrix = [0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000]
    payload += struct.pack(">9I", *matrix)
    pre_defined = [0, 0, 0, 0, 0, 0]
    payload += struct.pack(">6I", *pre_defined)
    payload += struct.pack(">I", next_track_id)
    return make_full_box("mvhd", 1, 0, payload)


def make_tkhd_v0(creation_time: int, modification_time: int, track_id: int, duration: int, width: int, height: int) -> bytes:
    flags = 0x000007
    payload = struct.pack(">IIII", creation_time, modification_time, track_id, 0)
    payload += struct.pack(">IQ", duration, 0)
    payload += struct.pack(">hhhh", 0, 0, 0x0100, 0)
    matrix = [0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000]
    payload += struct.pack(">9I", *matrix)
    payload += struct.pack(">II", width, height)
    return make_full_box("tkhd", 0, flags, payload)


def make_tkhd_v1(creation_time: int, modification_time: int, track_id: int, duration: int, width: int, height: int) -> bytes:
    flags = 0x000007
    payload = struct.pack(">QQIIQ", creation_time, modification_time, track_id, 0, duration)
    payload += struct.pack(">Q", 0)
    payload += struct.pack(">hhhh", 0, 0, 0x0100, 0)
    matrix = [0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000]
    payload += struct.pack(">9I", *matrix)
    payload += struct.pack(">II", width, height)
    return make_full_box("tkhd", 1, flags, payload)


def make_elst_v0(entries: list) -> bytes:
    payload = struct.pack(">I", len(entries))
    for seg_dur, media_time, rate_int, rate_frac in entries:
        payload += struct.pack(">iihh", seg_dur, media_time, rate_int, rate_frac)
    return make_full_box("elst", 0, 0, payload)


def make_elst_v1(entries: list) -> bytes:
    payload = struct.pack(">I", len(entries))
    for seg_dur, media_time, rate_int, rate_frac in entries:
        payload += struct.pack(">qqhh", seg_dur, media_time, rate_int, rate_frac)
    return make_full_box("elst", 1, 0, payload)


def make_mdhd_v0(creation_time: int, modification_time: int, timescale: int, duration: int, language: int) -> bytes:
    payload = struct.pack(">IIII", creation_time, modification_time, timescale, duration)
    payload += struct.pack(">HH", language & 0x7FFF, 0)
    return make_full_box("mdhd", 0, 0, payload)


def make_mdhd_v1(creation_time: int, modification_time: int, timescale: int, duration: int, language: int) -> bytes:
    payload = struct.pack(">QQIQ", creation_time, modification_time, timescale, duration)
    payload += struct.pack(">HH", language & 0x7FFF, 0)
    return make_full_box("mdhd", 1, 0, payload)


def make_hdlr(handler_type_str: str, name_str: str) -> bytes:
    payload = struct.pack(">I4s3I", 0, handler_type_str.encode("ascii"), 0, 0, 0)
    payload += name_str.encode("utf-8") + b"\x00"
    return make_full_box("hdlr", 0, 0, payload)


def generate_full_hierarchy_v0() -> None:
    path = os.path.join(FIXTURES_DIR, "mp4_p5f_full_hierarchy_v0.mp4")

    # 5. stbl container with dummy stsd and stts
    stsd = make_box("stsd", b"\x00\x00\x00\x00\x00\x00\x00\x00")
    stts = make_box("stts", b"\x00\x00\x00\x00\x00\x00\x00\x00")
    stbl = make_box("stbl", stsd + stts)

    # 4. minf container
    minf = make_box("minf", stbl)

    # 3. mdia container
    mdhd = make_mdhd_v0(1000, 1001, 30000, 150000, 0x15C7)
    hdlr = make_hdlr("vide", "VideoHandler")
    mdia = make_box("mdia", mdhd + hdlr + minf)

    # edts container in trak
    elst = make_elst_v0([(5000, 0, 1, 0)])
    edts = make_box("edts", elst)

    # 2. trak container
    tkhd = make_tkhd_v0(1000, 1001, 1, 5000, 0x07800000, 0x04380000)
    trak = make_box("trak", tkhd + edts + mdia)

    # 1. moov container
    mvhd = make_mvhd_v0(1000, 1001, 1000, 5000, 2)
    moov = make_box("moov", mvhd + trak)

    ftyp = make_ftyp()
    mdat = make_box("mdat", bytes(range(16)))

    with open(path, "wb") as f:
        f.write(ftyp + moov + mdat)

    print(f"Generated {path} ({os.path.getsize(path)} bytes)")


def generate_time_headers_v1() -> None:
    path = os.path.join(FIXTURES_DIR, "mp4_p5f_time_headers_v1.mp4")

    mdhd = make_mdhd_v1(0x100000000, 0x100000001, 48000, 0x200000000, 0x15C7)
    hdlr = make_hdlr("soun", "SoundHandler")
    mdia = make_box("mdia", mdhd + hdlr)

    elst = make_elst_v1([
        (0x100000000, -1, 1, 0),
        (0x100000000, 0, 1, 0),
    ])
    edts = make_box("edts", elst)

    tkhd = make_tkhd_v1(0x100000000, 0x100000001, 2, 0x200000000, 0, 0)
    trak = make_box("trak", tkhd + edts + mdia)

    mvhd = make_mvhd_v1(0x100000000, 0x100000001, 48000, 0x200000000, 3)
    moov = make_box("moov", mvhd + trak)

    ftyp = make_ftyp()
    mdat = make_box("mdat", bytes(range(16)))

    with open(path, "wb") as f:
        f.write(ftyp + moov + mdat)

    print(f"Generated {path} ({os.path.getsize(path)} bytes)")


if __name__ == "__main__":
    os.makedirs(FIXTURES_DIR, exist_ok=True)
    generate_full_hierarchy_v0()
    generate_time_headers_v1()

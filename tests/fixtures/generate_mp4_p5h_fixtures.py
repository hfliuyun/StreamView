#!/usr/bin/env python3
"""
Generate reproducible MP4 binary test fixtures for Task P5h:
- stsd (Sample Description Box)
- avc1 (Visual Sample Entry)
- mp4a (Audio Sample Entry)
- avcC (AVC Configuration Box with SPS / PPS @target_format("video.h264.nal"))
- esds (Elementary Stream Descriptor Box with AudioSpecificConfig @target_format("audio.aac.asc"))
- Error, truncation, and largesize variants
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

def make_ftyp() -> bytes:
    payload = b"isom" + struct.pack(">I", 512) + b"isom"
    return make_box("ftyp", payload)

def make_avc1_entry(children: bytes = b"", width: int = 1920, height: int = 1080) -> bytes:
    # 78 bytes VisualSampleEntry header
    # reserved_entry: 6 bytes (48 bits)
    # data_reference_index: 2 bytes (16 bits)
    # pre_defined: 2 bytes (0)
    # reserved_1: 2 bytes (0)
    # pre_defined_1: 3 * 4 = 12 bytes (0)
    # width: 2 bytes
    # height: 2 bytes
    # horizresolution: 4 bytes (0x00480000)
    # vertresolution: 4 bytes (0x00480000)
    # reserved_2: 4 bytes (0)
    # frame_count: 2 bytes (1)
    # compressorname: 32 bytes (4 * 8 bytes)
    # depth: 2 bytes (0x0018)
    # pre_defined_2: 2 bytes (-1 -> 0xFFFF)
    hdr = (
        b"\x00" * 6 +
        struct.pack(">H", 1) +
        struct.pack(">HH", 0, 0) +
        struct.pack(">III", 0, 0, 0) +
        struct.pack(">HH", width, height) +
        struct.pack(">II", 0x00480000, 0x00480000) +
        struct.pack(">I", 0) +
        struct.pack(">H", 1) +
        b"\x0aAVC Coding\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" +
        struct.pack(">h", 0x0018) +
        struct.pack(">h", -1)
    )
    assert len(hdr) == 78, f"Expected 78 bytes for VisualSampleEntry, got {len(hdr)}"
    return make_box("avc1", hdr + children)

def make_mp4a_entry(children: bytes = b"", channel_count: int = 2, sample_rate: int = 44100) -> bytes:
    # 28 bytes AudioSampleEntry header
    # reserved_entry: 6 bytes (48 bits)
    # data_reference_index: 2 bytes (16 bits)
    # reserved_1: 8 bytes (64 bits)
    # channelcount: 2 bytes (16 bits)
    # samplesize: 2 bytes (16 bits = 16)
    # pre_defined: 2 bytes (16 bits = 0)
    # reserved_2: 2 bytes (16 bits = 0)
    # samplerate: 4 bytes (32 bits = sample_rate << 16)
    hdr = (
        b"\x00" * 6 +
        struct.pack(">H", 1) +
        b"\x00" * 8 +
        struct.pack(">HH", channel_count, 16) +
        struct.pack(">HH", 0, 0) +
        struct.pack(">I", sample_rate << 16)
    )
    assert len(hdr) == 28, f"Expected 28 bytes for AudioSampleEntry, got {len(hdr)}"
    return make_box("mp4a", hdr + children)

def make_avcC_box(sps_list: list[bytes], pps_list: list[bytes], config_version: int = 1) -> bytes:
    # configurationVersion: 1 byte
    # avcProfileIndication: 1 byte
    # profile_compatibility: 1 byte
    # avcLevelIndication: 1 byte
    # reserved_6bits (111111b) | lengthSizeMinusOne (2 bits = 3): 1 byte -> 0xFF
    # reserved_3bits (111b) | numOfSequenceParameterSets (5 bits): 1 byte
    num_sps = len(sps_list) & 0x1F
    b5 = 0xE0 | num_sps

    sps_payload = b""
    for sps in sps_list:
        sps_payload += struct.pack(">H", len(sps)) + sps

    num_pps = len(pps_list) & 0xFF
    pps_payload = struct.pack(">B", num_pps)
    for pps in pps_list:
        pps_payload += struct.pack(">H", len(pps)) + pps

    profile = sps_list[0][1] if sps_list and len(sps_list[0]) > 1 else 0x64
    compat = sps_list[0][2] if sps_list and len(sps_list[0]) > 2 else 0x00
    level = sps_list[0][3] if sps_list and len(sps_list[0]) > 3 else 0x29

    payload = struct.pack(">BBBBBB", config_version, profile, compat, level, 0xFF, b5) + sps_payload + pps_payload
    return make_box("avcC", payload)

def make_esds_box(asc_bytes: bytes, version: int = 0, len_bytes_count: int = 1) -> bytes:
    # DecoderSpecificInfo tag 0x05
    if len_bytes_count == 1:
        dsi_len = struct.pack(">B", len(asc_bytes))
    elif len_bytes_count == 2:
        dsi_len = struct.pack(">BB", 0x80 | 0, len(asc_bytes))
    elif len_bytes_count == 3:
        dsi_len = struct.pack(">BBB", 0x80 | 0, 0x80 | 0, len(asc_bytes))
    elif len_bytes_count == 4:
        dsi_len = struct.pack(">BBBB", 0x80 | 0, 0x80 | 0, 0x80 | 0, len(asc_bytes))
    else:
        raise ValueError("len_bytes_count must be 1..4")

    dsi = b"\x05" + dsi_len + asc_bytes

    # DecoderConfigDescriptor tag 0x04
    # objectTypeIndication: 0x40 (Audio ISO/IEC 14496-3 AAC)
    # streamType: 0x05 (AudioStream) << 2 | upStream (0) << 1 | reserved (1) = 0x15
    # bufferSizeDB: 3 bytes (0x018000)
    # maxBitrate: 4 bytes (128000)
    # avgBitrate: 4 bytes (128000)
    dc_body = struct.pack(">BB", 0x40, (0x05 << 2) | 1) + struct.pack(">I", 0x018000)[1:] + struct.pack(">II", 128000, 128000) + dsi
    if len_bytes_count == 1:
        dc_len = struct.pack(">B", len(dc_body))
    elif len_bytes_count == 2:
        dc_len = struct.pack(">BB", 0x80 | 0, len(dc_body))
    elif len_bytes_count == 3:
        dc_len = struct.pack(">BBB", 0x80 | 0, 0x80 | 0, len(dc_body))
    elif len_bytes_count == 4:
        dc_len = struct.pack(">BBBB", 0x80 | 0, 0x80 | 0, 0x80 | 0, len(dc_body))

    dc = b"\x04" + dc_len + dc_body

    # ES_Descriptor tag 0x03
    # ES_ID: 2 bytes (1)
    # flags: 1 byte (0)
    es_body = struct.pack(">HB", 1, 0) + dc
    if len_bytes_count == 1:
        es_len = struct.pack(">B", len(es_body))
    elif len_bytes_count == 2:
        es_len = struct.pack(">BB", 0x80 | 0, len(es_body))
    elif len_bytes_count == 3:
        es_len = struct.pack(">BBB", 0x80 | 0, 0x80 | 0, len(es_body))
    elif len_bytes_count == 4:
        es_len = struct.pack(">BBBB", 0x80 | 0, 0x80 | 0, 0x80 | 0, len(es_body))

    es = b"\x03" + es_len + es_body

    return make_full_box("esds", version, 0, es)

def generate_avc1_avcC() -> Path:
    # 25-byte valid SPS NAL unit (profile 100, level 41)
    sps = b"\x67\x64\x00\x29\xac\x2b\x40\x3c\x01\x13\xf2\xe0\x22\x00\x00\x03\x00\x02\x00\x00\x03\x00\x79\x1e\x30"
    # 4-byte valid PPS NAL unit
    pps = b"\x68\xee\x3c\x80"

    avcC = make_avcC_box([sps], [pps])
    avc1 = make_avc1_entry(avcC, 1920, 1080)
    stsd = make_full_box("stsd", 0, 0, struct.pack(">I", 1) + avc1)

    stbl = make_box("stbl", stsd)
    minf = make_box("minf", stbl)
    mdia = make_box("mdia", minf)
    trak = make_box("trak", mdia)
    moov = make_box("moov", trak)
    mdat = make_box("mdat", b"\xAA" * 32)

    data = make_ftyp() + moov + mdat
    out_path = FIXTURES_DIR / "mp4_p5h_avc1_avcC.mp4"
    out_path.write_bytes(data)
    print(f"Generated {out_path} ({len(data)} bytes)")
    return out_path

def generate_mp4a_esds() -> Path:
    # 2-byte AudioSpecificConfig: AAC-LC (2), 44.1kHz (4), Stereo (2) -> 0x12 0x10
    asc = b"\x12\x10"

    esds = make_esds_box(asc)
    mp4a = make_mp4a_entry(esds, 2, 44100)
    stsd = make_full_box("stsd", 0, 0, struct.pack(">I", 1) + mp4a)

    stbl = make_box("stbl", stsd)
    minf = make_box("minf", stbl)
    mdia = make_box("mdia", minf)
    trak = make_box("trak", mdia)
    moov = make_box("moov", trak)
    mdat = make_box("mdat", b"\xBB" * 32)

    data = make_ftyp() + moov + mdat
    out_path = FIXTURES_DIR / "mp4_p5h_mp4a_esds.mp4"
    out_path.write_bytes(data)
    print(f"Generated {out_path} ({len(data)} bytes)")
    return out_path

def generate_esds_multibyte_len() -> Path:
    # 2-byte ASC with 3-byte 0x80 continuation encoding for tags
    asc = b"\x12\x10"

    esds = make_esds_box(asc, version=0, len_bytes_count=3)
    mp4a = make_mp4a_entry(esds, 2, 44100)
    stsd = make_full_box("stsd", 0, 0, struct.pack(">I", 1) + mp4a)

    stbl = make_box("stbl", stsd)
    minf = make_box("minf", stbl)
    mdia = make_box("mdia", minf)
    trak = make_box("trak", mdia)
    moov = make_box("moov", trak)
    mdat = make_box("mdat", b"\xCC" * 32)

    data = make_ftyp() + moov + mdat
    out_path = FIXTURES_DIR / "mp4_p5h_esds_multibyte_len.mp4"
    out_path.write_bytes(data)
    print(f"Generated {out_path} ({len(data)} bytes)")
    return out_path

def generate_unsupported_versions() -> Path:
    sps = b"\x67\x64\x00\x29\xac\x2b\x40\x3c\x01\x13\xf2\xe0\x22\x00\x00\x03\x00\x02\x00\x00\x03\x00\x79\x1e\x30"
    pps = b"\x68\xee\x3c\x80"
    asc = b"\x12\x10"

    # Track 1: unsupported stsd version (1)
    avc1_1 = make_avc1_entry(make_avcC_box([sps], [pps]))
    stsd_v1 = make_full_box("stsd", 1, 0, struct.pack(">I", 1) + avc1_1)
    trak1 = make_box("trak", make_box("mdia", make_box("minf", make_box("stbl", stsd_v1))))

    # Track 2: unsupported avcC configurationVersion (2)
    avcC_v2 = make_avcC_box([sps], [pps], config_version=2)
    avc1_2 = make_avc1_entry(avcC_v2)
    stsd_v0_2 = make_full_box("stsd", 0, 0, struct.pack(">I", 1) + avc1_2)
    trak2 = make_box("trak", make_box("mdia", make_box("minf", make_box("stbl", stsd_v0_2))))

    # Track 3: unsupported esds version (1)
    esds_v1 = make_esds_box(asc, version=1)
    mp4a_3 = make_mp4a_entry(esds_v1)
    stsd_v0_3 = make_full_box("stsd", 0, 0, struct.pack(">I", 1) + mp4a_3)
    trak3 = make_box("trak", make_box("mdia", make_box("minf", make_box("stbl", stsd_v0_3))))

    moov = make_box("moov", trak1 + trak2 + trak3)
    data = make_ftyp() + moov
    out_path = FIXTURES_DIR / "mp4_p5h_unsupported_versions.mp4"
    out_path.write_bytes(data)
    print(f"Generated {out_path} ({len(data)} bytes)")
    return out_path

def generate_largesize_boxes() -> Path:
    sps = b"\x67\x64\x00\x29\xac\x2b\x40\x3c\x01\x13\xf2\xe0\x22\x00\x00\x03\x00\x02\x00\x00\x03\x00\x79\x1e\x30"
    pps = b"\x68\xee\x3c\x80"
    asc = b"\x12\x10"

    avcC_payload = struct.pack(">BBBBBB", 1, 0x64, 0x00, 0x29, 0xFF, 0xE1) + struct.pack(">H", len(sps)) + sps + struct.pack(">B", 1) + struct.pack(">H", len(pps)) + pps
    avcC_large = make_large_box("avcC", avcC_payload)

    avc1_payload = (
        b"\x00" * 6 +
        struct.pack(">H", 1) +
        struct.pack(">HH", 0, 0) +
        struct.pack(">III", 0, 0, 0) +
        struct.pack(">HH", 1280, 720) +
        struct.pack(">II", 0x00480000, 0x00480000) +
        struct.pack(">I", 0) +
        struct.pack(">H", 1) +
        b"\x0aAVC Coding\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" +
        struct.pack(">h", 0x0018) +
        struct.pack(">h", -1) +
        avcC_large
    )
    avc1_large = make_large_box("avc1", avc1_payload)

    stsd_payload = struct.pack(">I", 0) + struct.pack(">I", 1) + avc1_large
    stsd_large = make_large_box("stsd", stsd_payload)

    moov = make_box("moov", make_box("trak", make_box("mdia", make_box("minf", make_box("stbl", stsd_large)))))
    data = make_ftyp() + moov
    out_path = FIXTURES_DIR / "mp4_p5h_largesize_boxes.mp4"
    out_path.write_bytes(data)
    print(f"Generated {out_path} ({len(data)} bytes)")
    return out_path

def generate_truncated_avcC() -> Path:
    sps = b"\x67\x64\x00\x29\xac\x2b\x40\x3c\x01\x13\xf2\xe0\x22\x00\x00\x03\x00\x02\x00\x00\x03\x00\x79\x1e\x30"
    pps = b"\x68\xee\x3c\x80"
    avcC = make_avcC_box([sps], [pps])

    # Truncate avcC by 10 bytes from end
    truncated_avcC = avcC[:-10]
    # Keep the original size in avc1 to simulate premature stream cut
    avc1 = make_avc1_entry(truncated_avcC, 1920, 1080)
    stsd = make_full_box("stsd", 0, 0, struct.pack(">I", 1) + avc1)

    moov = make_box("moov", make_box("trak", make_box("mdia", make_box("minf", make_box("stbl", stsd)))))
    data = make_ftyp() + moov
    out_path = FIXTURES_DIR / "mp4_p5h_truncated_avcC.mp4"
    out_path.write_bytes(data)
    print(f"Generated {out_path} ({len(data)} bytes)")
    return out_path

if __name__ == "__main__":
    generate_avc1_avcC()
    generate_mp4a_esds()
    generate_esds_multibyte_len()
    generate_unsupported_versions()
    generate_largesize_boxes()
    generate_truncated_avcC()

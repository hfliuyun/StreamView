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
from typing import Optional

FIXTURES_DIR = Path(__file__).resolve().parent


def make_box(fourcc_str: str, payload: bytes) -> bytes:
    return struct.pack(">I4s", len(payload) + 8, fourcc_str.encode("ascii")) + payload

def make_large_box(fourcc_str: str, payload: bytes) -> bytes:
    return struct.pack(">I4sQ", 1, fourcc_str.encode("ascii"), len(payload) + 16) + payload

def make_eof_box(fourcc_str: str, payload: bytes) -> bytes:
    return struct.pack(">I4s", 0, fourcc_str.encode("ascii")) + payload


def wrap_box(fourcc_str: str, payload: bytes, framing: str = "standard") -> bytes:
    if framing == "standard":
        return make_box(fourcc_str, payload)
    if framing == "large":
        return make_large_box(fourcc_str, payload)
    if framing == "eof":
        return make_eof_box(fourcc_str, payload)
    raise ValueError("framing must be standard, large, or eof")

def make_full_box(fourcc_str: str, version: int, flags: int, payload: bytes) -> bytes:
    v_flags = ((version & 0xFF) << 24) | (flags & 0x00FFFFFF)
    return make_box(fourcc_str, struct.pack(">I", v_flags) + payload)

def make_large_full_box(fourcc_str: str, version: int, flags: int, payload: bytes) -> bytes:
    v_flags = ((version & 0xFF) << 24) | (flags & 0x00FFFFFF)
    return make_large_box(fourcc_str, struct.pack(">I", v_flags) + payload)

def make_ftyp() -> bytes:
    payload = b"isom" + struct.pack(">I", 512) + b"isom"
    return make_box("ftyp", payload)

def make_avc1_entry(
    children: bytes = b"",
    width: int = 1920,
    height: int = 1080,
    framing: str = "standard",
) -> bytes:
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
    return wrap_box("avc1", hdr + children, framing)

def make_mp4a_entry(
    children: bytes = b"",
    channel_count: int = 2,
    sample_rate: int = 44100,
    framing: str = "standard",
) -> bytes:
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
    return wrap_box("mp4a", hdr + children, framing)


def make_avcC_payload(
    sps_list: list[bytes],
    pps_list: list[bytes],
    config_version: int = 1,
    sps_ext_list: Optional[list[bytes]] = None,
) -> bytes:
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

    payload = (
        struct.pack(">BBBBBB", config_version, profile, compat, level, 0xFF, b5)
        + sps_payload
        + pps_payload
    )
    if config_version == 1 and profile in (100, 110, 122, 144):
        extensions = sps_ext_list or []
        payload += struct.pack(">BBBB", 0xFD, 0xF8, 0xF8, len(extensions))
        for extension in extensions:
            payload += struct.pack(">H", len(extension)) + extension
    return payload


def make_avcC_box(
    sps_list: list[bytes],
    pps_list: list[bytes],
    config_version: int = 1,
    sps_ext_list: Optional[list[bytes]] = None,
    framing: str = "standard",
) -> bytes:
    return wrap_box(
        "avcC",
        make_avcC_payload(sps_list, pps_list, config_version, sps_ext_list),
        framing,
    )


def encode_descriptor_length(length: int, byte_count: int) -> bytes:
    if byte_count < 1 or byte_count > 4:
        raise ValueError("byte_count must be 1..4")
    if length < 0 or length >= (1 << (7 * byte_count)):
        raise ValueError("length does not fit descriptor encoding")
    encoded = bytearray()
    for index in range(byte_count):
        shift = 7 * (byte_count - index - 1)
        value = (length >> shift) & 0x7F
        encoded.append(value | (0x80 if index + 1 < byte_count else 0))
    return bytes(encoded)


def make_esds_payload(
    asc_bytes: bytes,
    version: int = 0,
    len_bytes_count: int = 1,
    depends_on_es_id: Optional[int] = None,
    url: Optional[bytes] = None,
    ocr_es_id: Optional[int] = None,
) -> bytes:
    # DecoderSpecificInfo tag 0x05
    dsi_len = encode_descriptor_length(len(asc_bytes), len_bytes_count)
    dsi = b"\x05" + dsi_len + asc_bytes

    # DecoderConfigDescriptor tag 0x04
    # objectTypeIndication: 0x40 (Audio ISO/IEC 14496-3 AAC)
    # streamType: 0x05 (AudioStream) << 2 | upStream (0) << 1 | reserved (1) = 0x15
    # bufferSizeDB: 3 bytes (0x018000)
    # maxBitrate: 4 bytes (128000)
    # avgBitrate: 4 bytes (128000)
    dc_body = struct.pack(">BB", 0x40, (0x05 << 2) | 1) + struct.pack(">I", 0x018000)[1:] + struct.pack(">II", 128000, 128000) + dsi
    dc_len = encode_descriptor_length(len(dc_body), len_bytes_count)
    dc = b"\x04" + dc_len + dc_body

    # ES_Descriptor tag 0x03
    # ES_ID: 2 bytes (1)
    flags = (
        (0x80 if depends_on_es_id is not None else 0)
        | (0x40 if url is not None else 0)
        | (0x20 if ocr_es_id is not None else 0)
    )
    optional_fields = b""
    if depends_on_es_id is not None:
        optional_fields += struct.pack(">H", depends_on_es_id)
    if url is not None:
        if len(url) > 255:
            raise ValueError("URL must fit one-byte length")
        optional_fields += struct.pack(">B", len(url)) + url
    if ocr_es_id is not None:
        optional_fields += struct.pack(">H", ocr_es_id)
    es_body = struct.pack(">HB", 1, flags) + optional_fields + dc
    es_len = encode_descriptor_length(len(es_body), len_bytes_count)
    es = b"\x03" + es_len + es_body
    return struct.pack(">I", (version & 0xFF) << 24) + es


def make_esds_box(
    asc_bytes: bytes,
    version: int = 0,
    len_bytes_count: int = 1,
    depends_on_es_id: Optional[int] = None,
    url: Optional[bytes] = None,
    ocr_es_id: Optional[int] = None,
    framing: str = "standard",
) -> bytes:
    payload = make_esds_payload(
        asc_bytes,
        version,
        len_bytes_count,
        depends_on_es_id,
        url,
        ocr_es_id,
    )
    return wrap_box("esds", payload, framing)


def make_stsd(entries: list[bytes], version: int = 0, framing: str = "standard") -> bytes:
    payload = struct.pack(">II", (version & 0xFF) << 24, len(entries)) + b"".join(entries)
    return wrap_box("stsd", payload, framing)

def generate_avc1_avcC() -> Path:
    # 25-byte valid SPS NAL unit (profile 100, level 41)
    sps = b"\x67\x64\x00\x29\xac\x2b\x40\x3c\x01\x13\xf2\xe0\x22\x00\x00\x03\x00\x02\x00\x00\x03\x00\x79\x1e\x30"
    # 4-byte valid PPS NAL unit
    pps = b"\x68\xee\x3c\x80"
    # 3-byte High-profile SPS extension NAL unit
    sps_ext = b"\x6d\x00\x01"

    avcC = make_avcC_box([sps], [pps], sps_ext_list=[sps_ext])
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
    # Four sample entries exercise every legal 1..4-byte descriptor length form.
    asc = b"\x12\x10"
    entries = []
    for byte_count in range(1, 5):
        kwargs = {}
        if byte_count == 4:
            kwargs = {
                "depends_on_es_id": 7,
                "url": b"codec-config",
                "ocr_es_id": 9,
            }
        esds = make_esds_box(
            asc,
            version=0,
            len_bytes_count=byte_count,
            **kwargs,
        )
        entries.append(make_mp4a_entry(esds, 2, 44100))
    stsd = make_stsd(entries)

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

    avcC_payload = make_avcC_payload([sps], [pps])
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

    video_track = make_box(
        "trak", make_box("mdia", make_box("minf", make_box("stbl", stsd_large)))
    )
    audio_esds_large = make_esds_box(b"\x12\x10", framing="large")
    audio_mp4a_large = make_mp4a_entry(audio_esds_large, framing="large")
    audio_stsd_large = make_stsd([audio_mp4a_large], framing="large")
    audio_track = make_box(
        "trak", make_box("mdia", make_box("minf", make_box("stbl", audio_stsd_large)))
    )
    moov = make_box("moov", video_track + audio_track)
    data = make_ftyp() + moov
    out_path = FIXTURES_DIR / "mp4_p5h_largesize_boxes.mp4"
    out_path.write_bytes(data)
    print(f"Generated {out_path} ({len(data)} bytes)")
    return out_path


def generate_size_zero_boxes() -> Path:
    sps = b"\x67\x64\x00\x29\xac\x2b\x40\x3c\x01\x13\xf2\xe0\x22\x00\x00\x03\x00\x02\x00\x00\x03\x00\x79\x1e\x30"
    pps = b"\x68\xee\x3c\x80"
    asc = b"\x12\x10"

    avcC = make_avcC_box([sps], [pps], framing="eof")
    avc1 = make_avc1_entry(avcC, framing="eof")
    video_stsd = make_stsd([avc1], framing="eof")
    video_track = make_box(
        "trak",
        make_box("mdia", make_box("minf", make_box("stbl", video_stsd))),
    )

    esds = make_esds_box(asc, framing="eof")
    mp4a = make_mp4a_entry(esds, framing="eof")
    audio_stsd = make_stsd([mp4a], framing="eof")
    audio_track = make_box(
        "trak",
        make_box("mdia", make_box("minf", make_box("stbl", audio_stsd))),
    )

    data = make_ftyp() + make_box("moov", video_track + audio_track)
    out_path = FIXTURES_DIR / "mp4_p5h_size0_boxes.mp4"
    out_path.write_bytes(data)
    print(f"Generated {out_path} ({len(data)} bytes)")
    return out_path


def make_invalid_continuation_esds(asc: bytes, target: str) -> bytes:
    invalid_dsi = b"\x05\x80\x80\x80\x80\x02" + asc
    dc_body = (
        struct.pack(">BB", 0x40, (0x05 << 2) | 1)
        + struct.pack(">I", 0x018000)[1:]
        + struct.pack(">II", 128000, 128000)
        + invalid_dsi
    )
    dc_length = (
        b"\x80\x80\x80\x80\x00"
        if target == "dc"
        else encode_descriptor_length(len(dc_body), 1)
    )
    dc = b"\x04" + dc_length + dc_body
    es_body = struct.pack(">HB", 1, 0) + dc
    es_length = (
        b"\x80\x80\x80\x80\x00"
        if target == "es"
        else encode_descriptor_length(len(es_body), 1)
    )
    if target != "dsi":
        valid_dsi = b"\x05\x02\x12\x10"
        dc_body = (
            struct.pack(">BB", 0x40, (0x05 << 2) | 1)
            + struct.pack(">I", 0x018000)[1:]
            + struct.pack(">II", 128000, 128000)
            + valid_dsi
        )
        dc_length = (
            b"\x80\x80\x80\x80\x00"
            if target == "dc"
            else encode_descriptor_length(len(dc_body), 1)
        )
        dc = b"\x04" + dc_length + dc_body
        es_body = struct.pack(">HB", 1, 0) + dc
        es_length = (
            b"\x80\x80\x80\x80\x00"
            if target == "es"
            else encode_descriptor_length(len(es_body), 1)
        )
    es = b"\x03" + es_length + es_body
    return make_box("esds", struct.pack(">I", 0) + es)


def generate_invalid_descriptors() -> Path:
    asc = b"\x12\x10"
    invalid_entries = [
        make_mp4a_entry(make_invalid_continuation_esds(asc, "es")),
        make_mp4a_entry(make_invalid_continuation_esds(asc, "dc")),
        make_mp4a_entry(make_invalid_continuation_esds(asc, "dsi")),
    ]

    invalid_tag_payload = bytearray(make_esds_payload(asc))
    invalid_tag_payload[4] = 0x04
    invalid_entries.append(make_mp4a_entry(make_box("esds", bytes(invalid_tag_payload))))

    stsd = make_stsd(invalid_entries)
    data = make_ftyp() + make_box(
        "moov",
        make_box("trak", make_box("mdia", make_box("minf", make_box("stbl", stsd)))),
    )
    out_path = FIXTURES_DIR / "mp4_p5h_invalid_descriptors.mp4"
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
    generate_size_zero_boxes()
    generate_invalid_descriptors()

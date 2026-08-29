#!/usr/bin/env python3
"""
Generate reproducible MP4 binary test fixtures for Task P5j-4:
`AnalysisSession` sample navigation (`tracks`, `samplesForTrack`, `enterSample`).

The P5j-3b fixtures already carry complete, mutually consistent sample tables,
but their mdat holds one repeated filler byte per sample. That is exactly right
for proving a descriptor's span points at the correct bytes, and exactly wrong
for `enterSample`, which must actually *execute* those bytes through the rule
package the track's `stsd` entry declares. Filler bytes are not a decodable NAL.

So the tracks below are the P5j-3b shape with real payloads:

  - the AVC track's samples are 4-byte length-prefixed AVC NAL units, laid out
    the way a real muxer writes them, so framing finds genuine unit boundaries
    and each unit compiles against `video.h264.nal`;
  - the AAC track's samples are opaque access units referencing the `esds`
    AudioSpecificConfig, since ADR-0105 section 5 presents an AAC sample whole
    rather than decoding inside it.

Sample sizes are therefore *derived from the payload* rather than chosen up
front: a length-prefixed sample's size is the total of its prefixed units. The
generator asserts that the stsz values and the mdat layout agree, because a
mismatch here would surface as a confusing execution failure rather than as a
table error.

Chunk offsets are resolved in two passes exactly as in the P5j-3b generator: the
moov is built once with zeroed offsets to measure its size, then rebuilt with
absolute offsets derived from that size. Entry widths do not depend on the
values, so the second moov is byte-length identical; the generator asserts this
rather than assuming it.
"""

import struct
from pathlib import Path
from typing import Optional

FIXTURES_DIR = Path(__file__).resolve().parent


# ---------------------------------------------------------------------------
# Box framing (identical to the P5f/P5g/P5h/P5j/P5j-3b generators)
# ---------------------------------------------------------------------------

def make_box(fourcc_str: str, payload: bytes) -> bytes:
    return struct.pack(">I4s", len(payload) + 8, fourcc_str.encode("ascii")) + payload


def make_full_box(fourcc_str: str, version: int, flags: int, payload: bytes) -> bytes:
    v_flags = ((version & 0xFF) << 24) | (flags & 0x00FFFFFF)
    return make_box(fourcc_str, struct.pack(">I", v_flags) + payload)


def make_ftyp() -> bytes:
    payload = b"isom" + struct.pack(">I", 512) + b"isom"
    return make_box("ftyp", payload)


def write_fixture(name: str, data: bytes) -> Path:
    out_path = FIXTURES_DIR / name
    out_path.write_bytes(data)
    print(f"Generated {out_path} ({len(data)} bytes)")
    return out_path


# ---------------------------------------------------------------------------
# Movie and track headers
# ---------------------------------------------------------------------------

def make_mvhd_v0(timescale: int, duration: int, next_track_id: int) -> bytes:
    payload = struct.pack(">IIII", 0, 0, timescale, duration)
    payload += struct.pack(">IH", 0x00010000, 0x0100)  # rate 1.0, volume 1.0
    payload += struct.pack(">HQ", 0, 0)  # reserved (16-bit) + reserved_2 (64-bit)
    matrix = [0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000]
    payload += struct.pack(">9I", *matrix)
    payload += struct.pack(">6I", 0, 0, 0, 0, 0, 0)
    payload += struct.pack(">I", next_track_id)
    return make_full_box("mvhd", 0, 0, payload)


def make_tkhd_v0(track_id: int, duration: int, width: int, height: int) -> bytes:
    flags = 0x000007
    payload = struct.pack(">IIII", 0, 0, track_id, 0)
    payload += struct.pack(">IQ", duration, 0)
    payload += struct.pack(">hhhh", 0, 0, 0x0100, 0)
    matrix = [0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000]
    payload += struct.pack(">9I", *matrix)
    payload += struct.pack(">II", width, height)
    return make_full_box("tkhd", 0, flags, payload)


def make_mdhd_v0(timescale: int, duration: int, language: int = 0x15C7) -> bytes:
    payload = struct.pack(">IIII", 0, 0, timescale, duration)
    payload += struct.pack(">HH", language & 0x7FFF, 0)
    return make_full_box("mdhd", 0, 0, payload)


def make_hdlr(handler_type_str: str, name_str: str) -> bytes:
    payload = struct.pack(">I4s3I", 0, handler_type_str.encode("ascii"), 0, 0, 0)
    payload += name_str.encode("utf-8") + b"\x00"
    return make_full_box("hdlr", 0, 0, payload)


# ---------------------------------------------------------------------------
# Sample entries (avc1/avcC and mp4a/esds), carrying @target_format payloads
# ---------------------------------------------------------------------------

def make_avc1_entry(children: bytes, width: int = 1920, height: int = 1080) -> bytes:
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


def make_mp4a_entry(children: bytes, channel_count: int = 2, sample_rate: int = 44100) -> bytes:
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


def make_avcC_box(sps_list: list,
                  pps_list: list,
                  length_size_minus_one: int = 3,
                  configuration_version: int = 1) -> bytes:
    """An AVCDecoderConfigurationRecord.

    `length_size_minus_one` occupies the low 2 bits of the byte whose high 6 bits
    are reserved 111111b, so every value 0..3 is expressible on the wire and
    yields a length prefix of 1..4 bytes.
    """
    if not 0 <= length_size_minus_one <= 3:
        raise ValueError("length_size_minus_one must fit 2 bits")

    num_sps = len(sps_list) & 0x1F
    b5 = 0xE0 | num_sps
    length_byte = 0xFC | length_size_minus_one

    sps_payload = b""
    for sps in sps_list:
        sps_payload += struct.pack(">H", len(sps)) + sps

    pps_payload = struct.pack(">B", len(pps_list) & 0xFF)
    for pps in pps_list:
        pps_payload += struct.pack(">H", len(pps)) + pps

    profile = sps_list[0][1]
    compat = sps_list[0][2]
    level = sps_list[0][3]

    payload = (
        struct.pack(">BBBBBB",
                    configuration_version, profile, compat, level, length_byte, b5)
        + sps_payload
        + pps_payload
    )
    # Profile 100 mandates the chroma/bit-depth extension block.
    payload += struct.pack(">BBBB", 0xFD, 0xF8, 0xF8, 0)
    return make_box("avcC", payload)


def encode_descriptor_length(length: int, byte_count: int = 1) -> bytes:
    if byte_count < 1 or byte_count > 4:
        raise ValueError("byte_count must be 1..4")
    if length < 0 or length >= (1 << (7 * byte_count)):
        raise ValueError("length does not fit descriptor encoding")
    encoded = bytearray()
    for index in range(byte_count):
        shift = 7 * (byte_count - index - 1)
        encoded.append(((length >> shift) & 0x7F) | (0x80 if index + 1 < byte_count else 0))
    return bytes(encoded)


def make_esds_box(asc_bytes: bytes) -> bytes:
    dsi = b"\x05" + encode_descriptor_length(len(asc_bytes)) + asc_bytes
    dc_body = (
        struct.pack(">BB", 0x40, (0x05 << 2) | 1)
        + struct.pack(">I", 0x018000)[1:]
        + struct.pack(">II", 128000, 128000)
        + dsi
    )
    dc = b"\x04" + encode_descriptor_length(len(dc_body)) + dc_body
    es_body = struct.pack(">HB", 1, 0) + dc
    es = b"\x03" + encode_descriptor_length(len(es_body)) + es_body
    return make_box("esds", struct.pack(">I", 0) + es)


def make_stsd(entries: list) -> bytes:
    payload = struct.pack(">II", 0, len(entries)) + b"".join(entries)
    return make_box("stsd", payload)


# 25-byte High-profile (100) level 4.1 SPS and a 4-byte PPS, as in the P5h/P5j-3b
# fixtures. These live in the avcC, i.e. they are the *configuration*, not the
# sample payload; the NALs written into mdat below are built separately.
AVC_SPS = (
    b"\x67\x64\x00\x29\xac\x2b\x40\x3c\x01\x13\xf2\xe0\x22\x00\x00\x03"
    b"\x00\x02\x00\x00\x03\x00\x79\x1e\x30\x63\x70"
)
AVC_PPS = b"\x68\xee\x3c\x80"
# AudioSpecificConfig: AAC-LC (2), 44.1 kHz (4), stereo (2).
AAC_ASC = b"\x12\x10"


def video_stsd() -> bytes:
    return make_stsd([make_avc1_entry(make_avcC_box([AVC_SPS], [AVC_PPS]))])


def audio_stsd() -> bytes:
    return make_stsd([make_mp4a_entry(make_esds_box(AAC_ASC))])


# ---------------------------------------------------------------------------
# In-band NAL payloads
#
# `enterSample` executes these bytes against `video.h264.nal`, so unlike the
# P5j-3b filler they have to be real syntax. They are written as bit strings and
# packed, which is how the H.264 tests build the same units, so an Exp-Golomb
# field is emitted at its true width rather than guessed at byte granularity.
# ---------------------------------------------------------------------------

def append_fixed_bits(bits: list, value: int, width: int) -> None:
    for index in range(width - 1, -1, -1):
        bits.append((value >> index) & 1)


def append_unsigned_exp_golomb(bits: list, value: int) -> None:
    """H.264 section 9.1 ue(v)."""
    code_num = value + 1
    width = code_num.bit_length()
    for _ in range(width - 1):
        bits.append(0)
    append_fixed_bits(bits, code_num, width)


def append_signed_exp_golomb(bits: list, value: int) -> None:
    """H.264 section 9.1.1 se(v)."""
    mapped = 2 * value - 1 if value > 0 else -2 * value
    append_unsigned_exp_golomb(bits, mapped)


def pack_bits(bits: list) -> bytes:
    assert len(bits) % 8 == 0, "bit string must be byte-aligned before packing"
    out = bytearray()
    for offset in range(0, len(bits), 8):
        byte = 0
        for bit in bits[offset:offset + 8]:
            byte = (byte << 1) | bit
        out.append(byte)
    return bytes(out)


def rbsp_trailing(bits: list) -> None:
    bits.append(1)  # rbsp_stop_one_bit
    while len(bits) % 8 != 0:
        bits.append(0)


def sps_nal(sps_id: int = 0) -> bytes:
    """A baseline-profile SPS NAL, header byte 0x67, no emulation prevention.

    The field order follows ISO/IEC 14496-10 section 7.3.2.1 for
    profile_idc 66, which takes neither the chroma_format_idc block nor the
    scaling-list block.
    """
    bits: list = []
    append_fixed_bits(bits, 66, 8)  # profile_idc: baseline
    append_fixed_bits(bits, 0, 8)   # constraint_set flags + reserved_zero_2bits
    append_fixed_bits(bits, 30, 8)  # level_idc
    append_unsigned_exp_golomb(bits, sps_id)
    append_unsigned_exp_golomb(bits, 0)   # log2_max_frame_num_minus4
    append_unsigned_exp_golomb(bits, 0)   # pic_order_cnt_type
    append_unsigned_exp_golomb(bits, 0)   # log2_max_pic_order_cnt_lsb_minus4
    append_unsigned_exp_golomb(bits, 1)   # max_num_ref_frames
    append_fixed_bits(bits, 0, 1)         # gaps_in_frame_num_value_allowed_flag
    append_unsigned_exp_golomb(bits, 19)  # pic_width_in_mbs_minus1 -> 320 px
    append_unsigned_exp_golomb(bits, 14)  # pic_height_in_map_units_minus1 -> 240 px
    append_fixed_bits(bits, 1, 1)         # frame_mbs_only_flag
    append_fixed_bits(bits, 1, 1)         # direct_8x8_inference_flag
    append_fixed_bits(bits, 0, 1)         # frame_cropping_flag
    append_fixed_bits(bits, 0, 1)         # vui_parameters_present_flag
    rbsp_trailing(bits)
    return b"\x67" + pack_bits(bits)


def pps_nal(pps_id: int = 0, sps_id: int = 0) -> bytes:
    """A PPS NAL, header byte 0x68, referring to `sps_id`."""
    bits: list = []
    append_unsigned_exp_golomb(bits, pps_id)
    append_unsigned_exp_golomb(bits, sps_id)
    append_fixed_bits(bits, 0, 1)        # entropy_coding_mode_flag
    append_fixed_bits(bits, 0, 1)        # bottom_field_pic_order_in_frame_present_flag
    append_unsigned_exp_golomb(bits, 0)  # num_slice_groups_minus1
    append_unsigned_exp_golomb(bits, 0)  # num_ref_idx_l0_default_active_minus1
    append_unsigned_exp_golomb(bits, 0)  # num_ref_idx_l1_default_active_minus1
    append_fixed_bits(bits, 0, 1)        # weighted_pred_flag
    append_fixed_bits(bits, 0, 2)        # weighted_bipred_idc
    append_signed_exp_golomb(bits, 0)    # pic_init_qp_minus26
    append_signed_exp_golomb(bits, 0)    # pic_init_qs_minus26
    append_signed_exp_golomb(bits, 0)    # chroma_qp_index_offset
    append_fixed_bits(bits, 0, 1)        # deblocking_filter_control_present_flag
    append_fixed_bits(bits, 0, 1)        # constrained_intra_pred_flag
    append_fixed_bits(bits, 0, 1)        # redundant_pic_cnt_present_flag
    rbsp_trailing(bits)
    return b"\x68" + pack_bits(bits)


def sei_nal(payload_byte_count: int = 4) -> bytes:
    """An SEI NAL (header 0x06) carrying a filler payload.

    Gives a sample a third unit whose type differs from SPS and PPS, so a test
    asserting "this sample entered several units" is not satisfied by a single
    structure being decoded three times.
    """
    body = bytes([0x03])                       # payload_type: filler_payload
    body += bytes([payload_byte_count])        # payload_size
    body += b"\xff" * payload_byte_count       # filler bytes, none of them zero
    body += b"\x80"                            # rbsp_trailing_bits
    return b"\x06" + body


def length_prefixed(units: list, prefix_bytes: int = 4) -> bytes:
    """Concatenates `units` with big-endian length prefixes.

    This is the in-container AVC sample layout the avcC's lengthSizeMinusOne
    describes, so a sample built here is framed by the same rule the extractor
    reads out of the configuration.
    """
    out = bytearray()
    for unit in units:
        out += len(unit).to_bytes(prefix_bytes, "big")
        out += unit
    return bytes(out)


# ---------------------------------------------------------------------------
# Sample tables
# ---------------------------------------------------------------------------

def stts_box(entries: list) -> bytes:
    """entries: list of (sample_count, sample_delta)."""
    payload = struct.pack(">I", len(entries))
    payload += b"".join(struct.pack(">II", count, delta) for count, delta in entries)
    return make_full_box("stts", 0, 0, payload)


def stsc_box(entries: list) -> bytes:
    """entries: list of (first_chunk, samples_per_chunk, sample_description_index)."""
    payload = struct.pack(">I", len(entries))
    payload += b"".join(struct.pack(">III", *entry) for entry in entries)
    return make_full_box("stsc", 0, 0, payload)


def stsz_box(sample_size: int, sizes: list) -> bytes:
    """A non-zero sample_size declares a uniform track and emits no entry table."""
    if sample_size != 0:
        return make_full_box("stsz", 0, 0, struct.pack(">II", sample_size, len(sizes)))
    payload = struct.pack(">II", 0, len(sizes))
    payload += b"".join(struct.pack(">I", size) for size in sizes)
    return make_full_box("stsz", 0, 0, payload)


def stco_box(offsets: list) -> bytes:
    payload = struct.pack(">I", len(offsets))
    payload += b"".join(struct.pack(">I", offset) for offset in offsets)
    return make_full_box("stco", 0, 0, payload)


def stss_box(sample_numbers: list) -> bytes:
    payload = struct.pack(">I", len(sample_numbers))
    payload += b"".join(struct.pack(">I", number) for number in sample_numbers)
    return make_full_box("stss", 0, 0, payload)


def ctts_box(entries: list, version: int) -> bytes:
    """entries: list of (sample_count, sample_offset), offsets in wire form."""
    payload = struct.pack(">I", len(entries))
    for count, offset in entries:
        payload += struct.pack(">I", count) + struct.pack(">i" if offset < 0 else ">I", offset)
    return make_full_box("ctts", version, 0, payload)


# ---------------------------------------------------------------------------
# Track and file assembly
# ---------------------------------------------------------------------------

def make_stbl(stsd: bytes,
              stts: bytes,
              stsc: bytes,
              size_box: bytes,
              offset_box: bytes,
              stss: Optional[bytes] = None,
              ctts: Optional[bytes] = None) -> bytes:
    children = stsd + stts + stsc + size_box + offset_box
    if stss is not None:
        children += stss
    if ctts is not None:
        children += ctts
    return make_box("stbl", children)


def make_trak(track_id: int,
              timescale: int,
              duration: int,
              handler: str,
              stbl: bytes,
              width: int = 1920 << 16,
              height: int = 1080 << 16) -> bytes:
    minf = make_box("minf", stbl)
    mdia = make_box(
        "mdia",
        make_mdhd_v0(timescale, duration)
        + make_hdlr(handler, "StreamView P5j-4")
        + minf,
    )
    return make_box("trak", make_tkhd_v0(track_id, duration, width, height) + mdia)


def assemble(build_traks, mdat_payload: bytes) -> bytes:
    """Resolve chunk offsets against the final file layout.

    `build_traks(mdat_data_offset)` returns the concatenated trak boxes with
    absolute chunk offsets based at `mdat_data_offset`.
    """
    ftyp = make_ftyp()
    mvhd = make_mvhd_v0(1000, 1000, 16)

    probe_moov = make_box("moov", mvhd + build_traks(0))
    mdat_data_offset = len(ftyp) + len(probe_moov) + 8

    moov = make_box("moov", mvhd + build_traks(mdat_data_offset))
    assert len(moov) == len(probe_moov), "chunk offset values must not change moov size"

    data = ftyp + moov + make_box("mdat", mdat_payload)
    assert data[mdat_data_offset - 8:mdat_data_offset - 4] == struct.pack(">I", len(mdat_payload) + 8)
    assert data[mdat_data_offset - 4:mdat_data_offset] == b"mdat"
    return data


def chunk_offsets(base: int, sizes: list, samples_per_chunk: int) -> list:
    offsets = []
    cursor = base
    for index in range(0, len(sizes), samples_per_chunk):
        offsets.append(cursor)
        cursor += sum(sizes[index:index + samples_per_chunk])
    return offsets


def stsc_for_chunking(sizes: list, samples_per_chunk: int) -> bytes:
    """Describe exactly the chunking that `chunk_offsets` lays out.

    An stsc run extends until the next run's first_chunk, so a trailing short
    chunk needs a run of its own. A lone (1, samples_per_chunk, 1) run would
    instead claim every chunk is full and over-count the track's samples.
    """
    counts = [len(sizes[index:index + samples_per_chunk])
              for index in range(0, len(sizes), samples_per_chunk)]
    runs = []
    for chunk_number, count in enumerate(counts, start=1):
        if not runs or runs[-1][1] != count:
            runs.append((chunk_number, count, 1))
    return stsc_box(runs)


def opaque_sample_bytes(sizes: list, first_byte: int) -> bytes:
    """One distinct fill byte per opaque sample.

    An AAC access unit is presented whole rather than decoded, so its bytes need
    only be distinguishable, not valid. A distinct fill byte per sample proves
    the envelope's payload span points at that sample.
    """
    out = bytearray()
    for index, size in enumerate(sizes):
        out += bytes([(first_byte + index) & 0xFF]) * size
    return bytes(out)


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

def generate_avc_multi_nal() -> Path:
    """One AVC track whose samples hold real 4-byte length-prefixed NAL units.

    Sample 1 carries three units (SPS, PPS, SEI) so entering it must produce
    several NAL children rather than one. Samples 2 and 3 carry a single SEI
    each, which keeps them decodable while staying distinct in size from sample
    1, so a descriptor cannot appear correct by carrying the wrong sample's
    extent. Two chunks of two and one sample exercise the mid-chunk path.
    """
    sample_payloads = [
        length_prefixed([sps_nal(), pps_nal(), sei_nal(4)]),
        length_prefixed([sei_nal(8)]),
        length_prefixed([sei_nal(16)]),
    ]
    # Sizes are derived from the payload, never chosen: the table has to describe
    # the bytes actually written, or framing would read a prefix mid-unit.
    sizes = [len(payload) for payload in sample_payloads]
    assert len(set(sizes)) == len(sizes), "sample sizes must stay distinguishable"

    def build_traks(base: int) -> bytes:
        stbl = make_stbl(
            video_stsd(),
            stts_box([(len(sizes), 1000)]),
            stsc_for_chunking(sizes, 2),
            stsz_box(0, sizes),
            stco_box(chunk_offsets(base, sizes, 2)),
            stss=stss_box([1]),
            ctts=ctts_box([(len(sizes), 0)], version=0),
        )
        return make_trak(1, 30000, 3000, "vide", stbl)

    return write_fixture(
        "mp4_p5j4_avc_multi_nal.mp4",
        assemble(build_traks, b"".join(sample_payloads)),
    )


def generate_aac_opaque() -> Path:
    """One AAC track whose samples are opaque access units.

    Neither stss nor ctts is present: an absent stss means every sample is a sync
    sample (ISO/IEC 14496-12 section 8.6.2.1), and an absent ctts means pts equals
    dts. Entering one of these samples must produce the access-unit envelope of
    ADR-0105 section 5, referencing the esds AudioSpecificConfig, rather than
    decoding into the payload.
    """
    sizes = [64, 48, 32, 16]

    def build_traks(base: int) -> bytes:
        stbl = make_stbl(
            audio_stsd(),
            stts_box([(len(sizes), 1024)]),
            stsc_box([(1, len(sizes), 1)]),
            stsz_box(0, sizes),
            stco_box(chunk_offsets(base, sizes, len(sizes))),
        )
        return make_trak(1, 44100, 4096, "soun", stbl, width=0, height=0)

    return write_fixture(
        "mp4_p5j4_aac_opaque.mp4",
        assemble(build_traks, opaque_sample_bytes(sizes, 0xB1)),
    )


def generate_two_track_movie_bytes() -> bytes:
    """An AVC track and an AAC track in one movie, with distinct track ids.

    `tracks()` must report both with their own timescale, sample count, and
    declared target format, and `enterSample` must reach the right rule for each.
    A single-track fixture cannot distinguish per-track resolution from
    first-track-only resolution.
    """
    video_payloads = [
        length_prefixed([sps_nal(), pps_nal()]),
        length_prefixed([sei_nal(4)]),
    ]
    video_sizes = [len(payload) for payload in video_payloads]
    audio_sizes = [96, 80]

    video_bytes = b"".join(video_payloads)
    audio_bytes = opaque_sample_bytes(audio_sizes, 0xC1)
    audio_base_delta = len(video_bytes)

    def build_traks(base: int) -> bytes:
        video = make_trak(1, 30000, 2000, "vide", make_stbl(
            video_stsd(),
            stts_box([(len(video_sizes), 1000)]),
            stsc_box([(1, len(video_sizes), 1)]),
            stsz_box(0, video_sizes),
            stco_box([base]),
            stss=stss_box([1]),
        ))
        audio = make_trak(2, 44100, 2048, "soun", make_stbl(
            audio_stsd(),
            stts_box([(len(audio_sizes), 1024)]),
            stsc_box([(1, len(audio_sizes), 1)]),
            stsz_box(0, audio_sizes),
            stco_box([base + audio_base_delta]),
        ), width=0, height=0)
        return video + audio

    return assemble(build_traks, video_bytes + audio_bytes)


def generate_two_track_movie() -> Path:
    return write_fixture("mp4_p5j4_two_tracks.mp4", generate_two_track_movie_bytes())


def generate_avc_truncated_unit() -> Path:
    """One AVC track whose second sample declares more payload than it carries.

    The sample's stsz size covers a 4-byte length prefix announcing 0x40 payload
    bytes while only 4 follow, so framing must fail on that unit alone. Entering
    it has to report the failure and leave the container tree, the paging state,
    and the shared sample tree exactly as they were.
    """
    intact = length_prefixed([sei_nal(4)])
    truncated = struct.pack(">I", 0x40) + sei_nal(4)[:4]
    sample_payloads = [intact, truncated, intact]
    sizes = [len(payload) for payload in sample_payloads]

    def build_traks(base: int) -> bytes:
        stbl = make_stbl(
            video_stsd(),
            stts_box([(len(sizes), 1000)]),
            stsc_box([(1, len(sizes), 1)]),
            stsz_box(0, sizes),
            stco_box([base]),
            stss=stss_box([1]),
        )
        return make_trak(1, 30000, 3000, "vide", stbl)

    return write_fixture(
        "mp4_p5j4_avc_truncated_unit.mp4",
        assemble(build_traks, b"".join(sample_payloads)),
    )


def generate_terminal_resource_limit() -> Path:
    """A complete two-track movie followed by a container nested past the limit.

    The tracks parse cleanly, so the analyzer publishes a usable-looking track
    list before the nested boxes exceed the 256-level container depth cap and
    end the analysis terminally. That ordering is the point: the tree holds real
    tracks, yet the analysis died, so anything that keys off `finished()` alone
    will happily index and permanently cache that abandoned tree.
    """
    movie = generate_two_track_movie_bytes()

    nested = b""
    for _ in range(300):
        nested = make_box("moov", nested)

    return write_fixture("mp4_p5j4_terminal_resource_limit.mp4", movie + nested)


def main() -> None:
    generate_avc_multi_nal()
    generate_aac_opaque()
    generate_two_track_movie()
    generate_avc_truncated_unit()
    generate_terminal_resource_limit()


if __name__ == "__main__":
    main()

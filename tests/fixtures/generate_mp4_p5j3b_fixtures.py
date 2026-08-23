#!/usr/bin/env python3
"""
Generate reproducible MP4 binary test fixtures for Task P5j-3b:
analysis tree -> Mp4TrackSampleTables extraction and reader binding.

The P5g/P5j fixtures exercise individual sample-table boxes and their failure
modes, so their tables are deliberately inconsistent with one another and their
tracks carry no tkhd/mdhd. P5j-3b needs the opposite: every track below is
*complete and mutually consistent*, carrying

  - tkhd  -> track_id,
  - mdhd  -> timescale,
  - stsd  -> a sample entry that declares a target format,
  - stts / stsc / (stsz | stz2) / (stco | co64) with agreeing declared counts,
  - chunk offsets that point at real bytes inside this file's own mdat,

which is what Mp4SampleTableIndex::build() accepts. No pre-existing fixture
provides that combination.

Chunk offsets are resolved in two passes: the moov is built once with zeroed
offsets to measure its size, then rebuilt with absolute offsets derived from
that size. Entry widths do not depend on the values, so the second moov is
byte-length identical; the generator asserts this rather than assuming it.
"""

import struct
from pathlib import Path
from typing import Optional

FIXTURES_DIR = Path(__file__).resolve().parent


# ---------------------------------------------------------------------------
# Box framing (identical to the P5f/P5g/P5h/P5j generators)
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
    yields a length prefix of 1..4 bytes. `configuration_version` is normally 1;
    any other value makes the rule stop at that field with an `unsupported`
    diagnostic, leaving the record's remaining fields undecoded.
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


# 25-byte High-profile (100) level 4.1 SPS and a 4-byte PPS, as in the P5h fixtures.
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


def video_stsd_length_size_3() -> bytes:
    """avcC declaring lengthSizeMinusOne == 2, i.e. a 3-byte length prefix.

    The field is 2 bits wide, so 3-byte prefixes are expressible on the wire even
    though SamplePayloadFramer accepts only 1, 2, and 4. Extraction must report
    the decoded 3 rather than substitute a legal value.
    """
    return make_stsd(
        [make_avc1_entry(make_avcC_box([AVC_SPS], [AVC_PPS], length_size_minus_one=2))]
    )


def video_stsd_unsupported_avcc_version() -> bytes:
    """avcC declaring configurationVersion 2.

    The rule stops at `unsupported(...)` before reaching lengthSizeMinusOne, so no
    prefix size is declared and no target format is reached.
    """
    return make_stsd(
        [make_avc1_entry(make_avcC_box([AVC_SPS], [AVC_PPS], configuration_version=2))]
    )


def video_stsd_without_avcc() -> bytes:
    """An avc1 entry carrying no avcC at all.

    Nothing declares a prefix size, so extraction must leave it absent instead of
    falling back to the 4-byte default that most AVC files happen to use.
    """
    return make_stsd([make_avc1_entry(b"")])


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


def stz2_box(field_size: int, sizes: list) -> bytes:
    payload = struct.pack(">BBBB", 0, 0, 0, field_size) + struct.pack(">I", len(sizes))
    if field_size == 4:
        for index in range(0, len(sizes), 2):
            first = sizes[index] & 0x0F
            second = sizes[index + 1] & 0x0F if index + 1 < len(sizes) else 0
            payload += struct.pack(">B", (first << 4) | second)
    elif field_size == 8:
        payload += b"".join(struct.pack(">B", size) for size in sizes)
    elif field_size == 16:
        payload += b"".join(struct.pack(">H", size) for size in sizes)
    else:
        raise ValueError("field_size must be 4, 8, or 16")
    return make_full_box("stz2", 0, 0, payload)


def stco_box(offsets: list) -> bytes:
    payload = struct.pack(">I", len(offsets))
    payload += b"".join(struct.pack(">I", offset) for offset in offsets)
    return make_full_box("stco", 0, 0, payload)


def co64_box(offsets: list) -> bytes:
    payload = struct.pack(">I", len(offsets))
    payload += b"".join(struct.pack(">Q", offset) for offset in offsets)
    return make_full_box("co64", 0, 0, payload)


def stss_box(sample_numbers: list) -> bytes:
    payload = struct.pack(">I", len(sample_numbers))
    payload += b"".join(struct.pack(">I", number) for number in sample_numbers)
    return make_full_box("stss", 0, 0, payload)


def ctts_box(entries: list, version: int) -> bytes:
    """entries: list of (sample_count, sample_offset).

    Offsets are written as raw 32-bit big-endian words; negative version 1
    offsets are emitted in two's complement, which is the exact wire form a real
    muxer produces. Sign reinterpretation is the indexer's job.
    """
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
        + make_hdlr(handler, "StreamView P5j-3b")
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


def sample_bytes(sizes: list, first_byte: int) -> bytes:
    """One distinct fill byte per sample, so tests can prove a descriptor's
    source span points at that sample's bytes and not merely at a plausible
    offset."""
    out = bytearray()
    for index, size in enumerate(sizes):
        out += bytes([(first_byte + index) & 0xFF]) * size
    return bytes(out)


def chunk_offsets(base: int, sizes: list, samples_per_chunk: int) -> list:
    offsets = []
    cursor = base
    for index in range(0, len(sizes), samples_per_chunk):
        offsets.append(cursor)
        cursor += sum(sizes[index:index + samples_per_chunk])
    return offsets


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

def generate_complete_track() -> Path:
    """The primary end-to-end track: stsz table, stss, ctts v0, two chunks.

    6 samples of 1000/200/150/300/120/90 bytes, 3 per chunk, timescale 30000,
    stts 6x1000 ticks, sync samples 1 and 4, ctts v0 rows (3, 500) and
    (3, 1000).
    """
    sizes = [1000, 200, 150, 300, 120, 90]

    def build_traks(base: int) -> bytes:
        stbl = make_stbl(
            video_stsd(),
            stts_box([(6, 1000)]),
            stsc_box([(1, 3, 1)]),
            stsz_box(0, sizes),
            stco_box(chunk_offsets(base, sizes, 3)),
            stss=stss_box([1, 4]),
            ctts=ctts_box([(3, 500), (3, 1000)], version=0),
        )
        return make_trak(1, 30000, 6000, "vide", stbl)

    return write_fixture(
        "mp4_p5j3b_complete_track.mp4",
        assemble(build_traks, sample_bytes(sizes, 0xA1)),
    )


def generate_stss_absent_no_ctts() -> Path:
    """An audio track with neither stss nor ctts.

    Absent stss means every sample is a sync sample (ISO/IEC 14496-12 8.6.2.1),
    which is distinct from a present but empty table. Absent ctts means
    pts == dts. The mp4a/esds entry also exercises the audio target format.
    """
    sizes = [64, 48, 32, 16]

    def build_traks(base: int) -> bytes:
        stbl = make_stbl(
            audio_stsd(),
            stts_box([(4, 1024)]),
            stsc_box([(1, 4, 1)]),
            stsz_box(0, sizes),
            stco_box(chunk_offsets(base, sizes, 4)),
        )
        return make_trak(1, 44100, 4096, "soun", stbl, width=0, height=0)

    return write_fixture(
        "mp4_p5j3b_stss_absent_no_ctts.mp4",
        assemble(build_traks, sample_bytes(sizes, 0xB1)),
    )


def generate_ctts_v1_negative() -> Path:
    """ctts version 1 carrying a negative offset, so one sample has pts < dts.

    Sample 2 has dts 3000 and offset -3000, giving pts 0. A signed decode must
    reach zero rather than clamping at the unsigned reinterpretation.
    """
    sizes = [100, 80, 60, 40]

    def build_traks(base: int) -> bytes:
        stbl = make_stbl(
            video_stsd(),
            stts_box([(4, 3000)]),
            stsc_box([(1, 4, 1)]),
            stsz_box(0, sizes),
            stco_box(chunk_offsets(base, sizes, 4)),
            stss=stss_box([1]),
            ctts=ctts_box([(1, 3000), (1, -3000), (2, 0)], version=1),
        )
        return make_trak(1, 90000, 12000, "vide", stbl)

    return write_fixture(
        "mp4_p5j3b_ctts_v1_negative.mp4",
        assemble(build_traks, sample_bytes(sizes, 0xC1)),
    )


def generate_uniform_sample_size() -> Path:
    """A non-zero stsz.sample_size: no entry table exists at all.

    The rule emits no window for this track, so the extraction must report the
    size through defaultSampleSize and bind no sampleSize reader.
    """
    sample_size = 512
    sizes = [sample_size] * 4

    def build_traks(base: int) -> bytes:
        stbl = make_stbl(
            video_stsd(),
            stts_box([(4, 1000)]),
            stsc_box([(1, 2, 1)]),
            stsz_box(sample_size, sizes),
            stco_box(chunk_offsets(base, sizes, 2)),
            stss=stss_box([1, 3]),
        )
        return make_trak(1, 24000, 4000, "vide", stbl)

    return write_fixture(
        "mp4_p5j3b_uniform_sample_size.mp4",
        assemble(build_traks, sample_bytes(sizes, 0xD1)),
    )


def generate_stz2_tracks() -> Path:
    """Three tracks whose sizes come from stz2 at field_size 4, 8, and 16.

    Track 1 has an odd sample count, so its final packed pair carries one real
    nibble and one padding nibble. Three distinct track_ids in one file also
    prove that extraction is per-track rather than first-track-only.
    """
    sizes_4 = [3, 5, 7, 9, 11]
    sizes_8 = [200, 150, 100, 50]
    sizes_16 = [1000, 2000, 3000]

    base_8_delta = sum(sizes_4)
    base_16_delta = base_8_delta + sum(sizes_8)

    def build_traks(base: int) -> bytes:
        track_4 = make_trak(1, 30000, 5000, "vide", make_stbl(
            video_stsd(),
            stts_box([(len(sizes_4), 1000)]),
            stsc_box([(1, len(sizes_4), 1)]),
            stz2_box(4, sizes_4),
            stco_box([base]),
            stss=stss_box([1]),
        ))
        track_8 = make_trak(2, 30000, 4000, "vide", make_stbl(
            video_stsd(),
            stts_box([(len(sizes_8), 1000)]),
            stsc_box([(1, len(sizes_8), 1)]),
            stz2_box(8, sizes_8),
            stco_box([base + base_8_delta]),
            stss=stss_box([1]),
        ))
        track_16 = make_trak(3, 30000, 3000, "vide", make_stbl(
            video_stsd(),
            stts_box([(len(sizes_16), 1000)]),
            stsc_box([(1, len(sizes_16), 1)]),
            stz2_box(16, sizes_16),
            stco_box([base + base_16_delta]),
            stss=stss_box([1]),
        ))
        return track_4 + track_8 + track_16

    payload = (
        sample_bytes(sizes_4, 0xE1)
        + sample_bytes(sizes_8, 0xF1)
        + sample_bytes(sizes_16, 0x11)
    )
    return write_fixture("mp4_p5j3b_stz2_tracks.mp4", assemble(build_traks, payload))


def generate_co64_multichunk() -> Path:
    """64-bit chunk offsets across three chunks of two samples each.

    Pages that start mid-chunk must perform the sample-size catch-up rather than
    assuming a page boundary coincides with a chunk boundary.
    """
    sizes = [120, 130, 140, 150, 160, 170]

    def build_traks(base: int) -> bytes:
        stbl = make_stbl(
            video_stsd(),
            stts_box([(6, 512)]),
            stsc_box([(1, 2, 1)]),
            stsz_box(0, sizes),
            co64_box(chunk_offsets(base, sizes, 2)),
            stss=stss_box([1, 5]),
        )
        return make_trak(1, 48000, 3072, "vide", stbl)

    return write_fixture(
        "mp4_p5j3b_co64_multichunk.mp4",
        assemble(build_traks, sample_bytes(sizes, 0x21)),
    )


def sample_entry_variant(name: str, stsd: bytes, first_byte: int) -> Path:
    """A minimal complete track whose only variable is its sample entry.

    The tables below are the smallest set `Mp4SampleTableIndex::build()` accepts,
    so a test asserting on the sample description binding is not also depending on
    table shape. Extraction must still succeed: what these fixtures vary is what
    the entry *declares*, not whether the track is usable.
    """
    sizes = [40, 50, 60]

    def build_traks(base: int) -> bytes:
        stbl = make_stbl(
            stsd,
            stts_box([(len(sizes), 1000)]),
            stsc_box([(1, len(sizes), 1)]),
            stsz_box(0, sizes),
            stco_box(chunk_offsets(base, sizes, len(sizes))),
            stss=stss_box([1]),
        )
        return make_trak(1, 30000, 3000, "vide", stbl)

    return write_fixture(name, assemble(build_traks, sample_bytes(sizes, first_byte)))


def generate_length_size_3() -> Path:
    """An avcC declaring a 3-byte length prefix: expressible but unframeable."""
    return sample_entry_variant(
        "mp4_p5j3b_avcc_length_size_3.mp4",
        video_stsd_length_size_3(),
        0x31,
    )


def generate_unsupported_avcc_version() -> Path:
    """An avcC whose configurationVersion the rule refuses to decode."""
    return sample_entry_variant(
        "mp4_p5j3b_avcc_unsupported_version.mp4",
        video_stsd_unsupported_avcc_version(),
        0x41,
    )


def generate_avc1_without_avcc() -> Path:
    """An avc1 entry with no configuration record at all."""
    return sample_entry_variant(
        "mp4_p5j3b_avc1_without_avcc.mp4",
        video_stsd_without_avcc(),
        0x51,
    )


def main() -> None:
    generate_complete_track()
    generate_stss_absent_no_ctts()
    generate_ctts_v1_negative()
    generate_uniform_sample_size()
    generate_stz2_tracks()
    generate_co64_multichunk()
    generate_length_size_3()
    generate_unsupported_avcc_version()
    generate_avc1_without_avcc()


if __name__ == "__main__":
    main()

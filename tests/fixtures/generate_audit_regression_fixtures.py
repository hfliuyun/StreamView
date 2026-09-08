#!/usr/bin/env python3
"""Reproducible audit regressions; synthetic syntax, not decoded video pictures."""

import generate_mp4_p5j4_fixtures as mp4


def sps(sps_id=0, frame_bits=4):
    bits = []
    fixed = lambda value, width: mp4.append_fixed_bits(bits, value, width)
    ue = lambda value: mp4.append_unsigned_exp_golomb(bits, value)
    fixed(66, 8)
    fixed(0, 8)
    fixed(30, 8)
    ue(sps_id)
    ue(frame_bits - 4)
    ue(0)
    ue(0)
    ue(1)
    fixed(0, 1)
    ue(19)
    ue(14)
    fixed(1, 1)
    fixed(1, 1)
    fixed(0, 1)
    fixed(0, 1)
    mp4.rbsp_trailing(bits)
    return b"\x67" + mp4.pack_bits(bits)


def idr(pps_id=0, frame_bits=4):
    bits = []
    mp4.append_unsigned_exp_golomb(bits, 0)  # first_mb_in_slice
    mp4.append_unsigned_exp_golomb(bits, 2)  # I slice
    mp4.append_unsigned_exp_golomb(bits, pps_id)
    mp4.append_fixed_bits(bits, 0, frame_bits)
    mp4.append_unsigned_exp_golomb(bits, 0)  # idr_pic_id
    mp4.append_fixed_bits(bits, 0, 4)  # pic_order_cnt_lsb
    mp4.append_fixed_bits(bits, 0, 1)  # no_output_of_prior_pics_flag
    mp4.append_fixed_bits(bits, 0, 1)  # long_term_reference_flag
    mp4.append_signed_exp_golomb(bits, 0)  # slice_qp_delta
    mp4.rbsp_trailing(bits)  # opaque slice data boundary
    return b"\x65" + mp4.pack_bits(bits)


def description(frame_bits=4, multiple=False):
    ids = [0, 1] if multiple else [0]
    return mp4.make_avc1_entry(mp4.make_avcC_box(
        [sps(i, frame_bits) for i in ids],
        [mp4.pps_nal(i, i) for i in ids]))


def track(track_id, descriptions, payloads, base, description_indices=None):
    sizes = [len(payload) for payload in payloads]
    indices = description_indices or [1] * len(sizes)
    stsc = [(1, 1, indices[0])]
    for index in range(1, len(indices)):
        if indices[index] != indices[index - 1]:
            stsc.append((index + 1, 1, indices[index]))
    return mp4.make_trak(track_id, 30000, len(sizes) * 1000, "vide", mp4.make_stbl(
        mp4.make_stsd(descriptions), mp4.stts_box([(len(sizes), 1000)]),
        mp4.stsc_box(stsc), mp4.stsz_box(0, sizes),
        mp4.stco_box(mp4.chunk_offsets(base, sizes, 1))))


def assemble(build, payload, moov_after=False):
    if not moov_after:
        return mp4.assemble(build, payload)
    ftyp = mp4.make_ftyp()
    mdat = mp4.make_box("mdat", payload)
    moov = mp4.make_box("moov", mp4.make_mvhd_v0(1000, 1000, 16)
                        + build(len(ftyp) + 8))
    return ftyp + mdat + moov


def main():
    for after in (False, True):
        payloads = [mp4.length_prefixed([idr(1)])] * 2
        data = assemble(lambda base: track(1, [description(multiple=True)], payloads, base),
                        b"".join(payloads), after)
        mp4.write_fixture(f"audit_avcc_only_{'after' if after else 'before'}.mp4", data)

    first = [mp4.length_prefixed([idr(frame_bits=4)])] * 2
    second = [mp4.length_prefixed([idr(frame_bits=8)])] * 2
    first_bytes = b"".join(first)
    data = assemble(lambda base:
                    track(1, [description(4)], first, base)
                    + track(2, [description(8)], second, base + len(first_bytes)),
                    first_bytes + b"".join(second), True)
    mp4.write_fixture("audit_avc_two_tracks.mp4", data)

    payloads = [first[0], second[0]]
    data = assemble(lambda base: track(1, [description(4), description(8)], payloads,
                                        base, [1, 2]), b"".join(payloads), True)
    mp4.write_fixture("audit_avc_two_descriptions.mp4", data)

    payloads = [mp4.length_prefixed([sps(2), mp4.pps_nal(2, 2), idr(2)]),
                mp4.length_prefixed([idr(2)])]
    data = assemble(lambda base: track(1, [description()], payloads, base),
                    b"".join(payloads), True)
    mp4.write_fixture("audit_avc_inband.mp4", data)

    payloads = [mp4.length_prefixed([sps(0, 8)]),
                mp4.length_prefixed([mp4.pps_nal(0, 0)]),
                mp4.length_prefixed([idr(0, 8)])]
    data = assemble(lambda base: track(1, [description()], payloads, base),
                    b"".join(payloads), True)
    mp4.write_fixture("audit_avc_rebound_pps.mp4", data)

    # 65,536 variable-sized samples, two tracks, one sample per chunk.
    sizes = [1 + (i % 3) for i in range(32768)]
    payload = b"".join(bytes([i % 251 + 1]) * size for i, size in enumerate(sizes))
    def large_track(track_id, base):
        return mp4.make_trak(track_id, 44100, len(sizes) * 1024, "soun", mp4.make_stbl(
            mp4.audio_stsd(), mp4.stts_box([(len(sizes), 1024)]),
            mp4.stsc_box([(1, 1, 1)]), mp4.stsz_box(0, sizes),
            mp4.stco_box(mp4.chunk_offsets(base, sizes, 1))), width=0, height=0)
    mp4.write_fixture("audit_many_samples.mp4", assemble(
        lambda base: large_track(1, base) + large_track(2, base + len(payload)),
        payload + payload))


if __name__ == "__main__":
    main()

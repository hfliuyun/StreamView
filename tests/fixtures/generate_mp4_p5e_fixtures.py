#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generator for MP4 P5e test fixtures.
Generates binary test files with precise byte layouts for:
- ftyp with compatible brands array;
- 64-bit largesize boxes;
- size == 0 extending to EOF;
- unknown opaque boxes.
"""

import os
import struct

FIXTURES_DIR = os.path.dirname(os.path.abspath(__file__))

def create_basic_ftyp_mdat_fixture():
    """
    Creates a standard MP4 file containing:
    1. ftyp (32 bytes): major_brand=isom, minor_version=0x00000200, 4 compatible brands (isom, iso2, avc1, mp41)
    2. free (8 bytes)
    3. mdat (24 bytes): 16 bytes of media payload
    Total size: 64 bytes
    """
    data = bytearray()
    # ftyp header (8 bytes) + major_brand (4) + minor_version (4) + 4 brands (16) = 32 bytes
    data.extend(struct.pack('>IIII', 32, 0x66747970, 0x69736F6D, 0x00000200))
    data.extend(struct.pack('>IIII', 0x69736F6D, 0x69736F32, 0x61766331, 0x6D703431))

    # free (8 bytes)
    data.extend(struct.pack('>II', 8, 0x66726565))

    # mdat (24 bytes)
    mdat_body = bytes([i & 0xFF for i in range(16)])
    data.extend(struct.pack('>II', 8 + len(mdat_body), 0x6D646174))
    data.extend(mdat_body)

    path = os.path.join(FIXTURES_DIR, "mp4_p5e_basic_ftyp_mdat.mp4")
    with open(path, "wb") as f:
        f.write(data)
    print(f"Generated {path} ({len(data)} bytes)")

def create_largesize_box_fixture():
    """
    Creates an MP4 file with a 64-bit largesize mdat box (size == 1):
    1. ftyp (16 bytes)
    2. free (8 bytes)
    3. mdat (32 bytes): size=1, largesize=32, 16 bytes payload
    Total size: 56 bytes
    """
    data = bytearray()
    # ftyp (16 bytes)
    data.extend(struct.pack('>IIII', 16, 0x66747970, 0x69736F6D, 0x00000200))

    # free (8 bytes)
    data.extend(struct.pack('>II', 8, 0x66726565))

    # mdat (32 bytes) with size=1 and largesize=32
    mdat_body = bytes([0xAA] * 16)
    data.extend(struct.pack('>IIQ', 1, 0x6D646174, 16 + len(mdat_body)))
    data.extend(mdat_body)

    path = os.path.join(FIXTURES_DIR, "mp4_p5e_largesize_box.mp4")
    with open(path, "wb") as f:
        f.write(data)
    print(f"Generated {path} ({len(data)} bytes)")

def create_size0_eof_box_fixture():
    """
    Creates an MP4 file with a size == 0 mdat box extending to EOF:
    1. ftyp (16 bytes)
    2. free (8 bytes)
    3. mdat (20 bytes): size=0, 12 bytes payload
    Total size: 44 bytes
    """
    data = bytearray()
    # ftyp (16 bytes)
    data.extend(struct.pack('>IIII', 16, 0x66747970, 0x69736F6D, 0x00000200))

    # free (8 bytes)
    data.extend(struct.pack('>II', 8, 0x66726565))

    # mdat (size=0, extends to EOF)
    mdat_body = bytes([0xBB] * 12)
    data.extend(struct.pack('>II', 0, 0x6D646174))
    data.extend(mdat_body)

    path = os.path.join(FIXTURES_DIR, "mp4_p5e_size0_eof_box.mp4")
    with open(path, "wb") as f:
        f.write(data)
    print(f"Generated {path} ({len(data)} bytes)")

def create_unknown_opaque_box_fixture():
    """
    Creates an MP4 file with an unknown/opaque box (skip):
    1. ftyp (16 bytes)
    2. skip (24 bytes): unknown box FourCC (0x736B6970), 16 bytes opaque payload
    3. mdat (20 bytes): 12 bytes media payload
    Total size: 60 bytes
    """
    data = bytearray()
    # ftyp (16 bytes)
    data.extend(struct.pack('>IIII', 16, 0x66747970, 0x69736F6D, 0x00000200))

    # skip (24 bytes)
    skip_body = bytes([0xCC] * 16)
    data.extend(struct.pack('>II', 8 + len(skip_body), 0x736B6970))
    data.extend(skip_body)

    # mdat (20 bytes)
    mdat_body = bytes([0xDD] * 12)
    data.extend(struct.pack('>II', 8 + len(mdat_body), 0x6D646174))
    data.extend(mdat_body)

    path = os.path.join(FIXTURES_DIR, "mp4_p5e_unknown_opaque_box.mp4")
    with open(path, "wb") as f:
        f.write(data)
    print(f"Generated {path} ({len(data)} bytes)")

if __name__ == "__main__":
    os.makedirs(FIXTURES_DIR, exist_ok=True)
    create_basic_ftyp_mdat_fixture()
    create_largesize_box_fixture()
    create_size0_eof_box_fixture()
    create_unknown_opaque_box_fixture()

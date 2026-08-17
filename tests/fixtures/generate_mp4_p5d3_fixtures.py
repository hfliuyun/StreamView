#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generator for MP4 P5d-3 test fixtures.
Generates binary test files with precise byte layouts for ISOBMFF box scanning,
nested container drilling, sample table windowing, and D7 fragmented MP4 handling.
"""

import os
import struct

FIXTURES_DIR = os.path.dirname(os.path.abspath(__file__))

def create_d7_fragmented_fixture():
    """
    Creates a fragmented MP4 stream containing:
    1. ftyp (16 bytes)
    2. moov (24 bytes, container with mvhd 16 bytes)
    3. moof (24 bytes, fragmented movie fragment)
    4. mdat (20 bytes, media data)
    Total size: 84 bytes
    """
    data = bytearray()
    
    # 1. ftyp: size=16, type='ftyp' (0x66747970), major_brand='isom' (0x69736F6D), minor_version=0x00000200
    data.extend(struct.pack('>IIII', 16, 0x66747970, 0x69736F6D, 0x00000200))
    
    # 2. moov: size=24, type='moov' (0x6D6F6F76)
    #    nested mvhd: size=16, type='mvhd' (0x6D766864), payload=8 bytes (0x0102030405060708)
    moov_body = struct.pack('>IIQ', 16, 0x6D766864, 0x0102030405060708)
    data.extend(struct.pack('>II', 8 + len(moov_body), 0x6D6F6F76))
    data.extend(moov_body)
    
    # 3. moof: size=24, type='moof' (0x6D6F6F66), payload=16 bytes
    moof_body = b'\x00' * 16
    data.extend(struct.pack('>II', 8 + len(moof_body), 0x6D6F6F66))
    data.extend(moof_body)
    
    # 4. mdat: size=20, type='mdat' (0x6D646174), payload=12 bytes
    mdat_body = b'\xAA' * 12
    data.extend(struct.pack('>II', 8 + len(mdat_body), 0x6D646174))
    data.extend(mdat_body)
    
    path = os.path.join(FIXTURES_DIR, "mp4_d7_fragmented.mp4")
    with open(path, "wb") as f:
        f.write(data)
    print(f"Generated {path} ({len(data)} bytes)")

def create_nested_container_fixture():
    """
    Creates a deeply nested container stream:
    ftyp -> moov -> trak -> mdia -> minf -> stbl -> stsz (leaf) -> mdat
    """
    data = bytearray()
    
    # ftyp (16 bytes)
    data.extend(struct.pack('>IIII', 16, 0x66747970, 0x69736F6D, 0x00000200))
    
    # Leaf stsz inside stbl: size=24, type='stsz' (0x7374737A), sample_size=0, sample_count=2, entry1=100, entry2=200
    stsz_body = struct.pack('>IIII', 0, 2, 100, 200)
    stsz_box = struct.pack('>II', 8 + len(stsz_body), 0x7374737A) + stsz_body # 24 bytes
    
    # stbl (32 bytes)
    stbl_box = struct.pack('>II', 8 + len(stsz_box), 0x7374626C) + stsz_box # 32 bytes
    
    # minf (40 bytes)
    minf_box = struct.pack('>II', 8 + len(stbl_box), 0x6D696E66) + stbl_box # 40 bytes
    
    # mdia (48 bytes)
    mdia_box = struct.pack('>II', 8 + len(minf_box), 0x6D646961) + minf_box # 48 bytes
    
    # trak (56 bytes)
    trak_box = struct.pack('>II', 8 + len(mdia_box), 0x7472616B) + mdia_box # 56 bytes
    
    # moov (64 bytes)
    moov_box = struct.pack('>II', 8 + len(trak_box), 0x6D6F6F76) + trak_box # 64 bytes
    data.extend(moov_box)
    
    # mdat (16 bytes)
    mdat_body = b'\xBB' * 8
    data.extend(struct.pack('>II', 8 + len(mdat_body), 0x6D646174))
    data.extend(mdat_body)
    
    path = os.path.join(FIXTURES_DIR, "mp4_container_nested.mp4")
    with open(path, "wb") as f:
        f.write(data)
    print(f"Generated {path} ({len(data)} bytes)")

def create_sample_table_fixture():
    """
    Creates a sample table stream with stsz table containing 4 sample size entries.
    """
    data = bytearray()
    
    # stsz box: size=8 + 8 + 16 = 32 bytes
    # type = 'stsz' (0x7374737A)
    # sample_size = 0 (32-bit), sample_count = 4 (32-bit)
    # entry 1 = 10, entry 2 = 20, entry 3 = 30, entry 4 = 40 (32-bit each)
    stsz_body = struct.pack('>IIIIII', 0, 4, 10, 20, 30, 40)
    data.extend(struct.pack('>II', 8 + len(stsz_body), 0x7374737A))
    data.extend(stsz_body)
    
    path = os.path.join(FIXTURES_DIR, "mp4_sample_table.mp4")
    with open(path, "wb") as f:
        f.write(data)
    print(f"Generated {path} ({len(data)} bytes)")

if __name__ == "__main__":
    os.makedirs(FIXTURES_DIR, exist_ok=True)
    create_d7_fragmented_fixture()
    create_nested_container_fixture()
    create_sample_table_fixture()

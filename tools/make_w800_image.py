#!/usr/bin/env python3
import binascii
import gzip
import struct
import sys
from pathlib import Path

if len(sys.argv) != 4:
    raise SystemExit("usage: make_w800_image.py <code.bin> <out.img> <out.bin.gz>")
code = Path(sys.argv[1]).read_bytes()
body = code + b"\x00" * ((4 - (len(code) % 4)) % 4)
magic = 0xA0FFFF9F
attr = 0x00000200
img_addr = 0x20004000
img_len = len(body)
img_header_addr = img_addr
upgrade_img_addr = 0
org_checksum = (binascii.crc32(body) ^ 0xFFFFFFFF) & 0xFFFFFFFF
upd_no = 0
ver = b"raw6.00\n" + b"\x00" * 8
res0 = res1 = nextp = 0
hdr = struct.pack("<IIIIIIII16sIII", magic, attr, img_addr, img_len, img_header_addr, upgrade_img_addr,
                  org_checksum, upd_no, ver, res0, res1, nextp)
hd_checksum = (binascii.crc32(hdr) ^ 0xFFFFFFFF) & 0xFFFFFFFF
img = hdr + struct.pack("<I", hd_checksum) + body
Path(sys.argv[2]).write_bytes(img)
Path(sys.argv[3]).write_bytes(gzip.compress(img, mtime=0))
print(f"wrote {sys.argv[2]} ({len(img)} bytes) and {sys.argv[3]} ({Path(sys.argv[3]).stat().st_size} bytes)")
print(f"img_len=0x{img_len:x} org_crc=0x{org_checksum:08x} hdr_crc=0x{hd_checksum:08x}")

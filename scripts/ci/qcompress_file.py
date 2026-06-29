#!/usr/bin/env python3
import pathlib
import struct
import sys
import zlib

if len(sys.argv) != 3:
    print('usage: qcompress_file.py <input> <output>', file=sys.stderr)
    sys.exit(2)

src = pathlib.Path(sys.argv[1])
dst = pathlib.Path(sys.argv[2])
data = src.read_bytes()
# Qt qUncompress/qCompress format: big-endian uncompressed size followed by zlib data.
compressed = struct.pack('>I', len(data)) + zlib.compress(data, 9)
dst.parent.mkdir(parents=True, exist_ok=True)
dst.write_bytes(compressed)

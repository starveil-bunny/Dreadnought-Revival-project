#!/usr/bin/env python3
"""Decode a cooked UE4 4.13 Texture2D (.uasset with inline mips) to PNG.

  tex2png.py <in.uasset> <out.png>

Finds FTexturePlatformData by its pixel-format FString ("PF_DXT5" etc.):
SizeX, SizeY, NumSlices precede it; FirstMip, NumMips and the mips follow, each
a bCooked i32, a bulk-data header (flags u32, count i32, size i32, offset i64)
plus inline data, then the mip's SizeX/SizeY. The first FString match is
usually the name table, so a match must have sane sizes before it. Writes the first mip whose data is inline.
DXT data is wrapped in a DDS header for Pillow; B8G8R8A8 is read raw.
"""
import struct, sys
from PIL import Image

FORMATS = {b'PF_DXT1': b'DXT1', b'PF_DXT5': b'DXT5', b'PF_DXT3': b'DXT3', b'PF_B8G8R8A8': None}


def dds(w, h, fourcc, data):
    pf = struct.pack('<II4s5I', 32, 4, fourcc, 0, 0, 0, 0, 0)
    hdr = struct.pack('<7I44x', 124, 0x1007 | 0x80000, h, w, len(data), 0, 1) + pf + struct.pack('<5I', 0x1000, 0, 0, 0, 0)
    return b'DDS ' + hdr + data


def decode(raw):
    for name, fourcc in FORMATS.items():
        key = struct.pack('<i', len(name) + 1) + name + b'\0'
        i = raw.find(key)
        while i >= 12:
            sx, sy, ns = struct.unpack_from('<iii', raw, i - 12)
            if 0 < sx <= 8192 and 0 < sy <= 8192 and ns == 1:
                break
            i = raw.find(key, i + 1)
        if i < 12:
            continue
        p = i + 4 + len(name) + 1
        first, nmips = struct.unpack_from('<ii', raw, p)
        p += 8
        mips = []
        for _ in range(nmips):
            _cooked, flags, count, size, off = struct.unpack_from('<iIiiq', raw, p)
            p += 24
            # 0x1 = BULKDATA_PayloadAtEndOfFile: the bytes sit at BulkDataStartOffset + off,
            # i.e. at the file's end just before the trailing package tag (decals, 2048 px).
            inline = not (flags & 0x121) and size > 0 and p + size <= len(raw)
            data = raw[p:p + size] if inline else b''
            if inline:
                p += size
            mw, mh = struct.unpack_from('<ii', raw, p)
            p += 8
            mips.append((flags, size, off, data, mw, mh))
            if not (0 < mw <= 8192 and 0 < mh <= 8192):
                break
        tail = [(o + n) for f, n, o, _, _, _ in mips if f & 0x1 and not f & 0x120 and n > 0]
        bulk = len(raw) - 4 - max(tail) if tail else 0
        for flags, size, off, data, mw, mh in mips:
            if not data and tail and flags & 0x1 and not flags & 0x120 and size > 0 and bulk + off >= 0:
                data = raw[bulk + off:bulk + off + size]
            if data and 0 < mw <= 8192 and 0 < mh <= 8192:
                if fourcc is None:
                    return Image.frombytes('RGBA', (mw, mh), data, 'raw', 'BGRA')
                import io
                return Image.open(io.BytesIO(dds(mw, mh, fourcc, data))).convert('RGBA')
        raise SystemExit(f'no inline mip ({sx}x{sy}, {nmips} mips)')
    raise SystemExit('no known pixel format')


if __name__ == '__main__':
    decode(open(sys.argv[1], 'rb').read()).save(sys.argv[2])

#!/usr/bin/env python3
"""Extract every texture block Emergency Call Ambulance can load, straight from the ROM.

    tools/extract_texture_blocks.py [--rom build/generated/rom] [--out build/run/Textures/eca_blocks_src]

The game fills texture RAM with 256x256 16-bit blocks (with mipmaps) that it DMAs from
VROM or banked CROM. The list of blocks is a table in the fixed CROM; each group is
    [0x90000000]  (address, header, 0x5555C)...  0xFFFFFFFF
where the marker means "VROM via the texture port" (address = VROM word address) and no
marker means "banked CROM via the texture FIFO" (address in the 0xFF000000 window). The
groups are per scene; together they cover every texture the game has.

Each block is rebuilt exactly as CReal3D::StoreTexture() lays it out in texture RAM, hashed
the same way runtime/hdtex.cpp hashes a resident block, and written once per colour format
(the format is chosen per polygon at draw time, so a block can't be decoded just once):
    blk_<format>_<hash>.png
Formats 0 (T1RGB5), 5/6 (8-bit greyscale in the low/high byte) and 7 (RGBA4) cover ~96%
of what the game draws; the rest fall back to per-texture dumps or the original texture.
Then run tools/upscale_textures.py on the output (build_texture_pack.py does both).
"""
import argparse
import os
import struct
import sys

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit('extract_texture_blocks.py needs Pillow and numpy (pip install pillow numpy)')

CROM_BASE = 0xFF800000
BANK_SIZE = 0x800000
BLOCK = 256
DECODE8X8 = np.array([1, 0, 5, 4, 9, 8, 13, 12, 3, 2, 7, 6, 11, 10, 15, 14,
                      17, 16, 21, 20, 25, 24, 29, 28, 19, 18, 23, 22, 27, 26, 31, 30,
                      33, 32, 37, 36, 41, 40, 45, 44, 35, 34, 39, 38, 43, 42, 47, 46,
                      49, 48, 53, 52, 57, 56, 61, 60, 51, 50, 55, 54, 59, 58, 63, 62])
M64 = (1 << 64) - 1


def parse_table(crom, table_addr):
    """Yields (is_vrom, address, header) for every entry in the texture load table."""
    word = lambda a: struct.unpack_from('>I', crom, a - CROM_BASE)[0]
    a = table_addr
    while word(a) != 0:
        is_vrom = word(a) == 0x90000000
        if is_vrom:
            a += 4
        count = 0
        while word(a) != 0xFFFFFFFF:
            yield is_vrom, word(a), word(a + 4)
            a += 12
            count += 1
        a += 4
        if count == 0:
            break


def detile(stream):
    """Level 0 of a 256x256 16-bit block, from upload order (8x8 tiles) to texture RAM order."""
    tiles = stream[:BLOCK * BLOCK].reshape(-1, 64)[:, DECODE8X8]
    n = BLOCK // 8
    return tiles.reshape(n, n, 8, 8).transpose(0, 2, 1, 3).reshape(BLOCK, BLOCK)


def block_hash(texels):
    """Same as HashRegion() in runtime/hdtex.cpp."""
    h = 0x9E3779B97F4A7C15 ^ ((BLOCK << 32) | BLOCK)
    for row in texels.tolist():
        for t in row:
            h = ((h ^ t) * 0x100000001B3) & M64
        h = ((h ^ (h >> 29)) * 0xBF58476D1CE4E5B9) & M64
    return h ^ (h >> 32)


def decode(fmt, v):
    """RGBA image from 16-bit texels; same as ExtractColour() in New3D's shader."""
    v = v.astype(np.uint32)
    out = np.zeros(v.shape + (4,), np.uint8)
    n5 = lambda x: (((x & 0x1F) * 255 + 15) // 31).astype(np.uint8)
    n4 = lambda x: ((x & 0xF) * 17).astype(np.uint8)
    if fmt == 0:
        out[..., 0], out[..., 1], out[..., 2] = n5(v >> 10), n5(v >> 5), n5(v)
        out[..., 3] = np.where(v & 0x8000, 0, 255)
    elif fmt == 7:
        out[..., 0], out[..., 1], out[..., 2], out[..., 3] = n4(v >> 12), n4(v >> 8), n4(v >> 4), n4(v)
    elif fmt in (5, 6):
        lum = (v >> (0 if fmt == 5 else 8)) & 0xFF
        out[..., 0] = out[..., 1] = out[..., 2] = lum.astype(np.uint8)
        out[..., 3] = np.where(lum == 255, 0, 255)       # full white is the transparent key
    else:
        raise ValueError(fmt)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rom', default='build/generated/rom', help='romtool output directory')
    ap.add_argument('--out', default='build/run/Textures/eca_blocks_src')
    ap.add_argument('--table', type=lambda s: int(s, 0), default=0xFF838080,
                    help='address of the texture load table in the fixed CROM')
    ap.add_argument('--banks', default='2,3',
                    help='banked CROM banks to read FIFO groups from (the game selects bank 2; '
                         'blocks are matched by content, so extra banks only cost disk space)')
    ap.add_argument('--formats', default='0,5,6,7')
    args = ap.parse_args()

    crom = open(os.path.join(args.rom, 'crom.bin'), 'rb').read()
    vrom = np.frombuffer(open(os.path.join(args.rom, 'vrom.bin'), 'rb').read(), '<u2')
    banked = np.frombuffer(open(os.path.join(args.rom, 'banked_crom.bin'), 'rb').read(), '<u2')
    banks = [int(b) for b in args.banks.split(',') if b]
    formats = [int(f) for f in args.formats.split(',') if f]
    os.makedirs(args.out, exist_ok=True)

    sources = []   # (description, u16 offset into the array, array)
    for is_vrom, addr, header in parse_table(crom, args.table):
        if not (header >> 23) & 1 or (32 << ((header >> 14) & 7)) != BLOCK or (32 << ((header >> 17) & 7)) != BLOCK:
            continue   # only 16-bit 256x256 blocks occur in ECA
        if is_vrom:
            sources.append(('VROM %07X' % addr, addr * 2, vrom))
        else:
            for b in banks:
                sources.append(('bank %d +%06X' % (b, addr - 0xFF000000), (b * BANK_SIZE + addr - 0xFF000000) // 2, banked))

    seen, written, skipped = set(), 0, 0
    for desc, off, arr in sources:
        if off + BLOCK * BLOCK > len(arr):
            continue
        texels = detile(arr[off:off + BLOCK * BLOCK])
        if (texels == texels.flat[0]).all():
            skipped += 1   # blank (unpopulated ROM)
            continue
        h = block_hash(texels)
        if h in seen:
            continue
        seen.add(h)
        for fmt in formats:
            path = os.path.join(args.out, 'blk_%d_%016x.png' % (fmt, h))
            if not os.path.exists(path):
                Image.fromarray(decode(fmt, texels), 'RGBA').save(path)
                written += 1
    print(f'{len(sources)} table entries, {len(seen)} distinct blocks ({skipped} blank skipped), '
          f'{written} new block images in {args.out}')


if __name__ == '__main__':
    main()

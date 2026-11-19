#!/usr/bin/env python3
"""Recover the decrypted asset stream from a Game Maker 7.0 exe.

Layout (GM 7.0 runner):
  header @ magic 1234321, version 700, debug flag
  settings (raw fields, version 702)
  D3DX8.dll (name + len + zlib data)
  u32 len + zlib blob  ->  [garbage1 n] [garbage2 n] [g1*4 junk] [u32 seed]
                           [g2*4 junk] [1 clear byte] [GMKrypt-encoded data to end]
Algorithm per gm_reader (elipsitz/gm_reader, src/decoder/decrypt.rs, gmkrypt_decrypt).

usage: gm7decrypt.py <game.exe> <out.bin>
"""
import struct
import sys
import zlib

MAGIC = 1234321


def find_header(d):
    # GM runners scan in steps of 10000 bytes for the magic value.
    for off in range(0, len(d) - 8, 10000):
        if struct.unpack_from('<2I', d, off) == (MAGIC, 700):
            return off
    raise SystemExit('GM7 header (1234321, 700) not found')


def gmkrypt_table(seed):
    """Reverse swap table from a seed (GMKrypt, same as GMK 7.0 files)."""
    a = 6 + seed % 250
    b = seed // 250
    t = list(range(256))
    for i in range(1, 10001):
        j = 1 + (i * a + b) % 254
        t[j], t[j + 1] = t[j + 1], t[j]
    rev = [0] * 256
    for i in range(1, 256):
        rev[t[i]] = i
    return rev


def decrypt(blob):
    g1, g2 = struct.unpack_from('<2I', blob, 0)
    p = 8 + g1 * 4
    (seed,) = struct.unpack_from('<I', blob, p)
    p += 4 + g2 * 4
    rev = gmkrypt_table(seed)
    # first byte is stored in the clear; the rest are rev[b] - absolute offset
    out = bytearray(blob[p:])
    base = p + 1
    for i in range(1, len(out)):
        out[i] = (rev[out[i]] - (base + i - 1)) & 0xFF
    return bytes(out)


def main():
    exe, out = sys.argv[1], sys.argv[2]
    d = open(exe, 'rb').read()
    h = find_header(d)
    dll = d.find(b'D3DX8.dll', h)
    if dll < 0:
        raise SystemExit('embedded D3DX8.dll not found')
    p = dll + 9
    (n,) = struct.unpack_from('<I', d, p)
    p += 4 + n
    (n,) = struct.unpack_from('<I', d, p)
    blob = zlib.decompress(d[p + 4:p + 4 + n])
    data = decrypt(blob)
    open(out, 'wb').write(data)
    print(f'header@{h} settings={dll - 4 - h - 12}B blob={len(blob)}B assets={len(data)}B')


if __name__ == '__main__':
    main()

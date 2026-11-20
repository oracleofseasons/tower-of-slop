#!/usr/bin/env python3
"""Build an instrumented copy of the original GM7 exe by splicing GML into event code.

GM 7.0 asset data is read sequentially with length-prefixed strings and no absolute
offsets, so a code string can be replaced with a longer one as long as its u32 length
prefix is updated. The stream is then re-encrypted (GMKrypt) and re-compressed.

usage:
  gm7patch.py <in.exe> <out.exe> <patch.json>
  gm7patch.py --roundtrip <in.exe>          (check decrypt/encrypt is lossless)

patch.json: [{"find": "<exact existing code>", "replace": "<new code>"}, ...]
Each "find" must match exactly one length-prefixed string in the asset stream.
"""
import json
import struct
import sys
import os
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gm7decrypt as dec  # noqa: E402


def locate(d):
    h = dec.find_header(d)
    dll = d.find(b'D3DX8.dll', h)
    p = dll + 9
    (n,) = struct.unpack_from('<I', d, p)
    p += 4 + n
    (n,) = struct.unpack_from('<I', d, p)
    return p, n   # offset of the blob's u32 length, blob length


def split(blob):
    g1, g2 = struct.unpack_from('<2I', blob, 0)
    p = 8 + g1 * 4
    (seed,) = struct.unpack_from('<I', blob, p)
    p += 4 + g2 * 4
    return p, seed


def encrypt(header, plain, seed, p):
    rev = dec.gmkrypt_table(seed)
    fwd = [0] * 256
    for i, r in enumerate(rev):
        fwd[r] = i
    out = bytearray(plain)
    base = p + 1
    for i in range(1, len(out)):
        out[i] = fwd[(out[i] + (base + i - 1)) & 0xFF]
    return header + bytes(out)


def rebuild(d, plain):
    lp, n = locate(d)
    blob = zlib.decompress(d[lp + 4:lp + 4 + n])
    p, seed = split(blob)
    new_blob = encrypt(blob[:p], plain, seed, p)
    z = zlib.compress(new_blob, 9)
    return d[:lp] + struct.pack('<I', len(z)) + z + d[lp + 4 + n:]


def decrypted(d):
    lp, n = locate(d)
    return dec.decrypt(zlib.decompress(d[lp + 4:lp + 4 + n]))


def splice(plain, find, replace):
    fb = find.encode('latin-1')
    rb = replace.encode('latin-1')
    needle = struct.pack('<I', len(fb)) + fb
    i = plain.find(needle)
    if i < 0 or plain.find(needle, i + 1) >= 0:
        raise SystemExit(f'patch target must occur exactly once (found {plain.count(needle)}): {find[:60]!r}')
    return plain[:i] + struct.pack('<I', len(rb)) + rb + plain[i + len(needle):]


def main():
    if sys.argv[1] == '--roundtrip':
        d = open(sys.argv[2], 'rb').read()
        lp, n = locate(d)
        blob = zlib.decompress(d[lp + 4:lp + 4 + n])
        p, seed = split(blob)
        again = encrypt(blob[:p], dec.decrypt(blob), seed, p)
        print('roundtrip', 'OK' if again == blob else 'MISMATCH', len(blob), 'bytes')
        return
    src, dst, patch = sys.argv[1:4]
    d = open(src, 'rb').read()
    plain = decrypted(d)
    for item in json.load(open(patch)):
        plain = splice(plain, item['find'], item['replace'])
    open(dst, 'wb').write(rebuild(d, plain))
    print(f'wrote {dst}')


if __name__ == '__main__':
    main()

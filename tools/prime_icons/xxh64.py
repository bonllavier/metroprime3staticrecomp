"""Pure-Python XXH64 (seeded), matching xxHash's reference implementation."""
import struct

P1 = 0x9E3779B185EBCA87
P2 = 0xC2B2AE3D27D4EB4F
P3 = 0x165667B19E3779F9
P4 = 0x85EBCA77C2B2AE63
P5 = 0x27D4EB2F165667C5
M = 0xFFFFFFFFFFFFFFFF


def rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & M


def rnd(acc, inp):
    acc = (acc + inp * P2) & M
    acc = rotl(acc, 31)
    return (acc * P1) & M


def merge(acc, val):
    val = rnd(0, val)
    acc ^= val
    return (acc * P1 + P4) & M


def xxh64(data, seed=0):
    n = len(data)
    i = 0
    if n >= 32:
        v1 = (seed + P1 + P2) & M
        v2 = (seed + P2) & M
        v3 = seed & M
        v4 = (seed - P1) & M
        limit = n - 32
        while i <= limit:
            a, b, c, d = struct.unpack_from('<QQQQ', data, i)
            v1 = rnd(v1, a); v2 = rnd(v2, b); v3 = rnd(v3, c); v4 = rnd(v4, d)
            i += 32
        h = (rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18)) & M
        h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4)
    else:
        h = (seed + P5) & M
    h = (h + n) & M
    while i + 8 <= n:
        k = struct.unpack_from('<Q', data, i)[0]
        h ^= rnd(0, k)
        h = (rotl(h, 27) * P1 + P4) & M
        i += 8
    if i + 4 <= n:
        k = struct.unpack_from('<I', data, i)[0]
        h ^= (k * P1) & M
        h = (rotl(h, 23) * P2 + P3) & M
        i += 4
    while i < n:
        h ^= (data[i] * P5) & M
        h = (rotl(h, 11) * P1) & M
        i += 1
    h ^= h >> 33
    h = (h * P2) & M
    h ^= h >> 29
    h = (h * P3) & M
    h ^= h >> 32
    return h


if __name__ == '__main__':
    assert xxh64(b'') == 0xEF46DB3751D8E999, hex(xxh64(b''))
    assert xxh64(b'a') == 0xD24EC4F1A98C6E5B, hex(xxh64(b'a'))
    assert xxh64(b'abc') == 0x44BC2CF5AD770999, hex(xxh64(b'abc'))
    assert xxh64(bytes(range(100))) == xxh64(bytes(range(100)))
    print('xxh64 vectors ok')

"""Read-only probe of Prime 3 (RM3E01) pak files: sections, named resources, compression."""
import struct, glob, os, sys, zlib, collections

ROOT = os.environ.get("PRIME_ICONS_FILES", r"G:/gamecube_roms/metroid3_wii/recomp/.tools/disc-extraction-rm3e01/files")


def parse_pak(path):
    f = open(path, 'rb')
    head = f.read(0x40)
    version, header_size = struct.unpack('>II', head[:8])
    assert version == 2 and header_size == 0x40, (path, version, header_size)
    f.seek(0x40)
    count = struct.unpack('>I', f.read(4))[0]
    sections = []
    for _ in range(count):
        name = f.read(4).decode('ascii')
        size = struct.unpack('>I', f.read(4))[0]
        sections.append((name, size))
    # section data starts at the next 64-byte boundary
    pos = f.tell()
    pos = (pos + 0x3F) & ~0x3F
    offsets = {}
    for name, size in sections:
        offsets[name] = (pos, size)
        pos = (pos + size + 0x3F) & ~0x3F
    # STRG: named resources
    names = []
    o, s = offsets['STRG']
    f.seek(o)
    n = struct.unpack('>I', f.read(4))[0]
    for _ in range(n):
        chars = b''
        while True:
            c = f.read(1)
            if c == b'\x00':
                break
            chars += c
        typ = f.read(4).decode('ascii')
        aid = struct.unpack('>Q', f.read(8))[0]
        names.append((chars.decode('ascii', 'replace'), typ, aid))
    # RSHD: resource headers
    o, s = offsets['RSHD']
    f.seek(o)
    n = struct.unpack('>I', f.read(4))[0]
    res = {}
    for _ in range(n):
        comp, typ, aid, size, off = struct.unpack('>I4sQII', f.read(24))
        res[aid] = (comp, typ.decode('ascii'), size, off)
    data_off = offsets['DATA'][0]
    return f, sections, names, res, data_off


def read_resource(f, res, data_off, aid):
    comp, typ, size, off = res[aid]
    f.seek(data_off + off)
    raw = f.read(size)
    if not comp:
        return raw, 'raw'
    assert raw[:4] == b'CMPD', raw[:16].hex()
    blocks = struct.unpack('>I', raw[4:8])[0]
    p = 8
    out = b''
    kinds = []
    hdrs = []
    for _ in range(blocks):
        csize, dsize = struct.unpack('>II', raw[p:p + 8])
        hdrs.append((csize, dsize))
        p += 8
    for csize, dsize in hdrs:
        flag = csize >> 24
        csize &= 0xFFFFFF
        chunk = raw[p:p + csize]
        p += csize
        if csize == dsize:
            out += chunk
            kinds.append('stored')
        elif chunk[:1] == b'\x78':
            out += zlib.decompress(chunk)
            kinds.append('zlib')
        else:
            kinds.append(f'other:{flag:x}:{chunk[:2].hex()}')
            out += chunk
    return out, ','.join(sorted(set(kinds)))


if __name__ == '__main__':
    pat = sys.argv[1].lower() if len(sys.argv) > 1 else 'icon'
    total = collections.Counter()
    for path in sorted(glob.glob(ROOT + '/*.pak')):
        f, sections, names, res, data_off = parse_pak(path)
        txtr_named = [(n, t, a) for n, t, a in names if t == 'TXTR']
        hits = [(n, t, a) for n, t, a in names if pat in n.lower() or 'button' in n.lower()]
        strgs = [a for a, (c, t, s, o) in res.items() if t == 'STRG']
        total['STRG'] += len(strgs)
        print(f"{os.path.basename(path)}: sections={[s[0] for s in sections]} names={len(names)} named TXTR={len(txtr_named)} resources={len(res)} STRG={len(strgs)}")
        for n, t, a in hits:
            comp, typ, size, off = res.get(a, (None, '?', 0, 0))
            print(f"   {n} {t} {a:016x} comp={comp} size={size}")
        f.close()

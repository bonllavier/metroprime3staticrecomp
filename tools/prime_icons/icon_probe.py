"""Read-only: decompress named button textures, decode TXTR to PNG, scan STRG text for image tags."""
import struct, glob, os, sys, zlib, collections, re, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pak_probe import parse_pak, read_resource, ROOT

OUT = os.environ.get('PRIME_ICONS_OUT', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out'))
os.makedirs(OUT, exist_ok=True)


def write_png(path, w, h, rgba):
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    raw = b''.join(b'\x00' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def decode_block_fmt(fmt, data, w, h, palette=None):
    """Decode GX tiled texture to RGBA list. Returns bytearray w*h*4."""
    out = bytearray(w * h * 4)
    def put(x, y, r, g, b, a):
        if x < w and y < h:
            i = (y * w + x) * 4
            out[i:i + 4] = bytes((r, g, b, a))
    def rgb5a3(v):
        if v & 0x8000:
            r = (v >> 10) & 0x1f; g = (v >> 5) & 0x1f; b = v & 0x1f
            return (r * 255 // 31, g * 255 // 31, b * 255 // 31, 255)
        a = (v >> 12) & 7; r = (v >> 8) & 0xf; g = (v >> 4) & 0xf; b = v & 0xf
        return (r * 17, g * 17, b * 17, a * 255 // 7)
    def rgb565(v):
        return ((v >> 11) * 255 // 31, ((v >> 5) & 0x3f) * 255 // 63, (v & 0x1f) * 255 // 31, 255)
    p = 0
    if fmt in (0, 8):  # I4, C4: 8x8 blocks, 4bpp
        bw, bh = 8, 8
        for by in range(0, h, bh):
            for bx in range(0, w, bw):
                for y in range(bh):
                    for x in range(0, bw, 2):
                        v = data[p]; p += 1
                        for k, idx in ((0, v >> 4), (1, v & 0xf)):
                            if fmt == 0:
                                put(bx + x + k, by + y, idx * 17, idx * 17, idx * 17, 255)
                            else:
                                put(bx + x + k, by + y, *palette[idx])
    elif fmt in (1, 9):  # I8, C8: 8x4 blocks
        bw, bh = 8, 4
        for by in range(0, h, bh):
            for bx in range(0, w, bw):
                for y in range(bh):
                    for x in range(bw):
                        v = data[p]; p += 1
                        if fmt == 1:
                            put(bx + x, by + y, v, v, v, 255)
                        else:
                            put(bx + x, by + y, *palette[v])
    elif fmt == 2:  # IA4
        bw, bh = 8, 4
        for by in range(0, h, bh):
            for bx in range(0, w, bw):
                for y in range(bh):
                    for x in range(bw):
                        v = data[p]; p += 1
                        i = (v & 0xf) * 17; a = (v >> 4) * 17
                        put(bx + x, by + y, i, i, i, a)
    elif fmt in (3, 4, 5):  # IA8, RGB565, RGB5A3: 4x4 blocks, 16bpp
        bw, bh = 4, 4
        for by in range(0, h, bh):
            for bx in range(0, w, bw):
                for y in range(bh):
                    for x in range(bw):
                        v = struct.unpack('>H', data[p:p + 2])[0]; p += 2
                        if fmt == 3:
                            put(bx + x, by + y, v & 0xff, v & 0xff, v & 0xff, v >> 8)
                        elif fmt == 4:
                            put(bx + x, by + y, *rgb565(v))
                        else:
                            put(bx + x, by + y, *rgb5a3(v))
    elif fmt == 6:  # RGBA8: 4x4 blocks, AR then GB
        bw, bh = 4, 4
        for by in range(0, h, bh):
            for bx in range(0, w, bw):
                ar = data[p:p + 32]; gb = data[p + 32:p + 64]; p += 64
                for y in range(bh):
                    for x in range(bw):
                        k = (y * 4 + x) * 2
                        put(bx + x, by + y, ar[k + 1], gb[k], gb[k + 1], ar[k])
    elif fmt == 10:  # CMPR
        bw, bh = 8, 8
        for by in range(0, h, bh):
            for bx in range(0, w, bw):
                for sy in (0, 4):
                    for sx in (0, 4):
                        c0, c1, bits = struct.unpack('>HHI', data[p:p + 8]); p += 8
                        cols = [rgb565(c0), rgb565(c1)]
                        if c0 > c1:
                            cols.append(tuple((2 * cols[0][i] + cols[1][i]) // 3 for i in range(3)) + (255,))
                            cols.append(tuple((cols[0][i] + 2 * cols[1][i]) // 3 for i in range(3)) + (255,))
                        else:
                            cols.append(tuple((cols[0][i] + cols[1][i]) // 2 for i in range(3)) + (255,))
                            cols.append((0, 0, 0, 0))
                        for y in range(4):
                            for x in range(4):
                                idx = (bits >> (30 - 2 * (y * 4 + x))) & 3
                                put(bx + sx + x, by + sy + y, *cols[idx])
    else:
        raise ValueError(f'format {fmt}')
    return out, p


# Retro TXTR format enum -> GX texture format (the number Aurora's key/filename uses).
RETRO_TO_GX = {0: 0, 1: 1, 2: 2, 3: 3, 4: 8, 5: 9, 6: 10, 7: 4, 8: 5, 9: 6, 10: 14}


def decode_txtr(data):
    rfmt, w, h, mips = struct.unpack('>IHHI', data[:12])
    fmt = RETRO_TO_GX[rfmt]
    if fmt == 14:
        fmt = 10  # local decoder id for CMPR
    p = 12
    palette = None
    pal_raw = None
    if fmt in (8, 9):
        pfmt, pw, ph = struct.unpack('>IHH', data[p:p + 8]); p += 8
        n = pw * ph
        pal_raw = data[p:p + n * 2]
        vals = struct.unpack(f'>{n}H', pal_raw); p += n * 2
        if pfmt == 0:
            palette = [(v & 0xff,) * 3 + (v >> 8,) for v in vals]
        elif pfmt == 1:
            palette = [((v >> 11) * 255 // 31, ((v >> 5) & 0x3f) * 255 // 63, (v & 0x1f) * 255 // 31, 255) for v in vals]
        else:
            def rgb5a3(v):
                if v & 0x8000:
                    return (((v >> 10) & 0x1f) * 255 // 31, ((v >> 5) & 0x1f) * 255 // 31, (v & 0x1f) * 255 // 31, 255)
                return (((v >> 8) & 0xf) * 17, ((v >> 4) & 0xf) * 17, (v & 0xf) * 17, ((v >> 12) & 7) * 255 // 7)
            palette = [rgb5a3(v) for v in vals]
    img = data[p:]
    rgba, used = decode_block_fmt(fmt, img, w, h, palette)
    return fmt, w, h, mips, rgba, img[:used], pal_raw


if __name__ == '__main__':
    kinds = collections.Counter()
    report = []
    tag_hits = []
    for path in sorted(glob.glob(ROOT + '/*.pak')):
        base = os.path.basename(path)
        f, sections, names, res, data_off = parse_pak(path)
        for n, t, a in names:
            if t == 'TXTR' and ('button' in n.lower() or 'icon' in n.lower()) and 'Bookmark' not in n and 'SaveIcon' not in n and 'Site' not in n and 'Station' not in n:
                data, kind = read_resource(f, res, data_off, a)
                kinds[kind] += 1
                if 'other' in kind:
                    report.append((base, n, kind))
                    continue
                try:
                    fmt, w, h, mips, rgba, raw, pal = decode_txtr(data)
                except Exception as e:
                    report.append((base, n, f'decode error {e}'))
                    continue
                out = os.path.join(OUT, f'{base[:-4]}_{n}.png')
                write_png(out, w, h, rgba)
                report.append((base, n, f'{a:016x}', f'fmt={fmt} {w}x{h} mips={mips} base={len(raw)}B pal={len(pal) if pal else 0}'))
        # STRG scan (limit per pak for speed; Logbook/Metroid* are the big ones)
        strgs = [(a, v) for a, v in res.items() if v[1] == 'STRG']
        for a, v in strgs:
            data, kind = read_resource(f, res, data_off, a)
            kinds['STRG:' + kind] += 1
            if b'&image=' in data or b'\x00&\x00i\x00m\x00a\x00g\x00e' in data:
                for m in re.finditer(rb'&image=([^;]{1,120});', data):
                    tag_hits.append((base, f'{a:016x}', m.group(1).decode('latin1')))
                for m in re.finditer(rb'(?:\x00&\x00i\x00m\x00a\x00g\x00e\x00=)((?:\x00[\x20-\x7e]){1,120})\x00;', data):
                    tag_hits.append((base, f'{a:016x}', 'u16:' + m.group(1).replace(b'\x00', b'').decode('latin1')))
        f.close()
    print('compression kinds:', dict(kinds))
    for r in report:
        print(' ', *r)
    print('image tags:', len(tag_hits))
    c = collections.Counter(t[2] for t in tag_hits)
    for k, v in c.most_common(60):
        print(f'  {v:4d} {k}')
    json.dump(tag_hits, open(os.path.join(OUT, 'image_tags.json'), 'w'), indent=1)

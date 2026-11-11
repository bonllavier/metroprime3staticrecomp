"""LZO1X decompression, Prime 3 CMPD resources, TXTR decode and contact sheet (read-only on game files)."""
import struct, glob, os, sys, zlib, collections, re, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pak_probe import parse_pak, ROOT
from icon_probe import decode_txtr, write_png, OUT


def lzo1x_decompress(src, expected=None):
    ip = 0
    op = bytearray()
    n = len(src)

    def copy_match(dist, length):
        start = len(op) - dist
        if start < 0:
            raise ValueError('lzo: bad match distance')
        for i in range(length):
            op.append(op[start + i])

    t = src[ip]; ip += 1
    if t > 17:
        t -= 17
        short = t < 4  # match_next semantics: a short literal, then any match code
        op += src[ip:ip + t]; ip += t
        t = src[ip]; ip += 1
        if t < 16 and not short:
            # first_literal_run: 3-byte match at least 0x801 back
            dist = (1 + 0x800) + (t >> 2) + (src[ip] << 2); ip += 1
            copy_match(dist, 3)
            t = src[ip - 2] & 3
            if t == 0:
                t = src[ip]; ip += 1
                state = 'literal'
            else:
                op += src[ip:ip + t]; ip += t
                t = src[ip]; ip += 1
                state = 'match'
        else:
            state = 'match'
    else:
        state = 'literal'
    while True:
        if state == 'literal':
            if t < 16:
                if t == 0:
                    while src[ip] == 0:
                        t += 255; ip += 1
                    t += 15 + src[ip]; ip += 1
                op += src[ip:ip + t + 3]; ip += t + 3
                t = src[ip]; ip += 1
                if t < 16:
                    dist = (1 + 0x800) + (t >> 2) + (src[ip] << 2); ip += 1
                    copy_match(dist, 3)
                    # match done handling below
                    t = src[ip - 2] & 3
                    if t == 0:
                        t = src[ip]; ip += 1
                        state = 'literal'
                        continue
                    op += src[ip:ip + t]; ip += t
                    t = src[ip]; ip += 1
                    state = 'match'
                    continue
            state = 'match'
        # match with code t (>= 16, or short match when t < 16 after a short literal)
        if t >= 64:
            dist = 1 + ((t >> 2) & 7) + (src[ip] << 3); ip += 1
            length = (t >> 5) + 1
            copy_match(dist, length)
        elif t >= 32:
            length = t & 31
            if length == 0:
                while src[ip] == 0:
                    length += 255; ip += 1
                length += 31 + src[ip]; ip += 1
            dist = 1 + (src[ip] >> 2) + (src[ip + 1] << 6); ip += 2
            copy_match(dist, length + 2)
        elif t >= 16:
            dist = (t & 8) << 11
            length = t & 7
            if length == 0:
                while src[ip] == 0:
                    length += 255; ip += 1
                length += 7 + src[ip]; ip += 1
            dist += (src[ip] >> 2) + (src[ip + 1] << 6); ip += 2
            if dist == 0:
                break  # end marker
            dist += 0x4000
            copy_match(dist, length + 2)
        else:
            dist = 1 + (t >> 2) + (src[ip] << 2); ip += 1
            copy_match(dist, 2)
        t = src[ip - 2] & 3
        if t == 0:
            t = src[ip]; ip += 1
            state = 'literal'
        else:
            op += src[ip:ip + t]; ip += t
            t = src[ip]; ip += 1
            state = 'match'
    if expected is not None and len(op) != expected:
        raise ValueError(f'lzo: size {len(op)} != {expected}')
    return bytes(op)


def read_resource_lzo(f, res, data_off, aid):
    comp, typ, size, off = res[aid]
    f.seek(data_off + off)
    raw = f.read(size)
    if not comp:
        return raw
    assert raw[:4] == b'CMPD'
    blocks = struct.unpack('>I', raw[4:8])[0]
    p = 8
    hdrs = []
    for _ in range(blocks):
        csize, dsize = struct.unpack('>II', raw[p:p + 8]); p += 8
        hdrs.append((csize & 0xFFFFFF, dsize))
    out = b''
    for csize, dsize in hdrs:
        chunk = raw[p:p + csize]; p += csize
        if csize == dsize:
            out += chunk
        else:
            # Prime 2/3 segmented LZO: s16 size per segment (negative = stored), 16 KiB each.
            q = 0
            remaining = dsize
            while remaining > 0 and q < len(chunk):
                seg = struct.unpack('>h', chunk[q:q + 2])[0]; q += 2
                want = min(0x4000, remaining)
                if seg < 0:
                    out += chunk[q:q - seg]; q += -seg; remaining -= -seg
                else:
                    out += lzo1x_decompress(chunk[q:q + seg], want); q += seg; remaining -= want
    return out


def contact_sheet(images, path, scale=6, pad=4):
    """images: list of (label, w, h, rgba). Writes one PNG, nearest-neighbour upscaled, in a row grid."""
    cols = 9
    cell_w = max(w for _, w, h, _ in images) * scale + pad
    cell_h = max(h for _, w, h, _ in images) * scale + pad
    rows = (len(images) + cols - 1) // cols
    W, H = cols * cell_w, rows * cell_h
    sheet = bytearray(b'\x40\x40\x40\xff' * (W * H))
    for i, (label, w, h, rgba) in enumerate(images):
        cx, cy = (i % cols) * cell_w, (i // cols) * cell_h
        for y in range(h * scale):
            for x in range(w * scale):
                sx, sy = x // scale, y // scale
                k = (sy * w + sx) * 4
                r, g, b, a = rgba[k:k + 4]
                # composite over dark grey
                r = (r * a + 0x40 * (255 - a)) // 255; g = (g * a + 0x40 * (255 - a)) // 255; b = (b * a + 0x40 * (255 - a)) // 255
                j = ((cy + y) * W + cx + x) * 4
                sheet[j:j + 4] = bytes((r, g, b, 255))
    write_png(path, W, H, sheet)


if __name__ == '__main__':
    # index every TXTR id across paks
    index = {}
    paks = {}
    for path in sorted(glob.glob(ROOT + '/*.pak')):
        f, sections, names, res, data_off = parse_pak(path)
        paks[path] = (f, names, res, data_off)
        for a, (c, t, s, o) in res.items():
            if t == 'TXTR' and a not in index:
                index[a] = path
    wanted = collections.OrderedDict()
    # named button/icon textures
    for path, (f, names, res, data_off) in paks.items():
        for n, t, a in names:
            if t == 'TXTR' and ('button' in n.lower() or n.startswith('TXTR_Icon')):
                wanted[a] = n
    # ids from image tags
    tags = [(h["pak"], h["strg"], h["tag"]) for h in json.load(open(os.path.join(OUT, "image_tags.json")))]
    tag_ids = collections.Counter()
    for base, strg, tag in tags:
        for m in re.finditer(r'0x([0-9A-Fa-f]{16})', tag):
            tag_ids[int(m.group(1), 16)] += 1
    for a, c in tag_ids.most_common():
        wanted.setdefault(a, f'tag x{c}')
    images = []
    meta = []
    for a, label in wanted.items():
        path = index.get(a)
        if not path:
            meta.append((label, f'{a:016x}', 'NOT FOUND in any pak'))
            continue
        f, names, res, data_off = paks[path]
        try:
            data = read_resource_lzo(f, res, data_off, a)
            fmt, w, h, mips, rgba, raw, pal = decode_txtr(data)
        except Exception as e:
            meta.append((label, f'{a:016x}', os.path.basename(path), f'ERROR {e}'))
            continue
        images.append((label, w, h, rgba))
        meta.append((label, f'{a:016x}', os.path.basename(path), f'fmt={fmt} {w}x{h} mips={mips} base={len(raw)}B pal={len(pal) if pal else 0}B'))
        write_png(os.path.join(OUT, f'{a:016x}.png'), w, h, rgba)
    for m in meta:
        print(' ', *m)
    # sheets of 24
    for i in range(0, len(images), 24):
        contact_sheet(images[i:i + 24], os.path.join(OUT, f'sheet_{i // 24}.png'), scale=4)
        print('sheet', i // 24, [im[0] for im in images[i:i + 24]])

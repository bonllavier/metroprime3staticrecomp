"""Build id -> (size, GX format, xxh64 base-level hash, predicted replacement filename) for candidate icons."""
import glob, os, sys, json, struct, collections, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pak_probe import parse_pak, ROOT
from lzo_txtr import read_resource_lzo
from icon_probe import decode_txtr, OUT, RETRO_TO_GX
from xxh64 import xxh64

GX_BPP = {0: 4, 1: 8, 2: 8, 3: 16, 4: 16, 5: 16, 6: 32, 8: 4, 9: 8, 10: 16, 14: 4}
GX_BLOCK = {0: (8, 8), 1: (8, 4), 2: (8, 4), 3: (4, 4), 4: (4, 4), 5: (4, 4), 6: (4, 4), 8: (8, 8), 9: (8, 4), 10: (4, 4), 14: (8, 8)}


def base_level_size(fmt, w, h):
    bw, bh = GX_BLOCK[fmt]
    tw = (w + bw - 1) // bw * bw
    th = (h + bh - 1) // bh * bh
    return tw * th * GX_BPP[fmt] // 8


index = {}
paks = {}
for path in sorted(glob.glob(ROOT + '/*.pak')):
    f, sections, names, res, data_off = parse_pak(path)
    paks[path] = (f, names, res, data_off)
    for a, (c, t, s, o) in res.items():
        if t == 'TXTR' and a not in index:
            index[a] = path
named = {}
for path, (f, names, res, data_off) in paks.items():
    for n, t, a in names:
        if t == 'TXTR':
            named.setdefault(a, n)
tags = json.load(open(os.path.join(OUT, 'image_tags.json')))
tag_ids = collections.Counter()
contexts = collections.defaultdict(list)
for h in tags:
    for m in re.finditer(r'0x([0-9A-Fa-f]{16})', h['tag']):
        a = int(m.group(1), 16)
        tag_ids[a] += 1
        if len(contexts[a]) < 3:
            contexts[a].append(h['context'].strip()[:120])
wanted = []
for a, n in named.items():
    if 'button' in n.lower() or n.startswith('TXTR_Icon'):
        wanted.append(a)
for a, c in tag_ids.most_common():
    if a not in wanted:
        wanted.append(a)
table = []
for a in wanted:
    path = index.get(a)
    if not path:
        continue
    f, names, res, data_off = paks[path]
    data = read_resource_lzo(f, res, data_off, a)
    rfmt, w, h, mips = struct.unpack('>IHHI', data[:12])
    gx = RETRO_TO_GX[rfmt]
    p = 12
    tlut_hash = None
    if gx in (8, 9):
        pfmt, pw, ph = struct.unpack('>IHH', data[p:p + 8]); p += 8
        pal = data[p:p + pw * ph * 2]; p += pw * ph * 2
    size = base_level_size(gx, w, h)
    body = data[p:p + size]
    hsh = xxh64(body, 0)
    if gx in (8, 9):
        # referenced TLUT range as Aurora computes it
        if gx == 8:
            idx = [b >> 4 for b in body] + [b & 0xF for b in body]
        else:
            idx = list(body)
        lo, hi = min(idx), max(idx)
        tlut_hash = xxh64(pal[2 * lo:2 * (hi + 1)], 0)
        fname = f'tex1_{w}x{h}_{hsh:016x}_{tlut_hash:016x}_{gx}.png'
    else:
        fname = f'tex1_{w}x{h}_{hsh:016x}_{gx}.png'
    table.append({'id': f'{a:016x}', 'name': named.get(a, ''), 'pak': os.path.basename(path), 'w': w, 'h': h, 'gx_format': gx,
                  'mips': mips, 'tag_uses': tag_ids.get(a, 0), 'file': fname, 'context': contexts.get(a, [])})
json.dump(table, open(os.path.join(OUT, 'icon_table.json'), 'w'), indent=1)
for t in table:
    print(f"{t['id']} {t['name'] or '-':28s} {t['pak']:14s} {t['w']}x{t['h']} fmt={t['gx_format']} uses={t['tag_uses']:3d} {t['file']}")
    for c in t['context'][:1]:
        print('      ', c)

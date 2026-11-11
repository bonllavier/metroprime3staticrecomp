"""Resolve hand-picked sweep positions to texture ids, hashes and a review sheet."""
import os, sys, json, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pak_probe import parse_pak, ROOT
from lzo_txtr import read_resource_lzo, contact_sheet
from icon_probe import decode_txtr, OUT, RETRO_TO_GX
from icon_table import base_level_size
from xxh64 import xxh64

PICKS = {
    'FrontEnd': {0: [6, 7]},
    'GuiDVD': {0: [0, 8, 10, 15, 16, 19, 21, 22, 26, 31, 34], 1: [4, 17, 18, 19, 20, 26, 31, 32, 33, 34],
               2: [1, 3, 7, 8, 12, 13, 14, 15, 16, 23, 27, 29, 31, 33, 35], 3: [2, 4, 8, 10, 14, 18, 27]},
    'GuiNAND': {0: [0, 4, 18, 23], 1: [9, 14, 21, 25], 2: [5, 10, 13, 14, 17, 24, 30], 3: [1, 7, 8, 18, 20, 21, 22], 4: [1]},
    'MiscData': {0: [10, 11, 16, 17, 19, 22, 24, 25, 26, 28, 31], 1: [4, 5, 7, 11, 15, 23]},
    'NoARAM': {0: [4]},
}
known = {t['file'] for t in json.load(open(os.path.join(OUT, 'icon_table.json')))}
rows = []
images = []
seen_files = set()
for pak, sheets in PICKS.items():
    order = json.load(open(os.path.join(OUT, f'sweep_{pak}.json')))
    f, sections, names, res, data_off = parse_pak(os.path.join(ROOT, pak + '.pak'))
    for sheet, positions in sheets.items():
        for pos in positions:
            entry = order[sheet * 36 + pos]
            a = int(entry['id'], 16)
            data = read_resource_lzo(f, res, data_off, a)
            rfmt, w, h, mips = struct.unpack('>IHHI', data[:12])
            gx = RETRO_TO_GX[rfmt]
            p = 12
            if gx in (8, 9):
                pfmt, pw, ph = struct.unpack('>IHH', data[p:p + 8]); p += 8 + pw * ph * 2
            body = data[p:p + base_level_size(gx, w, h)]
            fname = f'tex1_{w}x{h}_{xxh64(body, 0):016x}_{gx}'
            dup = fname in seen_files
            seen_files.add(fname)
            fmt, w2, h2, mips2, rgba, raw, pal = decode_txtr(data)
            idx = len(rows)
            rows.append({'idx': idx, 'pak': pak, 'sheet': sheet, 'pos': pos, 'id': entry['id'], 'name': entry['name'],
                         'w': w, 'h': h, 'gx': gx, 'file': fname, 'known': fname in known, 'dup': dup})
            images.append((str(idx), w, h, rgba))
    f.close()
json.dump(rows, open(os.path.join(OUT, 'candidates.json'), 'w'), indent=1)
for i in range(0, len(images), 45):
    contact_sheet(images[i:i + 45], os.path.join(OUT, f'cand_{i // 45}.png'), scale=3)
for r in rows:
    flag = 'KNOWN' if r['known'] else ('dup' if r['dup'] else 'new')
    print(f"{r['idx']:3d} {r['pak']:9s} s{r['sheet']}p{r['pos']:<2d} {r['id']} {r['name'] or '-':26s} {r['w']}x{r['h']} gx={r['gx']:2d} {flag:5s} {r['file']}")

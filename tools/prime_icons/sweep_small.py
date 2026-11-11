"""Decode every small texture (<= 64 px) of the GUI paks into contact sheets to spot button icons by eye."""
import glob, os, sys, struct, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pak_probe import parse_pak, ROOT
from lzo_txtr import read_resource_lzo, contact_sheet
from icon_probe import decode_txtr, OUT, RETRO_TO_GX

PAKS = sys.argv[1].split(',') if len(sys.argv) > 1 else ['FrontEnd', 'GuiDVD', 'GuiNAND', 'MiscData', 'NoARAM', 'InGameNAND', 'InGameDVD']
known = {t['id'] for t in json.load(open(os.path.join(OUT, 'icon_table.json')))}
os.makedirs(OUT, exist_ok=True)
for pak in PAKS:
    path = os.path.join(ROOT, pak + '.pak')
    f, sections, names, res, data_off = parse_pak(path)
    named = {a: n for n, t, a in names}
    images = []
    order = []
    for a, (c, t, s, o) in sorted(res.items()):
        if t != 'TXTR':
            continue
        try:
            data = read_resource_lzo(f, res, data_off, a)
            rfmt, w, h, mips = struct.unpack('>IHHI', data[:12])
            if w > 64 or h > 64:
                continue
            fmt, w, h, mips, rgba, raw, pal = decode_txtr(data)
        except Exception as e:
            print(pak, f'{a:016x}', 'ERR', e)
            continue
        images.append((f'{a:016x}', w, h, rgba))
        order.append({'id': f'{a:016x}', 'name': named.get(a, ''), 'w': w, 'h': h, 'gx': fmt if fmt != 10 else 14, 'known': f'{a:016x}' in known})
    for i in range(0, len(images), 36):
        contact_sheet(images[i:i + 36], os.path.join(OUT, f'sweep_{pak}_{i // 36}.png'), scale=3)
    json.dump(order, open(os.path.join(OUT, f'sweep_{pak}.json'), 'w'), indent=1)
    print(pak, len(images), 'small textures,', (len(images) + 35) // 36, 'sheets')
    f.close()

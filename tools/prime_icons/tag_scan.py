"""Decompress every STRG and collect &image= tags with the surrounding sentence (read-only)."""
import glob, os, sys, re, json, collections, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pak_probe import parse_pak, ROOT
from lzo_txtr import read_resource_lzo
from icon_probe import OUT

hits = []
errors = collections.Counter()
for path in sorted(glob.glob(ROOT + '/*.pak')):
    base = os.path.basename(path)
    f, sections, names, res, data_off = parse_pak(path)
    for a, (c, t, s, o) in res.items():
        if t != 'STRG':
            continue
        try:
            data = read_resource_lzo(f, res, data_off, a)
        except Exception as e:
            errors[str(e)[:40]] += 1
            continue
        # Prime 3 STRG v3 stores UTF-8 strings; find tags and ~60 chars of context
        for m in re.finditer(rb'&image=([^;]{1,160});', data):
            start = max(0, m.start() - 70)
            ctx = data[start:m.end() + 70]
            ctx = re.sub(rb'[^\x20-\x7e&;=,.]', b' ', ctx).decode('ascii')
            hits.append({'pak': base, 'strg': f'{a:016x}', 'tag': m.group(1).decode('latin1', 'replace'), 'context': ctx})
    f.close()
json.dump(hits, open(os.path.join(OUT, 'image_tags.json'), 'w'), indent=1)
print('tags', len(hits), 'errors', dict(errors))
c = collections.Counter(h['tag'] for h in hits)
for k, v in c.most_common(80):
    print(f'{v:4d} {k}')
# one context sample per distinct tag
seen = set()
for h in hits:
    key = h['tag']
    if key in seen:
        continue
    seen.add(key)
    print('---', key, '|', h['pak'], '|', h['context'][:200])

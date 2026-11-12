#!/usr/bin/env python3
"""Hashes of RM3E01's Wii Remote/Nunchuk motion prompts for runtime/src/hle/prime_motion_text.cpp (read-only).

Reads every STRG v3 table of the extracted paks (PRIME_ICONS_FILES, like the other scripts here),
finds the prompts by their English text and prints, for each of the five languages (Japanese
repeats English), the FNV-1a 64 of the exact UTF-8 string with its prompt index. The runtime
matches strings by these hashes, so no game text is copied into the source.
"""
import glob, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pak_probe import parse_pak, ROOT
from lzo_txtr import read_resource_lzo

# Prompt index -> a fragment that identifies its English text (order = kPrompts in prime_motion_text.cpp).
PROMPTS = [
    'Twist the Wii Remote clockwise.', 'Push the Wii Remote toward the screen.', 'Twist the Wii Remote counterclockwise.',
    'Pull the Wii Remote away from the screen.', 'Push the Wii Remote toward the screen to activate thrusters.',
    'and cast Nunchuk forward to fire.', 'quickly tilt the Wii Remote upward to perform a Spring Ball Jump.',
    'Quickly pull Nunchuk backward', 'Cast Nunchuk forward', 'Shake the Nunchuk up and down to break free!',
    'Point the Wii Remote at the screen.', 'Twist the Wii Remote.', 'Tilt the Wii Remote down.', 'Tilt the Wii Remote up.',
    'Grapple the power coupling',
]
LANGS = {'ENGL': 0, 'JAPN': 0, 'GERM': 1, 'FREN': 2, 'SPAN': 3, 'ITAL': 4}


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def strings(data):
    magic, version, langs, count = struct.unpack_from('>IIII', data, 0)
    if magic != 0x87654321 or version != 3:
        return None
    name_count, name_size = struct.unpack_from('>II', data, 16)
    pos = 24
    ids = [data[pos + 4 * i:pos + 4 * i + 4].decode() for i in range(langs)]
    pos += 4 * langs
    offsets = []
    for _ in range(langs):
        pos += 4
        offsets.append(struct.unpack_from('>' + 'I' * count, data, pos))
        pos += 4 * count
    base = pos + (name_size + 8 * name_count if name_count else 0)
    out = {}
    for lang, offs in zip(ids, offsets):
        out[lang] = [data[base + o + 4:base + o + 4 + struct.unpack_from('>I', data, base + o)[0]].rstrip(b'\0') for o in offs]
    return out


def match(english: str):
    # Longest fragment first: "toward the screen." is also inside the thrusters prompt.
    for index in sorted(range(len(PROMPTS)), key=lambda i: -len(PROMPTS[i])):
        if PROMPTS[index] in english:
            return index
    return None


# MOTION_TEXT_ROOTS: several extracted file trees separated by os.pathsep, e.g. RM3E01's files and
# Trilogy's files/MP3 (RS5.PAK). A string's hash is the same wherever it comes from, so the
# table simply grows; Trilogy prompts worded differently get their own rows.
roots = os.environ.get('MOTION_TEXT_ROOTS', ROOT).split(os.pathsep)
paks = sorted({p for root in roots for p in glob.glob(os.path.join(root, '*.pak')) + glob.glob(os.path.join(root, '*.PAK'))})
rows = {}
for path in paks:
    f, sections, names, res, data_off = parse_pak(path)
    for aid, (_, kind, _, _) in res.items():
        if kind != 'STRG':
            continue
        try:
            table = strings(read_resource_lzo(f, res, data_off, aid))
        except Exception:
            continue
        if not table or 'ENGL' not in table:
            continue
        for i, english in enumerate(table['ENGL']):
            prompt = match(english.decode('utf-8', 'replace'))
            if prompt is None or len(english) > 300:
                continue
            for lang, index in LANGS.items():
                if lang in table:
                    rows[fnv1a64(table[lang][i])] = (prompt, index)
    f.close()
found = sorted({p for p, _ in rows.values()})
if found != list(range(len(PROMPTS))):
    sys.exit(f'prompts not found: {sorted(set(range(len(PROMPTS))) - set(found))}')
for h, (prompt, lang) in sorted(rows.items(), key=lambda r: (r[1], r[0])):
    print(f'    {{0x{h:016X}ull, {prompt}, {lang}}},')
print(f'// {len(rows)} strings', file=sys.stderr)

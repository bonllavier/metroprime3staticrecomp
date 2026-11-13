#!/usr/bin/env python3
"""Localized text fixes for runtime/src/hle/prime_text_fixes.cpp (read-only over extracted discs).

The USA discs carry German, French, Spanish and Italian tables, but some strings were left in English
and others have typos (see text_fixes_data.py). This script reads every STRG v3 table of the given
extracted trees, applies the corrections of text_fixes_data.py and writes two tables for the runtime:

- prime_text_fixes_rules.inc: the corrections (fragment and replacement, or a whole new string);
- prime_text_fixes_table.inc: for each string that changes, the FNV-1a 64 of its exact original
  UTF-8 text, its language and the corrections to apply.

The runtime matches strings by hash and language, so the original game text is not part of the
source: only the short fragments and the texts written for the port.

A fragment matches only at word boundaries (the byte before and after the match is not an ASCII
letter nor part of a UTF-8 sequence), so "Bryo" leaves "Bryyo" alone. The runtime applies the same
test, left to right, one rule after another.

Usage (TEXT_FIX_ROOTS separated by os.pathsep; the default covers RM3E01 and Trilogy USA):
  TEXT_FIX_ROOTS="<RM3E01 files>:<R3ME01 files>" python tools/prime_icons/text_fixes.py \
      third_party/WiiCompiled/runtime/src/hle
"""
import glob, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pak_probe import parse_pak
from lzo_txtr import read_resource_lzo
import text_fixes_data as data

LANGUAGE_INDEX = {'GERM': 1, 'FREN': 2, 'SPAN': 3, 'ITAL': 4}  # same order as prime_motion_text.cpp
MAX_RULES_PER_STRING = 8  # kMaxRules in prime_text_fixes.cpp
MAX_TEXT = 8191  # kMaxGuestText - 1 in prime_motion_text.cpp


def fnv1a64(text: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in text:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def is_letter(byte: int) -> bool:
    return 0x41 <= byte <= 0x5A or 0x61 <= byte <= 0x7A or byte >= 0x80


def apply_rule(text: bytes, find: bytes, repl: bytes):
    """Same algorithm as ApplyRule in prime_text_fixes.cpp: left to right, non-overlapping."""
    out, pos, hits = bytearray(), 0, 0
    while True:
        at = text.find(find, pos)
        if at < 0:
            break
        before = text[at - 1] if at > 0 else 0
        after = text[at + len(find)] if at + len(find) < len(text) else 0
        if (is_letter(find[0]) and is_letter(before)) or (is_letter(find[-1]) and is_letter(after)):
            out += text[pos:at + 1]
            pos = at + 1
            continue
        out += text[pos:at] + repl
        pos = at + len(find)
        hits += 1
    out += text[pos:]
    return bytes(out), hits


def strg_tables(blob):
    """STRG v3: header, optional name table (its size covers entries and names), language ids,
    per-language size and offsets, then the strings (u32 length + bytes)."""
    magic, version, langs, count = struct.unpack_from('>IIII', blob, 0)
    if magic != 0x87654321 or version != 3:
        return None
    _, name_size = struct.unpack_from('>II', blob, 16)
    pos = 24 + name_size
    ids = [blob[pos + 4 * i:pos + 4 * i + 4].decode('ascii') for i in range(langs)]
    pos += 4 * langs
    offsets = []
    for _ in range(langs):
        pos += 4
        offsets.append(struct.unpack_from('>' + 'I' * count, blob, pos))
        pos += 4 * count
    return {lang: [blob[pos + o + 4:pos + o + 4 + struct.unpack_from('>I', blob, pos + o)[0]].rstrip(b'\0')
                   for o in offs] for lang, offs in zip(ids, offsets)}


def tables(roots):
    """Yields (pak path, asset id, {language: [strings]}) for every STRG under the roots."""
    for root in roots:
        paks = sorted(p for p in glob.glob(os.path.join(root, '**', '*'), recursive=True)
                      if p.lower().endswith('.pak'))
        for path in paks:
            f, _, _, res, data_off = parse_pak(path)
            for aid, (_, kind, _, _) in res.items():
                if kind == 'STRG':
                    table = strg_tables(read_resource_lzo(f, res, data_off, aid))
                    if table:
                        yield path, aid, table
            f.close()


LEADING_TAGS = re.compile(rb'^(?:&[a-z-]+(?:=[^;&]*)?;)*')


def whole_text(original: bytes, text) -> bytes:
    if isinstance(text, tuple):
        return text[1].encode()
    lead = LEADING_TAGS.match(original).group(0)
    tail = b'\n' if original.endswith(b'\n') else b''
    return lead + text.encode() + tail


def build(roots):
    rules = []  # (language, find or None, replacement)
    rule_index = {}

    def rule(lang, find, repl):
        key = (lang, find, repl)
        if key not in rule_index:
            rule_index[key] = len(rules)
            rules.append(key)
        return rule_index[key]

    fragments = {lang: [(f.encode(), r.encode()) for f, r in pairs] for lang, pairs in data.FRAGMENTS.items()}
    scoped = {lang: [(s, f.encode(), r.encode()) for s, f, r in items] for lang, items in data.SCOPED.items()}
    whole = {lang: {key: (guard.encode(), text) for key, guard, text in items} for lang, items in data.WHOLE.items()}
    used = set()
    fixes, changes = {}, []
    for path, aid, table in tables(roots):
        asset = '%016X' % aid
        for lang in LANGUAGE_INDEX:
            for index, original in enumerate(table.get(lang, ())):
                key = f'{asset}#{index}'
                applied, current = [], original
                entry = whole.get(lang, {}).get(key)
                if entry and entry[0] in original:
                    current = whole_text(original, entry[1])
                    applied.append(rule(lang, None, current))
                    used.add(('W', lang, key))
                else:
                    for n, (find, repl) in enumerate(fragments.get(lang, ())):
                        current, hits = apply_rule(current, find, repl)
                        if hits:
                            applied.append(rule(lang, find, repl))
                            used.add(('F', lang, n))
                    for n, (scope, find, repl) in enumerate(scoped.get(lang, ())):
                        if scope != asset and scope != key:
                            continue
                        current, hits = apply_rule(current, find, repl)
                        if hits:
                            applied.append(rule(lang, find, repl))
                            used.add(('S', lang, n))
                if not applied:
                    continue
                if len(applied) > MAX_RULES_PER_STRING:
                    sys.exit(f'{len(applied)} rules on {key} {lang} (max {MAX_RULES_PER_STRING})')
                if len(current) > MAX_TEXT:
                    sys.exit(f'{key} {lang}: corrected text is {len(current)} bytes (max {MAX_TEXT})')
                slot = (fnv1a64(original), LANGUAGE_INDEX[lang])
                if slot in fixes and fixes[slot] != applied:
                    sys.exit(f'two corrections for one string: {key} {lang}')
                if slot not in fixes:
                    fixes[slot] = applied
                    changes.append((os.path.basename(path), key, lang, original, current))
    unused_fragments = [(lang, f) for lang, pairs in data.FRAGMENTS.items()
                        for n, (f, _) in enumerate(pairs) if ('F', lang, n) not in used]
    if unused_fragments:
        sys.exit(f'fragments that match nothing: {unused_fragments}')
    unused = [(lang, s, f) for lang, items in data.SCOPED.items() for n, (s, f, _) in enumerate(items)
              if ('S', lang, n) not in used]
    unused += [(lang, key) for lang, items in data.WHOLE.items() for key, _, _ in items if ('W', lang, key) not in used]
    return rules, fixes, changes, unused


def c_string(text: bytes) -> str:
    out = []
    for ch in text.decode('utf-8'):
        out.append({'\\': '\\\\', '"': '\\"', '\n': '\\n', '\t': '\\t'}.get(ch, ch))
    return '"' + ''.join(out) + '"'


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    default = os.pathsep.join(['recomp/.tools/disc-extraction-rm3e01/files', 'recomp/.tools/disc-extraction-r3me01/files'])
    roots = os.environ.get('TEXT_FIX_ROOTS', default).split(os.pathsep)
    if not all(os.path.isdir(r) for r in roots):
        sys.exit(f'missing extracted trees: {roots}')
    rules, fixes, changes, unused = build(roots)
    out = sys.argv[1]
    header = '// Generated by tools/prime_icons/text_fixes.py from tools/prime_icons/text_fixes_data.py. Do not edit.\n'
    with open(os.path.join(out, 'prime_text_fixes_rules.inc'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(header + '// {language, fragment (nullptr: the whole string), replacement}\n')
        for i, (lang, find, repl) in enumerate(rules):
            find_text = 'nullptr' if find is None else c_string(find)
            f.write(f'    {{{LANGUAGE_INDEX[lang]}, {find_text}, {c_string(repl)}}},  // {i}\n')
    with open(os.path.join(out, 'prime_text_fixes_table.inc'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(header + '// {FNV-1a 64 of the original string, language, rule indices}, sorted by hash and language\n')
        for (h, lang), applied in sorted(fixes.items()):
            f.write(f'    {{0x{h:016X}ull, {lang}, {{{", ".join(map(str, applied))}}}}},\n')
    report = os.environ.get('TEXT_FIX_REPORT')
    if report:  # every change, for review (contains game text: keep it private)
        with open(report, 'w', encoding='utf-8') as f:
            for pak, key, lang, before, after in sorted(changes):
                f.write(f'## {pak} {key} {lang}\n- {before.decode()!r}\n+ {after.decode()!r}\n')
    for item in unused:
        print('not needed on these discs:', item, file=sys.stderr)
    print(f'{len(fixes)} strings, {len(rules)} rules', file=sys.stderr)


if __name__ == '__main__':
    main()

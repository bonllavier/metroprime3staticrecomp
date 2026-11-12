#!/usr/bin/env python3
"""Sets one key of a Config.toml in place: set_config.py PATH section.key=value (value written as given)."""
import re
import sys

path, setting = sys.argv[1], sys.argv[2]
key, value = setting.split('=', 1)
section, name = key.split('.', 1)
out, current, done = [], None, False
for line in open(path, encoding='utf-8').read().splitlines():
    header = re.match(r'\s*\[([^\]]+)\]', line)
    if header:
        if current == section and not done:
            out.append(f'{name} = {value}')
            done = True
        current = header.group(1)
    elif current == section and re.match(rf'\s*{re.escape(name)}\s*=', line):
        line = f'{name} = {value}'
        done = True
    out.append(line)
if not done:
    if current != section:
        out.append(f'[{section}]')
    out.append(f'{name} = {value}')
open(path, 'w', encoding='utf-8').write('\n'.join(out) + '\n')

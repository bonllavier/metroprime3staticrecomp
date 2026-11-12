#!/usr/bin/env python3
"""Summarize an in-process stack profile (F8 in the performance overlay).

Each sample is a native unwind of the profiled thread. Self counts attribute
the innermost frame; inclusive counts every function present in the stack
once per sample. EXE frames are named with llvm-nm; DLL frames by module.
Results are a sampling estimate of where the thread spends wall time while
running (a suspended/waiting thread is sampled too), not exact CPU time.
"""
import argparse
from bisect import bisect_right
from collections import Counter
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
NM = ROOT / 'recomp/.tools/portable-tools/llvm-mingw/bin/llvm-nm.exe'
CXXFILT = ROOT / 'recomp/.tools/portable-tools/llvm-mingw/bin/llvm-cxxfilt.exe'
# Linux profiles (ELF executable): the host's llvm-nm/nm and llvm-cxxfilt/c++filt.
if not NM.exists():
    NM = Path(shutil.which('llvm-nm') or shutil.which('nm') or 'nm')
    CXXFILT = Path(shutil.which('llvm-cxxfilt') or shutil.which('c++filt') or 'c++filt')


def load_symbols(exe):
    out = subprocess.run([str(NM), '--numeric-sort', '--defined-only', str(exe)],
                         capture_output=True, text=True, check=True).stdout
    pairs = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'tT':
            pairs.append((int(parts[0], 16), parts[2]))
    return [a for a, _ in pairs], [n for _, n in pairs]


def demangle(names):
    if not names:
        return {}
    out = subprocess.run([str(CXXFILT)], input='\n'.join(names), capture_output=True,
                         text=True, check=True).stdout.splitlines()
    return dict(zip(names, out))


def shorten(name, width):
    name = re.sub(r'std::__1::', 'std::', name)
    return name if len(name) <= width else name[:width - 3] + '...'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('profile', type=Path)
    p.add_argument('--top', type=int, default=45)
    p.add_argument('--width', type=int, default=110)
    p.add_argument('--focus', help='regex: only samples whose stack contains a matching frame')
    p.add_argument('--callers', help='regex: show caller breakdown of matching functions')
    p.add_argument('--tid', type=int, help='thread to summarize (default: the primary thread)')
    p.add_argument('--window', help='seconds since capture start, "from,to": only samples in that interval')
    a = p.parse_args()

    modules, samples, header, tids, times = [], [], '', [], []
    exe_path = None
    now = 0.0
    for line in a.profile.read_text(encoding='utf-8', errors='replace').splitlines():
        if line.startswith('module '):
            _, base, size, path = line.split(' ', 3)
            modules.append((int(base, 16), int(size, 16), path))
        elif line.startswith('s '):
            parts = line[2:].split()
            # v3 sweep marker. The writer uses the default float precision, so from 100 s on the
            # value has no decimal point ("s 119997"); a one-frame stack is a long hex address.
            if len(parts) == 1 and (('.' in parts[0]) or (parts[0].isdigit() and len(parts[0]) <= 7)):
                now = float(parts[0])  # v3: sweep marker, milliseconds since the capture started
                continue
            samples.append([int(x, 16) for x in parts])
            tids.append(None)
            times.append(now)
        elif line.startswith('t '):
            parts = line.split()
            tids.append(int(parts[1]))
            samples.append([int(x, 16) for x in parts[2:]])
            times.append(now)
        elif line.startswith('tid '):
            header = line
        elif line.startswith('exe_size '):
            _, size, path = line.split(' ', 2)
            exe_path = path
            if Path(path).exists() and Path(path).stat().st_size != int(size):
                raise SystemExit(f'{path} was rebuilt since this profile; symbols would be wrong')
    modules.sort()
    present = Counter(t for t in tids if t is not None)
    if present:
        primary = a.tid if a.tid is not None else int(header.split()[1])
        print('threads: ' + ', '.join(f'{t}={c}' for t, c in present.most_common()))
        keep = [i for i, t in enumerate(tids) if t == primary]
        samples = [samples[i] for i in keep]
        times = [times[i] for i in keep]
    if a.window:
        lo, hi = (float(x) * 1000.0 for x in a.window.split(','))
        samples = [s for s, t in zip(samples, times) if lo <= t <= hi]
    # Windows: the .exe module, linked at 0x140000000. Linux: the module named by exe_size, a PIE
    # whose symbol values are offsets from its load base.
    exe = next((m for m in modules if m[2].lower().endswith('.exe')), None)
    preferred = 0x140000000
    if exe is None and exe_path:
        exe = next((m for m in modules if m[2] == exe_path), None)
        preferred = 0
    addresses, names = load_symbols(Path(exe[2])) if exe else ([], [])

    cache = {}

    def name_of(address):
        if address in cache:
            return cache[address]
        result = None
        i = bisect_right([m[0] for m in modules], address) - 1
        if i >= 0 and address - modules[i][0] < modules[i][1]:
            base, _, path = modules[i]
            if exe and base == exe[0]:
                k = bisect_right(addresses, address - base + preferred) - 1
                result = names[k] if k >= 0 else 'exe?'
            else:
                result = Path(path).name
        cache[address] = result or 'unknown'
        return cache[address]

    named = [[name_of(f) for f in s] for s in samples]
    if a.focus:
        rx = re.compile(a.focus)
        named = [s for s in named if any(rx.search(n) for n in s)]
    total = len(named)
    self_counts, incl = Counter(), Counter()
    for s in named:
        self_counts[s[0]] += 1
        for n in set(s):
            incl[n] += 1
    pretty = demangle(list(set(self_counts) | set(incl)))
    print(header)
    print(f'samples={total}' + (f' focus={a.focus}' if a.focus else ''))
    print('\n== self ==')
    for n, c in self_counts.most_common(a.top):
        print(f'{100 * c / total:6.2f}%  {c:6d}  {shorten(pretty.get(n, n), a.width)}')
    print('\n== inclusive ==')
    for n, c in incl.most_common(a.top):
        print(f'{100 * c / total:6.2f}%  {c:6d}  {shorten(pretty.get(n, n), a.width)}')
    if a.callers:
        rx = re.compile(a.callers)
        callers = Counter()
        for s in named:
            for i, n in enumerate(s[:-1]):
                if rx.search(n) or rx.search(pretty.get(n, n)):
                    callers[(n, s[i + 1])] += 1
                    break
        print(f'\n== callers of /{a.callers}/ ==')
        extra = demangle([c for _, c in callers])
        for (n, c), k in callers.most_common(a.top):
            print(f'{100 * k / total:6.2f}%  {shorten(pretty.get(n, n), 50)}  <-  '
                  f'{shorten(extra.get(c, c), a.width - 50)}')


if __name__ == '__main__':
    main()

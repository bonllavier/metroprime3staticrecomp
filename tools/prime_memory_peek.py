"""Read-only peek into the running Corruption_3 PC.exe guest memory (MEM1) from outside.

Finds the host mapping of MEM1 by scanning for the first bytes of main.dol's
text section, then dumps MEM1 (24 MiB) and, optionally, the CPlayer object
located by two of its pointer fields. Nothing is written to the process.

  python -I recomp/tools/prime_memory_peek.py --out <dir> [--player-state 0x809954c0 --player-34e8 0x808c0f80]
"""
import argparse, ctypes, ctypes.wintypes as wt, struct, os, sys, time

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MEM_COMMIT = 0x1000
PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100
MEM1_SIZE = 24 * 1024 * 1024

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.ReadProcessMemory.restype = wt.BOOL
k32.ReadProcessMemory.argtypes = [wt.HANDLE, wt.LPCVOID, wt.LPVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualQueryEx.restype = ctypes.c_size_t


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p), ('AllocationProtect', wt.DWORD),
                ('PartitionId', wt.WORD), ('RegionSize', ctypes.c_size_t), ('State', wt.DWORD), ('Protect', wt.DWORD), ('Type', wt.DWORD)]


k32.VirtualQueryEx.argtypes = [wt.HANDLE, wt.LPCVOID, ctypes.POINTER(MBI), ctypes.c_size_t]


def find_pid(name='Corruption_3 PC.exe'):
    out = os.popen('tasklist /FI "IMAGENAME eq %s" /FO CSV /NH' % name).read()
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower() == name.lower():
            return int(parts[1])
    return None


def read(handle, address, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(handle, ctypes.c_void_p(address), buf, size, ctypes.byref(got)):
        return None
    return buf.raw[:got.value]


def regions(handle):
    address = 0
    mbi = MBI()
    while address < 0x7FFFFFFFFFFF:
        if not k32.VirtualQueryEx(handle, ctypes.c_void_p(address), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        base_addr = mbi.BaseAddress or 0
        if mbi.State == MEM_COMMIT and not (mbi.Protect & PAGE_GUARD) and mbi.Protect != PAGE_NOACCESS:
            yield base_addr, mbi.RegionSize
        address = base_addr + mbi.RegionSize


def dol_text_signature(dol_path):
    d = open(dol_path, 'rb').read()
    offs = struct.unpack('>18I', d[:0x48]); addrs = struct.unpack('>18I', d[0x48:0x90]); sizes = struct.unpack('>18I', d[0x90:0xd8])
    # second text section (0x80004000 region) is the large one; use the first non-empty
    for o, a, s in zip(offs, addrs, sizes):
        if s and a == 0x80004000:
            return a, d[o:o + 256]
    for o, a, s in zip(offs, addrs, sizes):
        if s:
            return a, d[o:o + 256]
    raise SystemExit('no text section')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--dol', default=os.path.join(os.path.dirname(__file__), '..', 'input', 'RM3E01_00', 'main.dol'))
    ap.add_argument('--player-state', type=lambda x: int(x, 16), default=None, help='value of CPlayer +0x35A0 (hex)')
    ap.add_argument('--player-34e8', type=lambda x: int(x, 16), default=None, help='value of CPlayer +0x34E8 (hex)')
    ap.add_argument('--label', default='snapshot')
    ap.add_argument('--no-mem1', action='store_true')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    pid = find_pid()
    if not pid:
        raise SystemExit('Corruption_3 PC.exe is not running')
    handle = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not handle:
        raise SystemExit(f'OpenProcess failed: {ctypes.get_last_error()}')
    sig_addr, sig = dol_text_signature(args.dol)
    t = time.time()
    base = None
    for start, size in regions(handle):
        if size < MEM1_SIZE:
            continue
        # scan in 4 MiB chunks for the signature
        chunk = 4 << 20
        off = 0
        while off < size and base is None:
            data = read(handle, start + off, min(chunk + 256, size - off))
            if not data:
                break
            i = data.find(sig)
            if i != -1:
                host_sig = start + off + i
                base = host_sig - (sig_addr - 0x80000000)
            off += chunk
        if base is not None:
            break
    if base is None:
        raise SystemExit('MEM1 mapping not found')
    print(f'pid {pid}: MEM1 host base 0x{base:x} (found in {time.time() - t:.1f}s)')
    mem1 = read(handle, base, MEM1_SIZE)
    if mem1 is None or len(mem1) != MEM1_SIZE:
        raise SystemExit('MEM1 read failed')
    if not args.no_mem1:
        path = os.path.join(args.out, f'mem1_{args.label}.bin')
        open(path, 'wb').write(mem1)
        print('wrote', path)
    if args.player_state is not None:
        needle = struct.pack('>I', args.player_state)
        cands = []
        i = mem1.find(needle)
        while i != -1:
            p = i - 0x35A0
            if p >= 0 and p % 4 == 0:
                ok = True
                if args.player_34e8 is not None:
                    ok = struct.unpack('>I', mem1[p + 0x34E8:p + 0x34EC])[0] == args.player_34e8
                if ok:
                    cands.append(p)
            i = mem1.find(needle, i + 1)
        print('CPlayer candidates:', [hex(0x80000000 + c) for c in cands])
        for c in cands[:1]:
            path = os.path.join(args.out, f'player_{args.label}.bin')
            open(path, 'wb').write(mem1[c:c + 0x4000])
            print('wrote', path, 'guest', hex(0x80000000 + c))


if __name__ == '__main__':
    main()

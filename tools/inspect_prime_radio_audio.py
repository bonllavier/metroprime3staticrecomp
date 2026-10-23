"""Read-only RM3E01_00 radio/audio diagnostics; no suspension, input or RAM dumps.

Layouts are partial, verified against the original DOL. The process must be the
owned WiiCompiled Prime runtime. Concurrent guest updates can change a snapshot.
"""
import argparse
import ctypes as c
import ctypes.wintypes as w
import json
import struct
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p,
                                        c.c_size_t, c.POINTER(c.c_size_t)]
    kernel.ReadProcessMemory.restype = w.BOOL
    kernel.CloseHandle.argtypes = [w.HANDLE]
    handle = kernel.OpenProcess(0x10, False, args.pid)  # VM_READ only
    if not handle:
        raise c.WinError(c.get_last_error())

    def read(address, length):
        if not (0x80000000 <= address and address + length <= 0x81800000 or
                0x90000000 <= address and address + length <= 0x94000000):
            raise ValueError(f'Outside Prime RAM: {address:#x}+{length:#x}')
        data = c.create_string_buffer(length)
        size = c.c_size_t()
        if not kernel.ReadProcessMemory(handle, 0x100000000000 + address,
                                        data, length, c.byref(size)) or size.value != length:
            raise c.WinError(c.get_last_error())
        return data.raw

    def word(address):
        return struct.unpack('>I', read(address, 4))[0]

    def number(address):
        return struct.unpack('>f', read(address, 4))[0]

    result = {'pid': args.pid, 'revision': 'RM3E01_00', 'read_only': True,
              'atomic_snapshot': False, 'groups': [], 'loaded_speech_actors': [],
              'threads': []}
    try:
        # The AX command-list routine's first instruction is a revision guard.
        if word(0x804BFB18) != 0x9421FFE0:
            raise ValueError('Unexpected guest code at RM3E01_00 AX entry')
        for index, name in enumerate(('sfx', 'music', 'speech')):
            group = word(0x805CC2CC + 4 * index)
            result['groups'].append({
                'name': name, 'address': hex(group),
                'requested_volume': number(0x805CC2EC + 4 * index),
                'current_volume': number(group + 0x34) if group else None,
                'channel_list_empty': word(group + 0x20) == group + 0x20 if group else None})
        # CScriptStreamedAudio partial vtable; scan MEM1 transiently, never save it.
        memory = read(0x80000000, 0x1800000)
        pattern = struct.pack('>I', 0x80597B60)
        offset = memory.find(pattern)
        while offset >= 0:
            if offset % 4 == 0 and offset + 0x114 <= len(memory):
                actor = 0x80000000 + offset
                sample = word(actor + 0x10C)
                if word(actor + 0xF0) == 2 and sample:
                    sound = word(sample + 8)
                    row = {'actor': hex(actor), 'flags': read(actor + 0xDC, 2).hex(),
                           'sample': hex(sample), 'sound': hex(sound),
                           'channel_handle': hex(word(actor + 0x110))}
                    if sound:
                        getter = word(word(sound + 0xBC) + 0x68)
                        row['state_getter'] = hex(getter)
                        if getter == 0x80427990:
                            row['open_state'] = word(sound + 0xA8)
                    result['loaded_speech_actors'].append(row)
            offset = memory.find(pattern, offset + 4)
        thread = word(0x800000DC)
        seen = set()
        while thread and thread not in seen and len(seen) < 64:
            seen.add(thread)
            result['threads'].append({
                'address': hex(thread),
                'state': struct.unpack('>H', read(thread + 0x2C8, 2))[0],
                'suspend_count': word(thread + 0x2CC),
                'stack': hex(word(thread + 4)),
                'queue': hex(word(thread + 0x2DC))})
            thread = word(thread + 0x2FC)
        result['file_callback_list_empty'] = word(0x8067DE94) == 0
    finally:
        kernel.CloseHandle(handle)
    output = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(output, encoding='utf-8')
    print(output, end='')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Bounded read-only x64 thread sample; always resume the exact suspended thread."""
from __future__ import annotations

import argparse
import bisect
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--thread", type=int, required=True)
    parser.add_argument("--image-base", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert c.sizeof(c.c_void_p) == 8, "Use native x64 Python"
    exe = ROOT / "recomp/build/RM3E01_00_runtime_link_probe/Corruption_3 PC.exe"
    image = exe.read_bytes()
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    assert image[pe:pe + 4] == b"PE\0\0"
    assert struct.unpack_from("<H", image, pe + 24)[0] == 0x20B
    preferred_base = struct.unpack_from("<Q", image, pe + 48)[0]
    image_size = struct.unpack_from("<I", image, pe + 80)[0]
    section_count = struct.unpack_from("<H", image, pe + 6)[0]
    section_table = pe + 24 + struct.unpack_from("<H", image, pe + 20)[0]
    executable_ranges = []
    for index in range(section_count):
        section = section_table + index * 40
        if struct.unpack_from("<I", image, section + 36)[0] & 0x20000000:
            size, address = struct.unpack_from("<II", image, section + 8)
            executable_ranges.append((address, address + size))
    nm = ROOT / "recomp/.tools/portable-tools/llvm-mingw/bin/llvm-nm.exe"
    symbols = []
    persistent_cpu = None
    text = subprocess.check_output([str(nm), "--defined-only", "--numeric-sort", str(exe)], text=True)
    for line in text.splitlines():
        if "g_persistentCpu" in line:
            persistent_cpu = int(line.split()[0], 16) - preferred_base + args.image_base
        match = re.match(r"([0-9a-fA-F]+) [tT] (.*)", line)
        if match:
            symbols.append((int(match[1], 16), match[2]))
    symbols.sort()
    addresses = [entry[0] for entry in symbols]

    def resolve(address):
        rva = address - args.image_base
        if not any(start <= rva < end for start, end in executable_ranges):
            return None
        original = address - args.image_base + preferred_base
        index = bisect.bisect_right(addresses, original) - 1
        if index < 0:
            return None
        base, name = symbols[index]
        return {"symbol": name, "offset": hex(original - base), "image_va": hex(original)}

    kernel = c.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = (w.DWORD, w.BOOL, w.DWORD)
    kernel.OpenProcess.restype = w.HANDLE
    kernel.OpenThread.argtypes = (w.DWORD, w.BOOL, w.DWORD)
    kernel.OpenThread.restype = w.HANDLE
    kernel.GetThreadContext.argtypes = (w.HANDLE, c.c_void_p)
    kernel.GetThreadContext.restype = w.BOOL
    kernel.SuspendThread.argtypes = (w.HANDLE,)
    kernel.SuspendThread.restype = w.DWORD
    kernel.ResumeThread.argtypes = (w.HANDLE,)
    kernel.ResumeThread.restype = w.DWORD
    kernel.ReadProcessMemory.argtypes = (w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t))
    kernel.ReadProcessMemory.restype = w.BOOL
    kernel.CloseHandle.argtypes = (w.HANDLE,)
    process = kernel.OpenProcess(0x410, False, args.pid)
    thread = kernel.OpenThread(0x4A, False, args.thread)
    if not process or not thread:
        raise c.WinError(c.get_last_error())
    samples = []
    log_text = (args.output.parent / "console.log").read_text(encoding="utf-8", errors="replace")
    guest_base = int(re.search(r"guest address space reserved at (0x[0-9a-f]+)", log_text, re.I)[1], 16)

    def read_memory(address, size):
        result = c.create_string_buffer(size)
        copied = c.c_size_t()
        if not kernel.ReadProcessMemory(process, address, result, size, c.byref(copied)) or copied.value != size:
            return None
        return result.raw

    def guest_backchain(stack):
        chain = []
        visited = set()
        for _ in range(16):
            if stack in visited or stack & 3 or not (0x80000000 <= stack < 0x81800000 or 0x90000000 <= stack < 0x98000000):
                break
            visited.add(stack)
            frame = read_memory(guest_base + stack, 8)
            if frame is None:
                break
            previous = int.from_bytes(frame[:4], "big")
            caller = read_memory(guest_base + previous + 4, 4) if previous else None
            chain.append({"sp": hex(stack), "previous": hex(previous),
                          "caller": hex(int.from_bytes(caller, "big")) if caller else None})
            stack = previous
        return chain
    try:
        for _ in range(3):
            # CONTEXT_AMD64 CONTROL|INTEGER. Per WinNT.h: flags@48,
            # general registers@120, RSP@152, RIP@248; CONTEXT is 16-aligned.
            storage = c.create_string_buffer(0x510)
            pointer = (c.addressof(storage) + 15) & ~15
            c.c_uint32.from_address(pointer + 48).value = 0x100003
            if kernel.SuspendThread(thread) == 0xFFFFFFFF:
                raise c.WinError(c.get_last_error())
            try:
                if not kernel.GetThreadContext(thread, pointer):
                    raise c.WinError(c.get_last_error())
                raw = c.string_at(pointer, 256)
                rip = struct.unpack_from("<Q", raw, 248)[0]
                rsp = struct.unpack_from("<Q", raw, 152)[0]
                stack = c.create_string_buffer(4096)
                copied = c.c_size_t()
                candidates = []
                if kernel.ReadProcessMemory(process, rsp, stack, len(stack), c.byref(copied)):
                    for offset in range(0, copied.value - 7, 8):
                        value = struct.unpack_from("<Q", stack.raw, offset)[0]
                        symbol = resolve(value)
                        if symbol:
                            candidates.append({"stack_offset": offset, "value": hex(value), **symbol})
                guest = None
                raw_cpu = read_memory(persistent_cpu, 464) if persistent_cpu else None
                if raw_cpu:
                    gprs = struct.unpack_from("<32I", raw_cpu)
                    guest = {"r1": hex(gprs[1]), "r13": hex(gprs[13]),
                             "lr": hex(struct.unpack_from("<I", raw_cpu, 132)[0]),
                             "msr": hex(struct.unpack_from("<I", raw_cpu, 460)[0]),
                             "backchain": guest_backchain(gprs[1])}
                samples.append({"rip": hex(rip), "rsp": hex(rsp), "location": resolve(rip),
                                "persistent_guest_context": guest, "stack_code_candidates": candidates})
            finally:
                if kernel.ResumeThread(thread) == 0xFFFFFFFF:
                    raise c.WinError(c.get_last_error())
            time.sleep(0.05)
    finally:
        kernel.CloseHandle(thread)
        kernel.CloseHandle(process)
    report = {"pid": args.pid, "thread": args.thread, "image_base": hex(args.image_base),
              "executable_sha256": hashlib.sha256(image).hexdigest(), "samples": samples,
              "note": "Raw stack code references are candidates, not an unwound call stack. No guest state is written."}
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "locations": [item["location"] for item in samples]}, indent=2))


if __name__ == "__main__":
    main()

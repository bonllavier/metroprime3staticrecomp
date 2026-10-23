#!/usr/bin/env python3
"""Capture native thread registers/stacks from a live, manually driven Prime run.

No input injection or guest-state edits. Each thread is briefly suspended for
one register/stack snapshot and resumed in finally. Stack words are candidates,
not an unwound call stack. Output remains private.
"""
import argparse
from bisect import bisect_right
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time


def symbolize(report):
    """Resolve addresses inside the exact EXE after correcting its ASLR base."""
    exe=Path(report['executable'])
    data=exe.read_bytes();pe=struct.unpack_from('<I',data,0x3C)[0]
    if hashlib.sha256(data).hexdigest()!=report['executable_sha256']:
        raise ValueError('Executable changed after capture; refusing mismatched symbols')
    if data[pe:pe+4]!=b'PE\0\0' or struct.unpack_from('<H',data,pe+24)[0]!=0x20B:
        raise ValueError('Expected a PE32+ executable')
    preferred=struct.unpack_from('<Q',data,pe+24+24)[0]
    image_size=struct.unpack_from('<I',data,pe+24+56)[0]
    section_count=struct.unpack_from('<H',data,pe+6)[0]
    section_table=pe+24+struct.unpack_from('<H',data,pe+20)[0]
    executable_ranges=[]
    for index in range(section_count):
        section=section_table+index*40
        virtual_size,rva,raw_size=struct.unpack_from('<III',data,section+8)
        flags=struct.unpack_from('<I',data,section+36)[0]
        if flags&0x20000000:executable_ranges.append((preferred+rva,preferred+rva+max(virtual_size,raw_size)))
    module=next(m for m in report['modules'] if Path(m['path'])==exe)
    actual=int(module['base'],16)
    root=Path(__file__).resolve().parents[2]
    nm=root/'recomp/.tools/portable-tools/llvm-mingw/bin/llvm-nm.exe'
    result=subprocess.run([str(nm),'--numeric-sort','--defined-only',str(exe)],
                          check=True,capture_output=True,text=True,encoding='utf-8')
    symbols=[]
    for line in result.stdout.splitlines():
        fields=line.split(maxsplit=2)
        if len(fields)==3 and fields[1] in ('t','T'):
            address=int(fields[0],16)
            if preferred<=address<preferred+image_size:symbols.append((address,fields[2]))
    symbols.sort();addresses=[row[0] for row in symbols]
    def resolve(value):
        address=int(value,16)
        if not actual<=address<actual+image_size:return None
        nominal=address-actual+preferred;index=bisect_right(addresses,nominal)-1
        if not any(start<=nominal<end for start,end in executable_ranges):return None
        if index<0:return None
        start,name=symbols[index]
        return {'address':value,'image_address':hex(nominal),'nearest_text_symbol':name,
                'offset':hex(nominal-start)}
    for sample in report['samples']:
        if 'rip' not in sample:continue
        sample['rip_symbol']=resolve(sample['rip'])
        sample['stack_executable_address_candidates']=[row for value in sample['stack_words']
                                                       if (row:=resolve(value)) is not None]
    report['symbolization']={'preferred_base':hex(preferred),'actual_base':hex(actual),
        'text_symbols':len(symbols),'nm_sha256':hashlib.sha256(nm.read_bytes()).hexdigest(),
        'scope':'Nearest text symbols and raw stack candidates, not validated return addresses or an unwind.'}


def capture(pid, *, iterations=3, interval=.1, stack_bytes=1024):
    if not 1<=iterations<=500 or not .001<=interval<=1 or stack_bytes not in (0,1024):
        raise ValueError('Capture settings exceed bounded diagnostic limits')
    k=C.WinDLL('kernel32',use_last_error=True)
    def api(name,args,result):
        f=getattr(k,name);f.argtypes=args;f.restype=result;return f
    close=api('CloseHandle',[W.HANDLE],W.BOOL)
    op=api('OpenProcess',[W.DWORD,W.BOOL,W.DWORD],W.HANDLE)
    ot=api('OpenThread',[W.DWORD,W.BOOL,W.DWORD],W.HANDLE)
    suspend=api('SuspendThread',[W.HANDLE],W.DWORD)
    resume=api('ResumeThread',[W.HANDLE],W.DWORD)
    context=api('GetThreadContext',[W.HANDLE,C.c_void_p],W.BOOL)
    times=api('GetThreadTimes',[W.HANDLE,C.POINTER(W.FILETIME),C.POINTER(W.FILETIME),C.POINTER(W.FILETIME),C.POINTER(W.FILETIME)],W.BOOL)
    read=api('ReadProcessMemory',[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.POINTER(C.c_size_t)],W.BOOL)
    snap=api('CreateToolhelp32Snapshot',[W.DWORD,W.DWORD],W.HANDLE)
    class Entry(C.Structure):
        _fields_=[('size',W.DWORD),('usage',W.DWORD),('tid',W.DWORD),('pid',W.DWORD),
                  ('priority',W.LONG),('delta',W.LONG),('flags',W.DWORD)]
    first=api('Thread32First',[W.HANDLE,C.POINTER(Entry)],W.BOOL)
    nxt=api('Thread32Next',[W.HANDLE,C.POINTER(Entry)],W.BOOL)
    query=api('QueryFullProcessImageNameW',[W.HANDLE,W.DWORD,W.LPWSTR,C.POINTER(W.DWORD)],W.BOOL)
    if C.sizeof(C.c_void_p)!=8:raise RuntimeError('Requires native 64-bit Python')
    process=op(0x410,False,pid)  # query information + VM_READ
    if not process:raise C.WinError(C.get_last_error())
    try:
        path=C.create_unicode_buffer(32768);size=W.DWORD(len(path))
        if not query(process,0,path,C.byref(size)):raise C.WinError(C.get_last_error())
        exe=Path(path.value)
        if exe.name.lower()!='wiicompiled.exe':raise ValueError('Unexpected target executable')
        ps=C.WinDLL('psapi',use_last_error=True)
        class ModuleInfo(C.Structure):
            _fields_=[('base',C.c_void_p),('size',W.DWORD),('entry',C.c_void_p)]
        module_info=ps.GetModuleInformation
        module_info.argtypes=[W.HANDLE,W.HMODULE,C.POINTER(ModuleInfo),W.DWORD]
        module_info.restype=W.BOOL
        enum=ps.EnumProcessModulesEx;enum.argtypes=[W.HANDLE,C.POINTER(W.HMODULE),W.DWORD,C.POINTER(W.DWORD),W.DWORD];enum.restype=W.BOOL
        filename=ps.GetModuleFileNameExW;filename.argtypes=[W.HANDLE,W.HMODULE,W.LPWSTR,W.DWORD];filename.restype=W.DWORD
        handles=(W.HMODULE*2048)();needed=W.DWORD();modules=[]
        if not enum(process,handles,C.sizeof(handles),C.byref(needed),3):raise C.WinError(C.get_last_error())
        if needed.value>C.sizeof(handles):raise RuntimeError('Module list overflow')
        for handle in handles[:needed.value//C.sizeof(W.HMODULE)]:
            module_path=C.create_unicode_buffer(32768)
            if filename(process,handle,module_path,len(module_path)):
                info=ModuleInfo()
                if not module_info(process,handle,C.byref(info),C.sizeof(info)):
                    raise C.WinError(C.get_last_error())
                modules.append({'base':hex(handle),'size':info.size,'path':module_path.value})
        snapshot=snap(4,0)
        if snapshot==C.c_void_p(-1).value:raise C.WinError(C.get_last_error())
        tids=[]
        try:
            e=Entry();e.size=C.sizeof(e);ok=first(snapshot,C.byref(e))
            while ok:
                if e.pid==pid:tids.append(e.tid)
                ok=nxt(snapshot,C.byref(e))
        finally:close(snapshot)
        samples=[]
        for iteration in range(iterations):
            for tid in tids:
                thread=ot(0x4A,False,tid)  # QUERY_INFORMATION + GET_CONTEXT + SUSPEND_RESUME
                row={'iteration':iteration,'tid':tid}
                if not thread:
                    row['error']=C.get_last_error();samples.append(row);continue
                suspended=False
                try:
                    creation,exit_time,kernel,user=(W.FILETIME() for _ in range(4))
                    if times(thread,C.byref(creation),C.byref(exit_time),C.byref(kernel),C.byref(user)):
                        row['cpu_100ns']=((kernel.dwHighDateTime<<32)|kernel.dwLowDateTime)+((user.dwHighDateTime<<32)|user.dwLowDateTime)
                    if suspend(thread)==0xFFFFFFFF:raise C.WinError(C.get_last_error())
                    suspended=True
                    storage=C.create_string_buffer(1280+16)
                    aligned=(C.addressof(storage)+15)&~15
                    C.c_uint32.from_address(aligned+48).value=0x100003
                    if not context(thread,aligned):raise C.WinError(C.get_last_error())
                    raw=C.string_at(aligned,256)
                    rsp=struct.unpack_from('<Q',raw,152)[0]
                    row['rip']=hex(struct.unpack_from('<Q',raw,248)[0]);row['rsp']=hex(rsp)
                    # Windows x64 volatile integer registers help distinguish
                    # large source comparisons from waits without reading RAM.
                    row['integer_registers']={name:hex(struct.unpack_from('<Q',raw,offset)[0])
                        for name,offset in (('rax',120),('rcx',128),('rdx',136),
                                            ('r8',184),('r9',192),('r10',200),('r11',208))}
                    row['stack_words']=[]
                    if stack_bytes:
                        stack=C.create_string_buffer(stack_bytes);got=C.c_size_t()
                        read(process,rsp,stack,len(stack),C.byref(got))
                        row['stack_words']=[hex(v[0]) for v in struct.iter_unpack('<Q',stack.raw[:got.value//8*8])]
                except OSError as error:row['error']=str(error)
                finally:
                    if suspended:
                        if resume(thread)==0xFFFFFFFF:raise C.WinError(C.get_last_error())
                    close(thread)
                samples.append(row)
            time.sleep(interval)
        return {'pid':pid,'executable':str(exe),'executable_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),
                'modules':modules,'samples':samples,'scope':'Native register snapshots; raw stack words are not an unwound call stack. No input or guest-state edits.'}
    finally:close(process)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid',type=int,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if a.output.exists():raise FileExistsError('Refusing to overwrite evidence')
    report=capture(a.pid)
    symbolize(report)
    a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'pid':a.pid,'samples':len(report['samples']),'output':str(a.output)}))

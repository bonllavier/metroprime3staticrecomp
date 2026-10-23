#!/usr/bin/env python3
"""Verify the OSInitContext literal is an interior Prime instruction, not an entry."""
from __future__ import annotations
import hashlib, json, re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
TARGET=0x801A20BC
EXPECTED_DOL_SHA256='6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104'

def sha(path: Path)->str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main()->int:
    src=ROOT/'recomp/third_party/WiiCompiled/runtime/src/hle/os/os_thread.cpp'
    dol=ROOT/'recomp/input/RM3E01_00/main.dol'
    symbols=ROOT/'decomp/config/RM3E01_00/symbols.txt'
    audit=ROOT/'recomp/checkpoints/20261020_runtime_literal_guest_target_audit.json'
    ev=ROOT/'recomp/checkpoints/20261020_runtime_osinitcontext_literal_rejection.json'
    assert sha(dol)==EXPECTED_DOL_SHA256, 'Wrong DOL revision/hash'
    source=src.read_text(encoding='utf-8')
    match=re.search(r'InvokeIndirectCpu\(0x801A20BCu,\s*cpu\);\s*// OSInitContext',source)
    assert match, 'Expected literal OSInitContext binding not found'
    line=source.count('\n',0,match.start())+1
    assert sha(src)=='0bfb4e93423e09cebb203f74f164df4ac26caab6e502439704482f6ed5600ccf'
    ranges=[]
    rx=re.compile(r'^(.+?) = \S+:0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)',re.M)
    for m in rx.finditer(symbols.read_text(encoding='utf-8')):
        start,size=int(m.group(2),16),int(m.group(3),16)
        if size:ranges.append((start,start+size,m.group(1)))
    container=next((x for x in ranges if x[0]<=TARGET<x[1]),None)
    assert container and container[0]==0x801A1DE0 and container[1]==0x801A2904
    fmap={int(line.split(maxsplit=1)[0],16) for line in (ROOT/'recomp/config/RM3E01_00/functions.txt').read_text(encoding='utf-8').splitlines() if line.strip()}
    assert TARGET not in fmap, 'Target unexpectedly appears as an exact function entry'
    image=dol.read_bytes()
    # DOL header stores seven text then eleven data sections.
    text_off=[int.from_bytes(image[i:i+4],'big') for i in range(0,0x1c,4)]
    data_off=[int.from_bytes(image[i:i+4],'big') for i in range(0x1c,0x48,4)]
    text_addr=[int.from_bytes(image[i:i+4],'big') for i in range(0x48,0x64,4)]
    data_addr=[int.from_bytes(image[i:i+4],'big') for i in range(0x64,0x90,4)]
    text_size=[int.from_bytes(image[i:i+4],'big') for i in range(0x90,0xac,4)]
    data_size=[int.from_bytes(image[i:i+4],'big') for i in range(0xac,0xd8,4)]
    file_offset=None
    for off,addr,size in list(zip(text_off,text_addr,text_size))+list(zip(data_off,data_addr,data_size)):
        if size and addr<=TARGET<TARGET+4<=addr+size:
            file_offset=off+(TARGET-addr);break
    assert file_offset is not None, 'Target not backed by a DOL section'
    raw=image[file_offset:file_offset+4]
    assert raw.hex()=='9001002c', f'Unexpected instruction bytes {raw.hex()}'
    report=json.loads(audit.read_text(encoding='utf-8'))
    site=next(s for s in report['sites'] if s['target']=='0x801A20BC')
    assert site['line']==line and site['source_sha256']==sha(src)
    assert site['classification']=='inside_rm3e01_function_body'
    checkpoint=json.loads(ev.read_text(encoding='utf-8'))
    assert checkpoint['ghidra']['xrefs_to_target']==0
    assert checkpoint['ghidra']['function_instruction_count']==713
    assert checkpoint['ghidra']['instruction_index']==183
    assert checkpoint['runtime_source']['line']==line
    print(json.dumps({'accepted':True,'target':'0x801A20BC','source_line':line,'container':'0x801A1DE0..0x801A2904','offset':hex(TARGET-container[0]),'dol_file_offset':hex(file_offset),'instruction_bytes':raw.hex(),'exact_function_entry':False,'ghidra_xrefs_to_target':0},indent=2))
    return 0
if __name__=='__main__':raise SystemExit(main())

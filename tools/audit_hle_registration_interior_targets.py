#!/usr/bin/env python3
"""Classify active WiiCompiled HLE fixed-address registrations against whole RM3E01 functions."""
from __future__ import annotations
import argparse, hashlib, json, re
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
MAP=ROOT/'recomp/config/RM3E01_00/functions.txt'
DOL=ROOT/'recomp/input/RM3E01_00/main.dol'
PROFILE_AUDIT=ROOT/'recomp/checkpoints/20261020_prime_profile_hle_overlap.json'
EXPECTED_DOL='6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104'
SYMBOLS=ROOT/'decomp/config/RM3E01_00/symbols.txt'
HLE=ROOT/'recomp/third_party/WiiCompiled/runtime/src'
REGISTRATION=re.compile(r'\b(?:PPC_NATIVE_OVERRIDE(?:_VOID)?|REGISTER_NATIVE_FUNCTION(?:_AS)?)\s*\(\s*(?:0x)?([0-9A-Fa-f]{8})\b')
SYMBOL=re.compile(r'^(.+?) = \S+:0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)',re.M)
def sha(p:Path)->str:return hashlib.sha256(p.read_bytes()).hexdigest()
def main()->int:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--profile',choices=('all','RM3E01_00'),default='RM3E01_00')
    ap.add_argument('--output',type=Path,default=ROOT/'recomp/checkpoints/20261020_hle_registration_function_range_audit.json')
    args=ap.parse_args(); profile=args.profile=='RM3E01_00'
    if sha(DOL)!=EXPECTED_DOL:raise SystemExit('Input DOL is not pinned RM3E01_00')
    starts={int(x.split()[0],16) for x in MAP.read_text(encoding='utf-8').splitlines() if x.strip()}
    ranges=[]
    for m in SYMBOL.finditer(SYMBOLS.read_text(encoding='utf-8')):
        start,size=int(m.group(2),16),int(m.group(3),16)
        if size:ranges.append((start,start+size,m.group(1)))
    ranges.sort()
    sites=[]
    for src in sorted(HLE.rglob('*.cpp')):
        active=True; stack=[]
        for n,line in enumerate(src.read_text(encoding='utf-8',errors='replace').splitlines(),1):
            st=line.strip()
            if st.startswith('#if'):
                parent=active; is_profile='MKW_GAME_PROFILE_RM3E01_00' in st
                if is_profile and '!defined' in st: branch=not profile; stack.append((parent,branch)); active=parent and branch
                elif is_profile and 'defined' in st: branch=profile; stack.append((parent,branch)); active=parent and branch
                else: stack.append((parent,None)); active=parent
                continue
            if st.startswith('#else') and stack:
                parent,branch=stack[-1]; active=parent and (not branch if branch is not None else True); stack[-1]=(parent,None if branch is None else not branch); continue
            if st.startswith('#endif') and stack:
                parent,_=stack.pop(); active=parent; continue
            if profile and not active:continue
            for m in REGISTRATION.finditer(line):
                addr=int(m.group(1),16)
                cont=next((v for v in ranges if v[0]<=addr<v[1]),None)
                classification='exact_rm3_function_entry' if addr in starts else ('inside_sized_rm3_function' if cont else 'no_sized_function_evidence')
                sites.append({'address':f'0x{addr:08X}','source':src.relative_to(ROOT).as_posix(),'line':n,'source_line':line.strip(),'classification':classification,'container':({'name':cont[2],'start':f'0x{cont[0]:08X}','end_exclusive':f'0x{cont[1]:08X}','offset':f'+0x{addr-cont[0]:X}'} if cont else None)})
    counts=Counter(s['classification'] for s in sites)
    unique=sorted({s['address'] for s in sites})
    # Check which fixed registrations collide with the translated main direct-call closure.
    closure_path=ROOT/'recomp/checkpoints/20261020_main_direct_closure_indirect_audit.json'
    closure=json.loads(closure_path.read_text(encoding='utf-8'))
    closure_addresses={int(f['address'],16) for f in closure['functions']}
    direct_override_hits=sorted({int(s['address'],16) for s in sites}&closure_addresses)
    entry_closure_path=ROOT/'recomp/checkpoints/20261020_entry_direct_closure_audit.json'
    entry_closure=json.loads(entry_closure_path.read_text(encoding='utf-8'))
    entry_closure_addresses={int(f['address'],16) for f in entry_closure['functions']}
    entry_override_hits=sorted({int(s['address'],16) for s in sites}&entry_closure_addresses)
    # Preserve the explicit static route from the entrypoint through title init to periodic alarm setup.
    meta_path=ROOT/'recomp/generated/RM3E01_00_conservative/base_translation_output.json'
    metadata=json.loads(meta_path.read_text(encoding='utf-8'))
    by_address={x['entryPoint']:x for x in metadata['functions']}
    root=0x802D5150; constructor=0x802EB5A4; alarm=0x804DEC60
    queue=[root]; previous={root:None}
    while queue:
        cur=queue.pop(0)
        if cur==constructor:break
        for nxt in by_address.get(cur,{}).get('build',{}).get('directCallDependencies',[]):
            if nxt not in previous:previous[nxt]=cur;queue.append(nxt)
    path=[]
    if constructor in previous:
        cur=constructor
        while cur is not None:path.append(cur);cur=previous[cur]
        path.reverse()
    ctor_item=by_address.get(constructor,{})
    ctor_deps=ctor_item.get('build',{}).get('directCallDependencies',[])
    ctor_source=ROOT/'recomp/generated/RM3E01_00_conservative/functions/func_802EB5A4.cpp'
    ctor_text=ctor_source.read_text(encoding='utf-8')
    direct_alarm_call=bool(re.search(r'InvokeDirectCpu<0x804DEC60u>\(ctx\)',ctor_text))
    callback=0x802EB4E4
    callback_item=by_address.get(callback,{})
    callback_source=ROOT/'recomp/generated/RM3E01_00_conservative/functions/func_802EB4E4.cpp'
    callback_source_hash_ok=callback_source.is_file() and sha(callback_source)==callback_item.get('sha256')
    callback_literal='r9 = (r9 + -19228);' in ctor_text
    runtime_internal=ROOT/'recomp/third_party/WiiCompiled/runtime/src/hle/os/os_internal.h'
    hle_alarm_source=ROOT/'recomp/third_party/WiiCompiled/runtime/src/hle/os/os_alarm.cpp'
    qmatch=re.search(r'kAlarmQueueOffsetFromR13\s*=\s*(0x[0-9A-Fa-f]+)',runtime_internal.read_text(encoding='utf-8'))
    hle_queue_offset=int(qmatch.group(1),16) if qmatch else None
    prime_alarm_checkpoint=json.loads((ROOT/'recomp/checkpoints/20261020_prime_alarm_hle_audit.json').read_text(encoding='utf-8'))
    prime_queue_offset=int(prime_alarm_checkpoint['prime_alarm_queue']['observed_r13_offset'],0)
    prime_sda1=int(prime_alarm_checkpoint['prime_alarm_queue']['prime_sda1_base'],0)
    prime_queue_head=int(prime_alarm_checkpoint['prime_alarm_queue']['effective_head_address'],0)
    hle_queue_head=prime_sda1-hle_queue_offset if hle_queue_offset is not None else None
    startup_alarm={'root_to_constructor_path':[f'0x{x:08X}' for x in path],'constructor_source_sha256':sha(ctor_source),'constructor_metadata_sha256':ctor_item.get('sha256'),'alarm_address':f'0x{alarm:08X}','constructor_has_direct_alarm_dependency':alarm in ctor_deps,'emitted_source_has_direct_alarm_call':direct_alarm_call,'alarm_callback_address':f'0x{callback:08X}','alarm_callback_entry_in_function_map':callback in {int(x.split()[0],16) for x in MAP.read_text(encoding="utf-8").splitlines() if x.strip()},'alarm_callback_source_hash_verified':callback_source_hash_ok,'emitted_r9_matches_callback':callback_literal,'callback_in_main_direct_closure':callback in closure_addresses,'prime_queue_offset_from_r13':f'-0x{abs(prime_queue_offset):X}','runtime_hle_queue_offset_from_r13':f'-0x{hle_queue_offset:X}' if hle_queue_offset is not None else None,'queue_head_if_runtime_uses_prime_sda1':f'0x{hle_queue_head:08X}' if hle_queue_head is not None else None,'prime_queue_head':f'0x{prime_queue_head:08X}','runtime_and_prime_queue_bases_match':hle_queue_head==prime_queue_head,'closure_contains_constructor':constructor in closure_addresses,'closure_contains_alarm_entry':alarm in closure_addresses,'closure_checkpoint_sha256':sha(closure_path),'metadata_sha256':sha(meta_path),'entrypoint_direct_closure':{'root':entry_closure['root'],'unique_functions':entry_closure['direct_call_closure']['unique_functions'],'direct_call_edges':entry_closure['direct_call_closure']['direct_call_edges'],'indirect_sites':entry_closure['direct_call_closure']['total_indirect_sites'],'missing_sources':entry_closure['direct_call_closure']['missing_sources'],'source_hash_mismatches':entry_closure['direct_call_closure']['source_hash_mismatches'],'fixed_hle_registrations_as_exact_nodes':len(entry_override_hits),'closure_checkpoint_sha256':sha(entry_closure_path)}}
    duplicates=[x for x,c in Counter(s['address'] for s in sites).items() if c>1]
    report={'created_utc':datetime.now(timezone.utc).isoformat(),'revision':'RM3E01_00','dol_sha256':'6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104','profile':args.profile,'audit_tool':{'path':'recomp/tools/audit_hle_registration_interior_targets.py','sha256':sha(Path(__file__).resolve())},'inputs':{'function_map_sha256':sha(MAP),'symbols_sha256':sha(SYMBOLS),'runtime_hle_root':HLE.relative_to(ROOT).as_posix(),'profile_address_checkpoint_sha256':sha(PROFILE_AUDIT),'pinned_dol_sha256':sha(DOL)},'summary':{'registration_sites':len(sites),'unique_addresses':len(unique),'classification_counts':dict(counts),'duplicate_address_sites':len(duplicates),'registered_addresses_in_main_direct_closure':len(direct_override_hits)},'main_direct_closure_registered_addresses':[f'0x{x:08X}' for x in direct_override_hits],'startup_alarm_path':startup_alarm,'interpretation':'The register macro labels addresses from WiiCompiled/Mario Kart. An address inside a sized Prime function body is not established as a callable Prime entry; if runtime dispatch reaches it, the HLE can intercept a different Prime instruction boundary. This static inventory does not prove any registration is invoked.','limitations':['Preprocessor handling follows the project profile scanner for MKW_GAME_PROFILE_RM3E01_00; unrelated feature guards are not evaluated.','Sized symbols are static map evidence, not a dynamic call trace.','Direct-closure membership does not resolve indirect dispatch targets or prove runtime execution.','Computed function pointers, registration outside scanned C++ macros, and runtime reachability are not covered.'],'sites':sites}
    if len(unique)!=len(sites):report['duplicate_addresses']=duplicates
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'summary':report['summary'],'selected_targets':[s for s in sites if s['address'] in {'0x801A9C08','0x80006348','0x801A0870','0x801A08E0','0x8012E594'}]},indent=2))
    return 0
if __name__=='__main__':raise SystemExit(main())

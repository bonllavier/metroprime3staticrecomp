#!/usr/bin/env python3
"""Bounded native sampling of a manually controlled Prime run; no guest edits.

RIP samples identify where threads are observed, including waiting. Thread CPU
deltas distinguish busy threads from sleepers. Samples are not stack unwinds,
GPU timings or an FPS benchmark; brief suspensions perturb the measured run.
"""
import argparse
from collections import Counter,defaultdict
from bisect import bisect_right
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time
from capture_prime_hang import capture,symbolize

def resolve_dll_exports(report):
    """Annotate nearby exports only; private DLL routines stay unattributed."""
    reader=Path(__file__).resolve().parents[2]/'recomp/.tools/portable-tools/llvm-mingw/bin/llvm-readobj.exe'
    exports={};provenance={}
    exe=Path(report['executable'])
    for row in report['samples']:
        if 'rip' not in row or row.get('rip_symbol'):continue
        address=int(row['rip'],16)
        module=next((m for m in report['modules'] if int(m['base'],16)<=address<int(m['base'],16)+m['size']),None)
        if module is None or Path(module['path'])==exe:continue
        path=module['path']
        if path not in exports:
            result=subprocess.run([str(reader),'--coff-exports',path],capture_output=True,text=True,check=True)
            pairs=sorted((int(rva,16),name.strip()) for name,rva in
                re.findall(r'Name: ([^\r\n]+)\s+RVA: (0x[0-9A-Fa-f]+)',result.stdout))
            exports[path]=([rva for rva,name in pairs],pairs)
            provenance[path]={'sha256':hashlib.sha256(Path(path).read_bytes()).hexdigest(),'exports':len(pairs)}
        addresses,pairs=exports[path]
        rva=address-int(module['base'],16)
        index=bisect_right(addresses,rva)-1
        if index<0:continue
        start,name=pairs[index]
        # A nearby export is still not proof of a private function's extent.
        # Bound attribution so large gaps never become invented hot functions.
        if rva-start>128:continue
        row['rip_dll_export']={'module':Path(path).name,'nearest_export':name,'offset':hex(rva-start)}
    report['dll_export_symbols']={'scope':'Nearest exports within 128 bytes only; aliases may share an address. No unwind or CPU attribution from wait samples.','modules':provenance}

def summarize(report):
    rows=defaultdict(list)
    for row in report['samples']:rows[row['tid']].append(row)
    summary=[]
    for tid,samples in rows.items():
        counts=Counter()
        cpu=[r['cpu_100ns'] for r in samples if 'cpu_100ns' in r]
        for row in samples:
            if 'rip' not in row:continue
            symbol=row.get('rip_symbol')
            if symbol:location=symbol['nearest_text_symbol']
            elif row.get('rip_dll_export'):
                export=row['rip_dll_export'];location=export['module']+'!'+export['nearest_export']+'+'+export['offset']
            else:
                address=int(row['rip'],16)
                module=next((m for m in report['modules'] if int(m['base'],16)<=address<int(m['base'],16)+m['size']),None)
                location=Path(module['path']).name if module else 'unresolved'
            counts[location]+=1
        summary.append({'tid':tid,'cpu_ms':(cpu[-1]-cpu[0])/10000 if len(cpu)>1 else None,
                        'samples':len(samples),'top_locations':counts.most_common(15)})
    return sorted(summary,key=lambda r:r['cpu_ms'] or 0,reverse=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid',type=int,required=True)
    p.add_argument('--samples',type=int,default=200)
    p.add_argument('--interval',type=float,default=.02)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if a.output.exists():raise FileExistsError('Refusing to overwrite evidence')
    started=time.perf_counter()
    report=capture(a.pid,iterations=a.samples,interval=a.interval,stack_bytes=0)
    report['capture_seconds']=time.perf_counter()-started
    symbolize(report)
    resolve_dll_exports(report)
    report['thread_summary']=summarize(report)
    a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'pid':a.pid,'capture_seconds':report['capture_seconds'],'threads':report['thread_summary'][:6],'output':str(a.output)},indent=2))

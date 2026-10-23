#!/usr/bin/env python3
"""Audit every original font quad from a selected GPU frame, never title-screen proof."""
import argparse, hashlib, json, math, re, struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def inspect(buffers, text):
    assert len(buffers)==212992, 'Expected complete diagnostic GPU buffer ranges'
    records=[]
    for m in re.finditer(r"\[prime-raster-coverage-input\] frame=(\d+) vertices=(\d+) v=(\d+)/(\d+) u=(\d+)/(\d+) i=(\d+)/(\d+)",text):
        frame,n,v,vs,u,us,i,isz=map(int,m.groups())
        assert n==4 and vs==80 and isz==12, 'Font draw is not a complete original quad'
        assert v+vs<=65536 and u+us<=131072 and i+isz<=16384
        start,current,w,h=struct.unpack_from('<2I2f',buffers,65536+u)
        assert start==v and current==0 and w>0 and h>0
        verts=[struct.unpack_from('>5f',buffers,v+j*20) for j in range(4)]
        assert all(math.isfinite(x) for vertex in verts for x in vertex)
        assert all(vertex[1]==0 and 0<=vertex[0]<=640 and 0<=vertex[2]<=448 and 0<=vertex[3]<=1 and 0<=vertex[4]<=1 for vertex in verts), 'Corrupt vertex position or UV'
        assert verts[0][0]==verts[1][0] and verts[2][0]==verts[3][0] and verts[0][2]==verts[2][2] and verts[1][2]==verts[3][2], 'Quad lost a coordinate'
        assert verts[0][0]<verts[2][0] and verts[0][2]<verts[1][2]
        indices=struct.unpack_from('<6H',buffers,196608+i)
        assert indices==(0,1,2,2,1,3), 'Unexpected font triangle index stream'
        assert struct.unpack_from('<4f',buffers,65536+u+240)==(1.,1.,1.,1.)
        assert struct.unpack_from('<8f',buffers,65536+u+256)==(64.,80.,0.,0.,64.,80.,0.,0.)
        records.append(dict(frame=frame,vertex_offset=v,uniform_offset=u,index_offset=i,vertices=verts))
    assert len(records)==50 and len({r['frame'] for r in records})==1, 'Expected all 50 quads of one font frame'
    return records

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--probe',type=Path,required=True);ap.add_argument('--capture',type=Path,required=True);ap.add_argument('--checkpoint',type=Path,required=True)
    a=ap.parse_args();probe=json.loads(a.probe.read_text(encoding='utf-8-sig'))
    exe=ROOT/'recomp/build/RM3E01_00_runtime_link_probe/WiiCompiled.exe';log=Path(probe['log'])
    assert sha(exe)==probe['executable_sha256'] and sha(log)==probe['log_sha256']
    text=log.read_text(encoding='utf-8',errors='replace')
    assert probe['alive_at_deadline'] and probe['gpu_validation_requested'] and not probe['failure_logged']
    assert 'WebGPU validation and robustness enabled for runtime verification' in text
    raw=Path(str(a.capture)+'.buffers.bin');records=inspect(raw.read_bytes(),text)
    report=dict(revision='RM3E01_00',result='PASS all 50 original font GPU input quads',executable_sha256=sha(exe),game_title_screen_verified=False,frame=records[0]['frame'],records=records,
        scope='Checks original positions, UVs, uniform references, atlas sizes and triangle indices. Does not prove sampled texture pixels, text fidelity or title-screen reachability.',
        artifacts=[dict(path=str(p.resolve()),sha256=sha(p)) for p in [a.probe,log,a.capture,raw]])
    a.checkpoint.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(result=report['result'],frame=report['frame'],quads=len(records)),indent=2))
if __name__=='__main__': main()

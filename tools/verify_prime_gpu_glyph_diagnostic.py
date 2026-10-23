#!/usr/bin/env python3
"""Verify a GPU readback of the original first font draw; never a game-screen gate."""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import re
import struct
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--capture',type=Path,required=True)
    parser.add_argument('--checkpoint',type=Path,required=True)
    parser.add_argument('--single-glyph',action='store_true')
    args=parser.parse_args()
    probe=json.loads(args.probe.read_text(encoding='utf-8-sig'))
    exe=ROOT/'recomp/build/RM3E01_00_runtime_link_probe/WiiCompiled.exe'
    assert sha(exe)==probe['executable_sha256'], 'Executable changed since diagnostic'
    log=Path(probe['log']); assert sha(log)==probe['log_sha256']
    text=log.read_text(encoding='utf-8',errors='replace')
    assert probe.get('gpu_validation_requested') and not probe['failure_logged']
    assert 'WebGPU validation and robustness enabled for runtime verification' in text
    assert not re.search(r'WebGPU error|Runtime error:|Unhandled C\+\+ exception:',text)
    match=re.search(r'\[prime-gpu-transformed-vertices\] source=(.+?) frame=(\d+) vertex_range=(\d+)/(\d+) uniform_range=(\d+)/(\d+)',text)
    assert match,'Missing compute diagnostic'
    source=Path(match[1]); shader=source.read_text(encoding='utf-8')
    assert 'ubuf.vtx_start + vidx * 20u + 0u' in shader and 'ubuf.vtx_start + vidx * 20u + 12u' in shader
    vertex_offset,vertex_bytes,uniform_offset,uniform_bytes=map(int,match.group(3,4,5,6))
    assert vertex_bytes==80 and uniform_bytes>=288
    raw=Path(str(args.capture)+'.buffers.bin'); transformed=Path(str(args.capture)+'.transformed.bin')
    buffers=raw.read_bytes(); actual=transformed.read_bytes()
    assert len(buffers) in (13312,212992) and len(actual)==128
    vertex_capacity,uniform_capacity=(8192,4096) if len(buffers)==13312 else (65536,131072)
    assert vertex_offset+80<=vertex_capacity and uniform_offset+288<=uniform_capacity
    u=vertex_capacity+uniform_offset
    start,current,w,h=struct.unpack_from('<2I2f',buffers,u)
    assert start==vertex_offset and current==0 and w>0 and h>0
    projection=struct.unpack_from('<16f',buffers,u+80)
    matrix=struct.unpack_from('<12f',buffers,u+144)
    records=[]; max_error=0
    for i in range(4):
        vertex=struct.unpack_from('>5f',buffers,vertex_offset+i*20)
        position=vertex[:3]+(1.,)
        mv=tuple(sum(position[k]*matrix[row*4+k] for k in range(4)) for row in range(3))+(1.,)
        expected=[sum(mv[k]*projection[row*4+k] for k in range(4)) for row in range(4)]
        expected[0]-=expected[3]/(6*w); expected[1]+=expected[3]/(6*h)
        observed=struct.unpack_from('<4f',actual,i*32)
        observed_uv=struct.unpack_from('<2f',actual,i*32+16)
        assert all(math.isfinite(v) for v in observed)
        error=max(abs(a-b) for a,b in zip(expected,observed)); max_error=max(max_error,error)
        assert error<2e-6 and observed_uv==vertex[3:]
        records.append(dict(index=i,input=vertex,expected_clip=expected,gpu_clip=observed,gpu_uv=observed_uv))
    bmp=args.capture.read_bytes(); assert bmp[:2]==b'BM'
    offset=struct.unpack_from('<I',bmp,10)[0]; width,height=struct.unpack_from('<ii',bmp,18)
    assert struct.unpack_from('<H',bmp,28)[0]==32 and width>0 and 0<abs(height)<=16384
    points=[]
    for y in range(abs(height)):
        row=abs(height)-1-y if height>0 else y
        for x in range(width):
            pixel=offset+(row*width+x)*4
            if any(bmp[pixel:pixel+3]): points.append((x,y))
    bounds=[min(x for x,y in points),min(y for x,y in points),max(x for x,y in points),max(y for x,y in points)] if points else None
    if args.single_glyph:
        projected=[(2+(r['gpu_clip'][0]/r['gpu_clip'][3]+1)*320,2+(1-r['gpu_clip'][1]/r['gpu_clip'][3])*224) for r in records]
        assert len(points)>=5 and bounds is not None
        assert bounds[0]>=min(x for x,y in projected)-2 and bounds[2]<=max(x for x,y in projected)+2
        assert bounds[1]>=min(y for x,y in projected)-2 and bounds[3]<=max(y for x,y in projected)+2
    artifacts=[args.probe,log,source,args.capture,raw,transformed]
    report=dict(revision='RM3E01_00',result='PASS original-font GPU diagnostic',game_screen_verified=False,
        diagnostic_only=True,executable_sha256=probe['executable_sha256'],frame=int(match[2]),
        gpu_transform_max_abs_error=max_error,vertices=records,single_glyph_raster_bounds_checked=args.single_glyph,
        nonblack_pixels=len(points),pixel_bounds=bounds,
        artifacts=[dict(path=str(p.resolve()),sha256=sha(p)) for p in artifacts],
        scope='Original font vertex shader evaluated against captured GPU input. Isolated raster is diagnostic, not game output.')
    args.checkpoint.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:report[k] for k in ['result','frame','gpu_transform_max_abs_error','nonblack_pixels','pixel_bounds','game_screen_verified']},indent=2))
if __name__=='__main__': main()

#!/usr/bin/env python3
"""Verify original front-end activation, decoder calls, DMA and color copies."""
import argparse,hashlib,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--probe',type=Path,required=True);ap.add_argument('--checkpoint',type=Path,required=True);a=ap.parse_args()
    probe=json.loads(a.probe.read_text(encoding='utf-8-sig'));log=Path(probe['log']);exe=ROOT/'recomp/build/RM3E01_00_runtime_link_probe/WiiCompiled.exe'
    assert sha(exe)==probe['executable_sha256'] and sha(log)==probe['log_sha256']
    assert probe['alive_at_deadline'] and probe['gpu_validation_requested'] and not probe['failure_logged']
    text=log.read_text(encoding='utf-8',errors='replace')
    assert '[prime-front-gate] call=1 result=1' in text
    assert re.search(r'\[prime-front-state\].*flags=0x87 .*movies=14',text)
    assert re.search(r'\[prime-movie-control\] function=0x80259474 .*event=0x504c4159 .*loading=0 state=0 frames=901',text)
    assert re.search(r'\[prime-movie-control\] function=0x80259978 .*loading=0 state=0 frames=901',text)
    decodes=re.findall(r'address=0x804eb174 phase=return r3=(0x[0-9a-f]+)',text)
    assert len(decodes)>=32 and set(decodes)=={'0x0'}
    assert 'PPC_WriteSpr unhandled SPR 922 ' not in text and 'PPC_WriteSpr unhandled SPR 923 ' not in text
    dma=[tuple(int(v,16) if i<2 else int(v) for i,v in enumerate(m)) for m in re.findall(r'\[prime-locked-cache-dma\] memory=0x([0-9a-f]+) cache=0x([0-9a-f]+) bytes=(\d+) load=([01])',text)]
    assert len(dma)==32
    assert [r[1:] for r in dma[:5]]==[(0xE0000000,4096,0),(0xE0001000,4096,0),(0xE0002000,2048,0),(0xE0002A00,2560,0),(0xE0003480,2560,0)]
    assert dma[1][0]==dma[0][0]+4096 and dma[2][0]==dma[0][0]+8192
    copies=re.findall(r'\[prime-raw-texture-copy\] destination=(0x[0-9a-f]+) trigger=0x10043 stride_word=0xa0 left=0 top=0 width=640 height=448',text)
    assert len(copies)>=16
    bodies=[]
    root=ROOT/'recomp/generated/RM3E01_00_conservative'
    for address in [0x804EE4A8,0x804EB174,0x80353F70,0x803540A8,0x80259474,0x80259978,0x802598B0,0x802599FC]:
        raw=root/f'functions/func_{address:08X}.cpp';overlay=root/f'runtime_hooks/functions/func_{address:08X}.cpp'
        body=overlay.read_text().replace('#include "hle/prime_video_trace.h"\n','')
        body=body.replace(f'    PrimeTraceVideo(ctx,0x{address:08X}u,false);\n','').replace(f'    PrimeTraceVideo(ctx,0x{address:08X}u,true);\n','')
        body=body.replace(f'    PrimeTraceMovieControl(ctx,0x{address:08X}u);\n','')
        body=body.replace('#include "hle/prime_video_capture.h"\n','').replace('    const PrimeVideoFrameCapture frameCapture(ctx);\n','').replace('    frameCapture.Finish(ctx);\n','')
        assert body==raw.read_text(),'Observation changed original movie/control body'
        bodies.append(dict(address=hex(address),raw_sha256=sha(raw),overlay_sha256=sha(overlay)))
    captures=[]
    for c in probe['captures']:
        p=Path(c['path']);assert sha(p)==c['sha256'];captures.append(c)
    report=dict(result='PASS original title-sequence activation and transfer evidence',revision='RM3E01_00',executable_sha256=sha(exe),decoder_success_returns=len(decodes),dma_observations=dma,texture_copies=len(copies),original_bodies_preserved=bodies,captures=captures,
        limitation='Execution/transfer proof is separate from independent decoded-pixel comparison, original-GPU parity, title-menu interactivity, audio and performance validation.',game_title_screen_verified=False,
        artifacts=[dict(path=str(p.resolve()),sha256=sha(p)) for p in [a.probe,log,Path(__file__)]])
    a.checkpoint.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(result=report['result'],decoder_success_returns=len(decodes),texture_copies=len(copies)),indent=2))
if __name__=='__main__':main()

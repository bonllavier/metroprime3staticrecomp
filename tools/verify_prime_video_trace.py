#!/usr/bin/env python3
"""Check an observation-only trace of the original movie/THP path."""
import argparse,hashlib,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--probe',type=Path,required=True);ap.add_argument('--checkpoint',type=Path,required=True);a=ap.parse_args()
    probe=json.loads(a.probe.read_text(encoding='utf-8-sig'));log=Path(probe['log']);exe=ROOT/'recomp/build/RM3E01_00_runtime_link_probe/Corruption_3 PC.exe'
    assert sha(exe)==probe['executable_sha256'] and sha(log)==probe['log_sha256']
    assert probe['alive_at_deadline'] and probe['gpu_validation_requested'] and not probe['failure_logged']
    text=log.read_text(encoding='utf-8',errors='replace')
    assert 'address=0x804ee4a8 phase=return r3=0x1' in text
    assert 'address=0x804eb174 phase=enter' not in text, 'Decoder now executes: revisit the earlier preload-only diagnosis'
    assert 'address=0x80353f70 phase=enter' not in text, 'Engine decode dispatcher now executes'
    records=[]
    pattern=r"\[prime-video-object\] address=(0x[0-9a-f]+) frames=(\d+) components=(\d+) read=(\d+) loaded=(\d+) decode_frame=(\d+) decode_index=(\d+) requested=(\d+) buffered=(\d+) state=(\d+) time=([^ ]+) interval=([^ ]+) dt=([^ ]+) io=(0x[0-9a-f]+)"
    for m in re.finditer(pattern,text):
        obj,frames,components,read,loaded,frame,index,requested,buffered,state,time,duration,dt,io=m.groups()
        r=dict(object=obj,frames=int(frames),components=int(components),read=int(read),loaded=int(loaded),decode_frame=int(frame),decode_index=int(index),requested=int(requested),buffered=int(buffered),state=int(state),time=float(time),duration=float(duration),dt=float(dt),io=io)
        assert r['frames']==1285 and r['components']==1 and r['state']==0 and r['buffered']==0 and r['decode_frame']==0
        assert .015<r['dt']<.018 and r['time']==0
        records.append(r)
    assert len(records)>=8 and len({r['object'] for r in records})==1
    assert max(r['loaded'] for r in records)>=3
    overlays=[]
    for address in [0x804EE4A8,0x804EB174,0x80353F70,0x803540A8]:
        root=ROOT/'recomp/generated/RM3E01_00_conservative'
        raw=root/f'functions/func_{address:08X}.cpp';overlay=root/f'runtime_hooks/functions/func_{address:08X}.cpp'
        body=overlay.read_text(encoding='utf-8').replace('\r\n','\n')
        body=body.replace('#include "hle/prime_video_trace.h"\n','')
        body=body.replace('#include "hle/prime_video_capture.h"\n','').replace('    const PrimeVideoFrameCapture frameCapture(ctx);\n','').replace('    frameCapture.Finish(ctx);\n','')
        body=body.replace(f'    PrimeTraceVideo(ctx,0x{address:08X}u,false);\n','').replace(f'    PrimeTraceVideo(ctx,0x{address:08X}u,true);\n','')
        assert body==raw.read_text(encoding='utf-8').replace('\r\n','\n'), 'Observation overlay changed original body'
        overlays.append(dict(address=f'0x{address:08X}',raw_sha256=sha(raw),overlay_sha256=sha(overlay)))
    report=dict(result='PASS original THP init and paused attract preload observation',revision='RM3E01_00',executable_sha256=sha(exe),records=records,original_bodies_preserved=overlays,
        decoder_called=False,video_output_verified=False,game_title_screen_verified=False,
        conclusion='The selected attract object preloads at least three frames and remains state0 with no decoded frames. The trace does not establish the decoder as the blocker; follow the original front-end activation/state path.',
        artifacts=[dict(path=str(p.resolve()),sha256=sha(p)) for p in [a.probe,log,ROOT/'recomp/third_party/WiiCompiled/runtime/include/hle/prime_video_trace.h']])
    a.checkpoint.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(result=report['result'],observations=len(records),decoder_called=False,game_title_screen_verified=False),indent=2))
if __name__=='__main__':main()

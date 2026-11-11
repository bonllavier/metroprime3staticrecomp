#!/usr/bin/env python3
"""Validate Windows/SDL input reaching the original Prime caller as PPC32 samples."""
import argparse,hashlib,json,math,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def decode(data):
    assert len(data)>=140*5 and len(data)%140==0, 'Incomplete input capture'
    result=[];previous=0;previous_frame=-1
    for offset in range(0,len(data),140):
        r=data[offset:offset+140];frame,dest=struct.unpack_from('>2I',r)
        assert frame>previous_frame and 0x80000000<=dest<0x94000000
        hold,trigger,release=struct.unpack_from('>3I',r,8)
        assert trigger==hold&~previous and release==previous&~hold, 'Button edge differs from held state'
        assert hold&~0xFFFF==0
        f=lambda o:struct.unpack_from('>f',r,8+o)[0]
        assert tuple(f(o) for o in (12,16,20))==(0.,-1.,0.)
        assert tuple(f(o) for o in (104,108,112))==(0.,-1.,0.)
        assert f(24)==1. and f(116)==1.
        assert list(r[100:104]) in ([1,0,0,5],[1,0,1,5]), 'Device/format/error fields differ'
        x,y=f(96),f(100);assert math.isfinite(x) and math.isfinite(y) and x*x+y*y<=1.000001
        px,py=f(32),f(36);valid=r[102]
        assert math.isfinite(px) and math.isfinite(py) and (not valid or -1<=px<=1 and -1<=py<=1)
        result.append(dict(retrace=frame,destination=f'0x{dest:08X}',hold=hold,trigger=trigger,release=release,stick=[x,y],pointer_valid=bool(valid),pointer=[px,py]))
        previous=hold;previous_frame=frame
    assert any(r['hold']==0x800 and r['trigger']==0x800 for r in result), 'Enter did not reach A'
    assert any(r['hold']==0 and r['release']==0x800 for r in result), 'A release missing'
    assert any(r['stick']==[0.,1.] for r in result), 'W did not reach Nunchuk up'
    assert result[-1]['hold']==0 and result[-1]['stick']==[0.,0.], 'Stuck host input'
    assert any(r['pointer_valid'] for r in result), 'No mouse pointing sample'
    return result
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['probe','capture','checkpoint']:p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();probe=json.loads(a.probe.read_text(encoding='utf-8-sig'));log=Path(probe['log'])
    exe=ROOT/'recomp/build/RM3E01_00_runtime_link_probe/Corruption_3 PC.exe'
    assert sha(exe)==probe['executable_sha256'] and sha(log)==probe['log_sha256']
    assert probe['alive_at_deadline'] and not probe['failure_logged'] and probe['gpu_validation_requested'] and probe['keyboard_exercise_requested']
    receipts=probe['keyboard_message_receipts'];messages=[r for r in receipts if 'message' in r]
    assert len(messages)==7 and all(r['posted'] and r['pid']==probe['pid'] for r in receipts)
    assert [(r['message'],r['wparam']) for r in messages]==[(7,0),(512,0),(256,13),(257,13),(256,87),(257,87),(8,0)]
    assert any(r.get('kind')=='cursor_move' for r in receipts)
    text=log.read_text(encoding='utf-8',errors='replace')
    assert '[prime-keyboard-device-event] kind=connected callback=0x803c48ac channel=0 value=0' in text
    assert '[prime-keyboard-device-event] kind=nunchuk callback=0x803c499c channel=0 value=1' in text
    assert 'scan=40 down=1' in text and 'scan=40 down=0' in text
    assert 'scan=26 down=1' in text and 'scan=26 down=0' in text
    assert 'path=/Video/FrontEnd/attract01.thp' in text
    records=decode(a.capture.read_bytes())
    report=dict(result='PASS targeted Windows messages through SDL into captured PPC32 input samples',revision='RM3E01_00',executable_sha256=sha(exe),records=records,
        game_title_screen_verified=False,physical_wiimote_verified=False,
        scope='Exercises keyboard A press/release, Nunchuk stick, mouse pointing and original connect/extension callbacks. Full motion, physical remotes, input latency and title rendering remain unverified.',
        artifacts=[dict(path=str(p.resolve()),sha256=sha(p)) for p in [a.probe,log,a.capture]])
    a.checkpoint.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(result=report['result'],samples=len(records),game_title_screen_verified=False),indent=2))
if __name__=='__main__':main()

#!/usr/bin/env python3
"""Match a real decoder input to its THP frame and compare independently decoded planes."""
import argparse,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def untile(data,width,height):
    assert len(data)==width*height and width%8==0 and height%4==0
    result=bytearray(len(data));at=0
    for y in range(0,height,4):
        for x in range(0,width,8):
            for row in range(4):
                p=(y+row)*width+x;result[p:p+8]=data[at:at+8];at+=8
    return result
def statistics(actual,reference):
    assert len(actual)==len(reference)
    errors=[abs(x-y) for x,y in zip(actual,reference)]
    return dict(samples=len(errors),exact=sum(e==0 for e in errors),maximum=max(errors),mean=sum(errors)/len(errors),over_one=sum(e>1 for e in errors))
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--capture-prefix',required=True,type=Path)
    ap.add_argument('--probe',required=True,type=Path)
    ap.add_argument('--checkpoint',required=True,type=Path)
    ap.add_argument('--expect-later-layout-boundary',action='store_true')
    a=ap.parse_args();prefix=str(a.capture_prefix)
    metadata=Path(prefix+'.json');capture=json.loads(metadata.read_text());w,h=capture['width'],capture['height']
    assert capture['return']==0 and capture['layout']=='I8 tiles 8x4'
    probe=json.loads(a.probe.read_text(encoding='utf-8-sig'));log=Path(probe['log'])
    assert sha(log)==probe['log_sha256']
    if a.expect_later_layout_boundary:
        assert probe['failure_logged'] and probe['process_exit_code']==1
        assert 'Runtime error: Prime raw GX vertex layout disagrees with its verified packet size' in log.read_text(errors='replace')
    else:
        assert probe['alive_at_deadline'] and not probe['failure_logged']
    assert sha(ROOT/'recomp/build/RM3E01_00_runtime_link_probe/WiiCompiled.exe')==probe['executable_sha256']
    assert f'[prime-video-reference-capture] prefix={prefix} call={capture["call"]}' in log.read_text(errors='replace')
    thp=ROOT/'recomp/.tools/disc-extraction-rm3e01/files/Video/FrontEnd/Mp3_Title_Sequence.thp'
    data=thp.read_bytes();word=lambda at:int.from_bytes(data[at:at+4],'big')
    assert data[:4]==b'THP\0' and word(20)==901 and word(word(32))==1
    jpeg=Path(prefix+'.jpeg.bin');selected=jpeg.read_bytes();assert len(selected)==capture['jpeg_bytes']
    matches=[];offset=word(40);frame_size=word(24)
    for n in range(word(20)):
        assert offset+frame_size<=len(data) and frame_size>=12
        size=word(offset+8);assert size+12<=frame_size
        if data[offset+12:offset+12+size]==selected:matches.append(n)
        next_size=word(offset)
        offset+=frame_size;frame_size=next_size
    assert len(matches)==1, matches
    frame=matches[0];assert frame==capture['call']-1,'Invocation did not map to the expected sequential title frame'
    tool=json.loads((ROOT/'recomp/checkpoints/20261021_ffmpeg_reference_tool.json').read_text())
    ffmpeg=ROOT/tool['executable'];assert sha(ffmpeg)==tool['executable_sha256']
    raw=Path(prefix+'.ffmpeg.yuv')
    command=[str(ffmpeg),"-hide_banner","-loglevel","error","-i",str(thp),"-vf",f"select='eq(n,{frame})'","-frames:v","1","-pix_fmt","yuvj420p","-f","rawvideo","-y",str(raw)]
    result=subprocess.run(command,capture_output=True,text=True,timeout=120);assert result.returncode==0,result.stderr
    reference=raw.read_bytes();assert len(reference)==w*h*3//2
    planes={};at=0;files=[a.probe,log,metadata,jpeg,thp,ffmpeg,raw,Path(__file__)]
    for channel,width,height in [('y',w,h),('u',w//2,h//2),('v',w//2,h//2)]:
        p=Path(prefix+f'.{channel}.bin');files.append(p)
        actual=untile(p.read_bytes(),width,height);expected=reference[at:at+len(actual)];at+=len(actual)
        planes[channel]=statistics(actual,expected)
    # Independent IDCT implementations can differ at rounding boundaries.
    # Preserve the original SDK; never alter it to make this oracle match.
    passed=all(p['maximum']<=1 for p in planes.values())
    report=dict(result='PASS independent THP planes within one code value' if passed else 'FAIL independent THP plane bound',frame=frame,planes=planes,executable_sha256=probe['executable_sha256'],ffmpeg_command=command,
        scope='Original SDK output after real LC DMA, exact compressed-frame identity, independent FFmpeg YUV decoder. Bound is per sample with no spatial tolerance; this is not proof of bit equality with Wii hardware or renderer fidelity.',
        artifacts=[dict(path=str(p.resolve()),sha256=sha(p)) for p in files])
    a.checkpoint.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(result=report['result'],frame=frame,planes=planes),indent=2))
    if not passed:raise SystemExit(1)
if __name__=='__main__':main()

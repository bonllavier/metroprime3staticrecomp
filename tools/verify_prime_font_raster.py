#!/usr/bin/env python3
"""Compare the real GPU font raster with an independent C4/bitplane palette oracle."""
import argparse, hashlib, json, math, struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def reference_pixels(texture,draws):
    assert len(texture)==2560 and len(draws)==50*113
    pixels={};palettes=set()
    for record in range(50):
        off=record*113;vertices=[struct.unpack_from('>5f',draws,off+i*20) for i in range(4)]
        palette=struct.unpack_from('>16H',draws,off+80)
        assert draws[off+112]==2 and set(palette)=={0,65535}
        palettes.add(palette)
        a,b,c,d=vertices
        assert all(v[1]==0 and all(math.isfinite(x) for x in v) for v in vertices)
        assert a[0]==b[0] and c[0]==d[0] and a[2]==c[2] and b[2]==d[2]
        assert a[0]<c[0] and a[2]<b[2]
        # The verified original 640x448 viewport starts at (2,2). GX pixel
        # center compensation moves the geometric edge by -1/12 output pixel.
        left,top=a[0]+2-1/12,a[2]+2-1/12
        right,bottom=c[0]+2-1/12,b[2]+2-1/12
        for y in range(math.ceil(top-.5),math.ceil(bottom-.5)):
            for x in range(math.ceil(left-.5),math.ceil(right-.5)):
                u=a[3]+((x+.5-left)/(right-left))*(c[3]-a[3])
                v=a[4]+((y+.5-top)/(bottom-top))*(b[4]-a[4])
                # The generated TEV shader truncates coordinates to 1/128
                # texel before native bilinear filtering. RGB and alpha both
                # interpolate the same 0/1 palette, then SRC_ALPHA blends them.
                tx,ty=math.trunc(u*64*128)/128-.5,math.trunc(v*80*128)/128-.5
                ix,iy=math.floor(tx),math.floor(ty);fx,fy=tx-ix,ty-iy
                def sample(sx,sy):
                    sx=max(0,min(63,sx));sy=max(0,min(79,sy))
                    byte=texture[((sy//8)*8+sx//8)*32+(sy%8)*4+(sx%8)//2]
                    return int(palette[(byte>>(0 if sx%2 else 4))&15]==65535)
                value=sample(ix,iy)*(1-fx)*(1-fy)+sample(ix+1,iy)*fx*(1-fy)+sample(ix,iy+1)*(1-fx)*fy+sample(ix+1,iy+1)*fx*fy
                pixels[x,y]=round(value*value*255)
    assert len(palettes)==4, 'Expected four distinct original font bitplane palettes'
    return pixels,len(palettes)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['probe','capture','source-prefix','checkpoint']:p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();probe=json.loads(a.probe.read_text(encoding='utf-8-sig'));log=Path(probe['log'])
    exe=ROOT/'recomp/build/RM3E01_00_runtime_link_probe/Corruption_3 PC.exe'
    assert sha(exe)==probe['executable_sha256'] and sha(log)==probe['log_sha256']
    assert probe['gpu_validation_requested'] and not probe['failure_logged'] and probe['alive_at_deadline']
    texture=Path(str(a.source_prefix)+'.c4.bin');draws=Path(str(a.source_prefix)+'.draws.bin')
    expected,palettes=reference_pixels(texture.read_bytes(),draws.read_bytes())
    bmp=a.capture.read_bytes();assert bmp[:2]==b'BM' and struct.unpack_from('<H',bmp,28)[0]==32
    offset=struct.unpack_from('<I',bmp,10)[0];w,h=struct.unpack_from('<ii',bmp,18)
    assert w==640 and abs(h)==448
    different=0;max_error=0;nonblack=0
    for y in range(abs(h)):
        row=abs(h)-1-y if h>0 else y
        for x in range(w):
            pixel=bmp[offset+(row*w+x)*4:offset+(row*w+x)*4+3]
            assert len(pixel)==3
            if any(pixel):nonblack+=1
            error=max(abs(component-expected.get((x,y),0)) for component in pixel)
            max_error=max(max_error,error);different+=error!=0
    # One 8-bit step covers native interpolation/UNORM blend rounding. No
    # spatial tolerance: a wrong glyph, palette or position fails this bound.
    assert max_error<=1, f'Raster differs: max channel error={max_error}, pixels={different}'
    artifacts=[a.probe,log,a.capture,texture,draws]
    report=dict(result='PASS original-font GPU raster within one 8-bit channel step of independent reference',revision='RM3E01_00',executable_sha256=sha(exe),quads=50,distinct_bitplane_palettes=palettes,nonblack_pixels=nonblack,compared_pixels=w*abs(h),exact_pixel_matches=w*abs(h)-different,max_channel_error=max_error,allowed_channel_error=1,game_title_screen_verified=False,
        scope='Selected stable font raster agrees with the first 50 original raw vertex/texture/palette inputs using bilinear sampling and SRC_ALPHA blending; native rounding differs by at most one 8-bit channel step. Does not establish title-screen reachability or complete console renderer equivalence.',
        artifacts=[dict(path=str(p.resolve()),sha256=sha(p)) for p in artifacts])
    a.checkpoint.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps({k:v for k,v in report.items() if k!='artifacts'},indent=2))
if __name__=='__main__':main()

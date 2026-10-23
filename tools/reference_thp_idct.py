#!/usr/bin/env python3
"""Offline baseline-THP entropy and direct double-IDCT reference; never used by the game.

Mathematical reference: ITU T.81, Annexes A, C and F.
THP entropy data has no JPEG byte stuffing. Only the observed SOF0 4:2:0
single-scan form is accepted. No game/runtime decoder code is reused.
"""
import argparse,hashlib,json,math
from collections import Counter
from pathlib import Path
from verify_prime_thp_pixels import untile,statistics

ZIGZAG=[]
for diagonal in range(15):
    rows=list(range(max(0,diagonal-7),min(7,diagonal)+1))
    if diagonal%2==0:rows.reverse()
    ZIGZAG.extend(row*8+diagonal-row for row in rows)
BASIS=[[math.cos((2*x+1)*u*math.pi/16)*(1/math.sqrt(2) if u==0 else 1) for u in range(8)] for x in range(8)]

def idct(coefficients):
    # Separable evaluation of the complete mathematical transform. The SDK's
    # paired-single AAN implementation and FFmpeg's IDCT are not called here.
    intermediate=[[sum(BASIS[x][u]*coefficients[v*8+u] for u in range(8)) for x in range(8)] for v in range(8)]
    return [128+sum(BASIS[y][v]*intermediate[v][x] for v in range(8))/4 for y in range(8) for x in range(8)]

class Bits:
    def __init__(self,data):self.data=data;self.at=0
    def read(self,n):
        value=0
        for _ in range(n):
            if self.at>=len(self.data)*8:raise ValueError('Truncated entropy')
            value=(value<<1)|((self.data[self.at//8]>>(7-self.at%8))&1);self.at+=1
        return value
    def symbol(self,table):
        code=0
        for n in range(1,17):
            code=(code<<1)|self.read(1)
            if (n,code) in table:return table[n,code]
        raise ValueError('Invalid Huffman code')
    def signed(self,n):
        if not n:return 0
        v=self.read(n)
        return v if v>=(1<<(n-1)) else v-((1<<n)-1)

def decode(data):
    assert data[:2]==b'\xff\xd8'
    quant={};huffman={};components={};scan=[];pos=2;entropy=None
    while pos+4<len(data):
        assert data[pos]==255
        marker=data[pos+1];length=int.from_bytes(data[pos+2:pos+4],'big');body=data[pos+4:pos+2+length]
        assert length>=2 and len(body)==length-2
        pos+=2+length
        if marker==219:
            p=0
            while p<len(body):
                info=body[p];p+=1;assert info>>4==0
                values=body[p:p+64];p+=64;assert len(values)==64
                table=[0]*64
                for i,v in enumerate(values):table[ZIGZAG[i]]=v
                quant[info&15]=table
        elif marker==196:
            p=0
            while p<len(body):
                info=body[p];counts=body[p+1:p+17];p+=17;assert len(counts)==16
                table={};code=0
                for n,count in enumerate(counts,1):
                    for _ in range(count):table[n,code]=body[p];p+=1;code+=1
                    code<<=1
                huffman[info>>4,info&15]=table
        elif marker==192:
            assert body[0]==8 and body[5]==3 and len(body)==15
            height=int.from_bytes(body[1:3],'big');width=int.from_bytes(body[3:5],'big')
            assert width%16==0 and height%16==0
            for p in range(6,15,3):components[body[p]]=(body[p+1]>>4,body[p+1]&15,body[p+2])
            assert list(components)==[1,2,3] and [v[:2] for v in components.values()]==[(2,2),(1,1),(1,1)]
        elif marker==218:
            assert body[0]==3 and body[-3:]==b'\0?\0'
            scan=[(body[p],body[p+1]>>4,body[p+1]&15) for p in range(1,7,2)]
            assert [v[0] for v in scan]==[1,2,3]
            entropy=Bits(data[pos:]);break
        else:raise ValueError(f'Unreviewed JPEG marker {marker:x}')
    assert entropy is not None
    dims={c:(width if c==1 else width//2,height if c==1 else height//2) for c in components}
    planes={c:[0.0]*(w*h) for c,(w,h) in dims.items()};predictors={c:0 for c in components}
    for my in range(height//16):
        for mx in range(width//16):
            for c,dc,ac in scan:
                hs,vs,q=components[c];w,_=dims[c]
                for by in range(vs):
                    for bx in range(hs):
                        coefficients=[0]*64
                        n=entropy.symbol(huffman[0,dc]);assert n<=11
                        predictors[c]+=entropy.signed(n);coefficients[0]=predictors[c]*quant[q][0]
                        at=1
                        while at<64:
                            symbol=entropy.symbol(huffman[1,ac])
                            if symbol==0:break
                            if symbol==240:at+=16;assert at<=64;continue
                            run,n=symbol>>4,symbol&15;assert n and n<=10
                            at+=run;assert at<64
                            natural=ZIGZAG[at];coefficients[natural]=entropy.signed(n)*quant[q][natural];at+=1
                        pixels=idct(coefficients)
                        for y in range(8):
                            start=(my*vs*8+by*8+y)*w+mx*hs*8+bx*8
                            planes[c][start:start+8]=pixels[y*8:y*8+8]
    return dims,planes,entropy.at

def quantize(values,bias):return bytes(max(0,min(255,math.floor(v+bias+1e-9))) for v in values)
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--capture-prefix',type=Path,required=True);ap.add_argument('--checkpoint',type=Path,required=True);a=ap.parse_args()
    assert len(ZIGZAG)==64 and len(set(ZIGZAG))==64
    assert max(abs(v-128) for v in idct([0]*64))==0
    dc=[0]*64;dc[0]=80;assert max(abs(v-138) for v in idct(dc))<1e-10
    ac=[0]*64;ac[1]=8;assert abs(sum(v-128 for v in idct(ac)))<1e-10
    prefix=str(a.capture_prefix);jpeg=Path(prefix+'.jpeg.bin');dims,planes,bits=decode(jpeg.read_bytes())
    result={};outputs=[]
    for c,name in [(1,'y'),(2,'u'),(3,'v')]:
        w,h=dims[c];p=Path(prefix+f'.{name}.bin');actual=untile(p.read_bytes(),w,h)
        floor=quantize(planes[c],0);nearest=quantize(planes[c],.5)
        out=Path(prefix+f'.{name}.direct_idct_floor.bin');out.write_bytes(floor);outputs.append(out)
        result[name]=dict(truncate=statistics(actual,floor),nearest=statistics(actual,nearest),
            signed_truncate_histogram=dict(sorted(Counter(x-y for x,y in zip(actual,floor)).items())))
    passed=all(r['truncate']['maximum']<=1 for r in result.values())
    report=dict(result='PASS direct IDCT with observed truncation within one code value' if passed else 'FAIL direct IDCT truncation comparison',planes=result,entropy_bits_consumed=bits,
        reference='https://www.w3.org/Graphics/JPEG/itu-t81.pdf',
        scope='Independent Huffman/dequantization/direct double-IDCT mathematics. Truncation policy matches the runtime PSQ integer store; this does not prove equality with Wii hardware. FFmpeg comparison remains a separate result.',
        artifacts=[dict(path=str(p.resolve()),sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in [jpeg,Path(__file__)]+outputs])
    a.checkpoint.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(report,indent=2))
    if not passed:raise SystemExit(1)
if __name__=='__main__':main()

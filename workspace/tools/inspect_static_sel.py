"""Read-only RM3E01_00 static symbol baseline from selfile.sel and main.dol.

Section bases are checked against RSOStaticLocateObject's jump table and
instructions in the original DOL. This does not relocate or execute a module.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from inspect_startup_rso import elf_name_hash,inspect_bytes as inspect_rso

ROOT=Path(__file__).resolve().parents[1]
DOL_SHA1='3e81f9b09dfb1e7c02e22b9c6c48a4a1390224b2'
SEL_SHA1='bb9a23da22e406dbf627c5c096e2345d58b9d172'


def section_bases(dol):
    if hashlib.sha1(dol).hexdigest()!=DOL_SHA1:
        raise ValueError('Wrong original DOL revision')
    sections=[tuple(struct.unpack_from('>I',dol,b+i*4)[0] for b in (0,0x48,0x90)) for i in range(18)]
    def word(address):
        for offset,start,size in sections:
            if offset and start<=address and address+4<=start+size:
                return struct.unpack_from('>I',dol,offset+address-start)[0]
        raise ValueError('Unbacked original instruction/table')
    # Case destinations read from the switch in RSOStaticLocateObject.
    expected={1:0x8054092c,2:0x8054093c,5:0x805409ac,6:0x805409bc,
              7:0x805409cc,8:0x805409dc,9:0x805409fc,11:0x805409ec,12:0x80540a0c}
    if (word(0x80540914),word(0x80540918))!=(0x3c60805c,0x3863f9b0):
        raise ValueError('Static section switch differs')
    result={}
    for index,pc in expected.items():
        if word(0x805bf9b0+index*4)!=pc:
            raise ValueError('Static section case differs')
        high,low=word(pc),word(pc+4)
        if high&0xffff0000!=0x3c600000 or low&0xffff0000!=0x38030000:
            raise ValueError('Unexpected section base instruction pair')
        signed_low=(low&0x7fff)-(low&0x8000)
        result[index]=(((high&65535)<<16)+signed_low)&0xffffffff
    return result


def inspect_bytes(data,bases):
    def span(offset,size):
        if offset<0 or size<0 or offset+size>len(data):
            raise ValueError('SEL range outside file')
        return data[offset:offset+size]
    def string(offset):
        span(offset,1)
        end=data.find(b'\0',offset,min(len(data),offset+65536))
        if end<0:raise ValueError('Unterminated SEL symbol')
        return data[offset:end].decode('ascii')
    fields=struct.unpack('>8I4B13I',span(0,0x58))
    if fields[0] or fields[1] or fields[6]!=1 or any(fields[7:12]):
        raise ValueError('Expected static unlinked version-1 SEL')
    count,table=fields[2:4]
    if not 1<=count<=256:raise ValueError('Invalid section count')
    if any(span(table,count*8)):
        raise ValueError('Expected empty static section descriptors')
    if any(fields[i] for i in (12,13,14,16,18,23)):
        raise ValueError('Unexpected entries, relocations or imports in static SEL')
    name=span(fields[4],fields[5]).rstrip(b'\0').decode('ascii')
    exports,export_size,names=fields[19:22]
    if export_size%16:raise ValueError('Partial SEL export')
    span(exports,export_size)
    result=[];seen=set()
    for position in range(exports,exports+export_size,16):
        name_offset,value,section,name_hash=struct.unpack_from('>4I',data,position)
        symbol=string(names+name_offset)
        if symbol in seen:raise ValueError('Duplicate SEL symbol')
        if elf_name_hash(symbol)!=name_hash:raise ValueError('SEL name hash differs')
        seen.add(symbol)
        if section==0xfff1:address=value
        elif section in bases and section<count:address=(bases[section]+value)&0xffffffff
        else:raise ValueError('Unreviewed static SEL section')
        result.append({'name':symbol,'section':section,'section_offset':value,'address':address,'name_hash':name_hash})
    return {'module_name':name,'section_count':count,'exports':result,'sha1':hashlib.sha1(data).hexdigest(),'size':len(data)}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=ROOT/'analysis/first_screen/static_sel.json')
    parser.add_argument('--modules',type=Path,nargs='*',default=[])
    args=parser.parse_args()
    path=ROOT/'decomp/orig/RM3E01_00/files/RSO/Rev2 Production/selfile.sel'
    data=path.read_bytes()
    if hashlib.sha1(data).hexdigest()!=SEL_SHA1:raise ValueError('Wrong SEL baseline')
    bases=section_bases((ROOT/'decomp/orig/RM3E01_00/sys/main.dol').read_bytes())
    report=inspect_bytes(data,bases)
    symbols={m[1]:int(m[2],16) for m in re.finditer(r'(?m)^(.+?) = \S+:0x([0-9a-fA-F]+);',
        (ROOT/'decomp/config/RM3E01_00/symbols.txt').read_text())}
    for item in report['exports']:
        item['dtk_address']=symbols.get(item['name'])
        item['dtk_address_matches']=item['dtk_address']==item['address']
    report.update(revision='RM3E01_00',source=str(path),section_bases=bases,
        matching_named_dtk_exports=sum(x['dtk_address_matches'] for x in report['exports']),
        scope='Static symbol baseline only; no applied relocations, module rebuilt or native execution.')
    if any(x['dtk_address'] is not None and not x['dtk_address_matches'] for x in report['exports']):
        raise ValueError('Static SEL and named DTK addresses disagree')
    exports={item['name']:item['address'] for item in report['exports']}
    modules=[]
    for path in args.modules:
        module=inspect_rso(path.read_bytes())
        unresolved=[item['name'] for item in module['imports'] if item['name'] not in exports]
        disagreements=[item['name'] for item in module['imports']
            if item['name'] in exports and symbols.get(item['name'])!=exports[item['name']]]
        if disagreements:raise ValueError('Module import/static address disagreement')
        modules.append({'file':str(path),'sha1':module['sha1'],'imports':len(module['imports']),
            'imports_in_static_sel':len(module['imports'])-len(unresolved),'unresolved_static_names':unresolved})
    report['modules']=modules
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ('revision','size','sha1','section_count','matching_named_dtk_exports','modules')},indent=2))


if __name__=='__main__':main()

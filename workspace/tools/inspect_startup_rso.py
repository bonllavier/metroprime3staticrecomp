"""Bounded read-only RSO metadata inspection, not a linker or decompiler.

Format checked against decomp-toolkit 1.8.4 src/util/rso.rs, commit
a0c455e46cab58e1d2e0885623f85089ff0499db (MIT OR Apache-2.0).
This independent inspector preserves original section indices and reports
imports/relocations without applying them or guessing load addresses.
"""
import argparse, hashlib, json, re, struct
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
REFERENCE='https://github.com/encounter/decomp-toolkit/blob/a0c455e46cab58e1d2e0885623f85089ff0499db/src/util/rso.rs'

def elf_name_hash(name):
    result=0
    for byte in name.encode('ascii'):
        result=((result<<4)+byte)&0xffffffff
        high=result&0xf0000000
        result=(result^(high>>24))&~high
    return result

def inspect_bytes(data):
    def span(offset,size):
        if offset<0 or size<0 or offset+size>len(data):
            raise ValueError(f'Range outside RSO: {offset:#x}+{size:#x}')
        return data[offset:offset+size]
    def string(offset):
        if not 0<=offset<len(data):raise ValueError('String offset outside RSO')
        end=data.find(b'\0',offset,min(len(data),offset+65536))
        if end<0:raise ValueError('Unterminated RSO name')
        return data[offset:end].decode('ascii')
    fields=struct.unpack('>8I4B13I',span(0,0x58))
    next_module,prev_module,n,section_offset,name_offset,name_size,version,bss_size=fields[:8]
    if next_module or prev_module or fields[11] or version!=1:
        raise ValueError('Expected unlinked version-1 RSO')
    if not 1<=n<=256:raise ValueError('Unsupported section count')
    span(section_offset,n*8)
    sections=[]
    for i in range(n):
        flags,size=struct.unpack('>II',span(section_offset+i*8,8));offset=flags&~1
        if offset:span(offset,size)
        sections.append({'index':i,'file_offset':offset,'size':size,'executable_flag':bool(flags&1),
                         'bss':bool(size and not offset)})
    if sum(s['size'] for s in sections if s['bss'])!=bss_size:raise ValueError('BSS sum differs')
    entries={}
    for i,label in enumerate(['prolog','epilog','unresolved']):
        section,offset=fields[8+i],fields[12+i]
        if section:
            if section>=n or offset>=sections[section]['size']:raise ValueError('Entry outside section')
            entries[label]={'section':section,'offset':offset}
    tables={}
    for label,table_offset,table_size,names,stride in [
        ('exports',fields[19],fields[20],fields[21],16),
        ('imports',fields[22],fields[23],fields[24],12)]:
        if table_size%stride:raise ValueError('Partial symbol record')
        span(table_offset,table_size);records=[]
        for position in range(table_offset,table_offset+table_size,stride):
            values=struct.unpack('>'+str(stride//4)+'I',span(position,stride))
            name=string(names+values[0])
            record={'name':name,'offset':values[1]}
            if label=='exports':
                section=values[2]
                if section>=n or values[1]>sections[section]['size']:raise ValueError('Export outside section')
                if elf_name_hash(name)!=values[3]:raise ValueError('Export name hash differs')
                record.update(section=section,name_hash=values[3])
            else:
                if values[1]!=0:raise ValueError('Nonzero imported symbol offset')
                record['first_relocation_offset']=values[2]
            records.append(record)
        tables[label]=records
    relocations={}
    for label,offset,size in [('internal',fields[15],fields[16]),('external',fields[17],fields[18])]:
        if size%12:raise ValueError('Partial relocation record')
        span(offset,size);records=[]
        for position in range(offset,offset+size,12):
            source,packed,target=struct.unpack('>III',span(position,12));index=packed>>8
            rel_type=packed&255
            # Only the four kinds observed and reviewed in these three modules.
            widths={1:4,4:2,6:2,10:4}
            if rel_type not in widths:raise ValueError('Unreviewed relocation kind')
            width=widths[rel_type];span(source,width)
            owners=[s for s in sections if s['file_offset'] and s['file_offset']<=source
                    and source+width<=s['file_offset']+s['size']]
            if len(owners)!=1 or source%width:raise ValueError('Relocation source outside one aligned section field')
            if rel_type==10 and struct.unpack('>I',span(source,4))[0]>>26!=18:
                raise ValueError('REL24 source is not an unconditional branch instruction')
            if label=='external':
                if index>=len(tables['imports']) or target:raise ValueError('Bad external relocation target')
            elif index>=n or target>sections[index]['size']:raise ValueError('Bad internal relocation target')
            records.append({'source_file_offset':source,'source_section':owners[0]['index'],
                            'source_section_offset':source-owners[0]['file_offset'],'field_width':width,
                            'type':rel_type,'index':index,'target_offset':target})
        relocations[label]=records
    # Import references point to byte offsets into the external relocation table.
    external=relocations['external']
    for i,record in enumerate(tables['imports']):
        first=record['first_relocation_offset']
        if first%12 or first>=fields[18] or external[first//12]['index']!=i:
            raise ValueError('Import first-relocation link differs')
        record['relocation_count']=sum(r['index']==i for r in external)
    module=span(name_offset,name_size).rstrip(b'\0').decode('ascii') if name_offset else ''
    return {'module_name':module,'version':version,'size':len(data),'sha1':hashlib.sha1(data).hexdigest(),
            'sha256':hashlib.sha256(data).hexdigest(),'bss_size':bss_size,'sections':sections,
            'entry_points':entries,**tables,'relocations':relocations}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files',nargs='+',type=Path)
    parser.add_argument('--output',type=Path,default=ROOT/'analysis/first_screen/rso_dependencies.json')
    args=parser.parse_args()
    symbols={}
    for line in (ROOT/'decomp/config/RM3E01_00/symbols.txt').read_text().splitlines():
        m=re.match(r'(.+?) = (\S+):0x([0-9a-fA-F]+); // (.*)',line)
        if m:symbols[m[1]]={'address':int(m[3],16),'section':m[2],'function':'type:function' in m[4]}
    ledger=json.loads((ROOT/'decomp/config/RM3E01_00/recovered_leaf.json').read_text())
    recovered={e['address']:e['unit'] for e in ledger}
    recovered.update({0x803057C4:'runtime/strlen.c',0x802D50CC:'startup/region.c',
                     0x8038F998:'platform/alloc_region.c',0x804E524C:'sdk/set_region.c',
                     0x804E526C:'sdk/get_saved_region.c',0x804DF358:'sdk/get_arena_low.c',
                     0x804DF340:'sdk/get_arena_high.c',0x804DF3CC:'sdk/alloc_arena_low.c'})
    modules=[]
    for path in args.files:
        report=inspect_bytes(path.read_bytes());report['file']=str(path)
        for item in report['imports']:
            target=symbols.get(item['name']);item['main_symbol']=target
            item['recovered_source']=recovered.get(target['address']) if target else None
        report['summary']={'imports':len(report['imports']),'exports':len(report['exports']),
           'imports_named_in_main':sum(x['main_symbol'] is not None for x in report['imports']),
           'imports_with_recovered_source':sum(x['recovered_source'] is not None for x in report['imports']),
           'internal_relocations':len(report['relocations']['internal']),
           'external_relocations':len(report['relocations']['external'])}
        modules.append(report)
    result={'revision':'RM3E01_00','format_reference':REFERENCE,'modules':modules,
            'scope':'Read-only metadata, no applied relocations, no RSO rebuilt or native code executed.'}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps([{ 'module':Path(m['file']).name,**m['summary']} for m in modules]))

if __name__=='__main__':main()

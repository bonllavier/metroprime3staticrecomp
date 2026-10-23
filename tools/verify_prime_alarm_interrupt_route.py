#!/usr/bin/env python3
"""Verify how Prime OSInit installs and reaches the decrementer alarm handler."""
from __future__ import annotations
import hashlib,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
DOL=ROOT/'recomp/input/RM3E01_00/main.dol'
EXPECTED='6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104'
def sha(p:Path)->str:return hashlib.sha256(p.read_bytes()).hexdigest()
def main()->int:
    image=DOL.read_bytes()
    if sha(DOL)!=EXPECTED:raise SystemExit('DOL hash mismatch; expected pinned RM3E01_00')
    text_off=[int.from_bytes(image[i:i+4],'big') for i in range(0,0x1c,4)]
    data_off=[int.from_bytes(image[i:i+4],'big') for i in range(0x1c,0x48,4)]
    text_addr=[int.from_bytes(image[i:i+4],'big') for i in range(0x48,0x64,4)]
    data_addr=[int.from_bytes(image[i:i+4],'big') for i in range(0x64,0x90,4)]
    text_size=[int.from_bytes(image[i:i+4],'big') for i in range(0x90,0xac,4)]
    data_size=[int.from_bytes(image[i:i+4],'big') for i in range(0xac,0xd8,4)]
    def read(addr:int,n:int)->bytes:
        for off,base,size in list(zip(text_off,text_addr,text_size))+list(zip(data_off,data_addr,data_size)):
            if size and base<=addr and addr+n<=base+size:return image[off+addr-base:off+addr-base+n]
        raise ValueError(f'Address not present in DOL sections: {addr:#x}')
    def branch_target(addr:int)->int:
        ins=int.from_bytes(read(addr,4),'big')
        if ins>>26!=18 or ins&(1<<1):raise ValueError(f'Expected relative b/bl at {addr:#x}, got {ins:08x}')
        disp=ins&0x03fffffc
        if disp&0x02000000:disp-=0x04000000
        return (addr+disp)&0xffffffff
    expected={0x804DE72C:'7c9043a6',0x804DE730:'808000c0',0x804DE734:'9064000c',0x804DE738:'7c7042a6',0x804DE794:'38600000',0x804DE798:'808000d4',0x804DE7B4:'546515ba',0x804DE7B8:'80a53000',0x804DE7BC:'7cba03a6',0x804DE7C0:'4c000064',0x804DE948:'4bfffdd5',0x804DE94c:'3c80804e',0x804DE950:'3884f028',0x804DE968:'38600008',0x804DE970:'4bfffd99',0x804DF074:'4bfffd88'}
    actual={f'0x{a:08X}':read(a,4).hex() for a in expected}
    for a,b in expected.items():
        if read(a,4).hex()!=b:raise SystemExit(f'Unexpected DOL bytes at {a:#x}: {read(a,4).hex()}')
    target_calls={'0x804DE948':f'0x{branch_target(0x804DE948):08X}','0x804DE970':f'0x{branch_target(0x804DE970):08X}','0x804DF074':f'0x{branch_target(0x804DF074):08X}'}
    if target_calls!={'0x804DE948':'0x804DE71C','0x804DE970':'0x804DE708','0x804DF074':'0x804DEDFC'}:raise SystemExit(f'Unexpected relative targets: {target_calls}')
    symbols=ROOT/'decomp/config/RM3E01_00/symbols.txt'
    text=symbols.read_text(encoding='utf-8')
    if 'OSInit = .text:0x804DE05C; // type:function size:0x404' not in text:raise SystemExit('OSInit symbol/range mismatch')
    for item in ['fn_804DE938 = .text:0x804DE938; // type:function size:0x58','fn_804DF028 = .text:0x804DF028; // type:function size:0x50','fn_804DEDFC = .text:0x804DEDFC; // type:function size:0x22C']:
        if item not in text:raise SystemExit('Expected sized symbol missing: '+item)
    meta=json.loads((ROOT/'recomp/generated/RM3E01_00_conservative/base_translation_output.json').read_text(encoding='utf-8'))
    by={x['entryPoint']:x for x in meta['functions']}
    for addr in [0x804DE05C,0x804DE460,0x804DE708,0x804DE71C,0x804DE72C,0x804DE938,0x804DEDFC,0x804DF028]:
        if addr not in by:raise SystemExit(f'Missing translated function metadata {addr:#x}')
    if 0x804DE938 not in by[0x804DE05C]['build'].get('directCallDependencies',[]):raise SystemExit('OSInit does not directly call alarm init in translated metadata')
    generated={a:ROOT/'recomp/generated/RM3E01_00_conservative/functions'/f'func_{a:08X}.cpp' for a in [0x804DE05C,0x804DE460,0x804DE72C,0x804DE938,0x804DEDFC,0x804DF028]}
    generated_hashes={f'0x{a:08X}':sha(src) for a,src in generated.items()}
    if any(generated_hashes[f'0x{a:08X}']!=by[a]['sha256'] for a in generated):raise SystemExit('Translated source hash mismatch')
    osinit_text=generated[0x804DE05C].read_text(encoding='utf-8')
    if 'InvokeDirectCpu<0x804DE460u>(ctx);' not in osinit_text or 'InvokeDirectCpu<0x804DE938u>(ctx);' not in osinit_text:raise SystemExit('Emitted OSInit source lacks exception/alarm init calls')
    if 'inline leaf 0x804DE708' not in generated[0x804DE938].read_text(encoding='utf-8'):raise SystemExit('Emitted alarm init lacks handler-table setter')
    if 'InvokeDirectCpu<0x804DEDFCu>(ctx);' not in generated[0x804DF028].read_text(encoding='utf-8'):raise SystemExit('Emitted wrapper lacks service dispatch')
    compile_report=json.loads((ROOT/'recomp/checkpoints/20261020_RM3E01_00_conservative_shard_compile.json').read_text(encoding='utf-8'))
    if compile_report['input_sha256']!=EXPECTED or compile_report['compiled_object_count']!=92 or compile_report['failures']:raise SystemExit('Full conservative shard compile checkpoint does not validate for this DOL')
    report={'accepted':True,'verifier':{'path':'recomp/tools/verify_prime_alarm_interrupt_route.py','sha256':sha(Path(__file__).resolve())},'dol_sha256':sha(DOL),'raw_instructions':actual,'relative_branch_targets':target_calls,'exception_vector_template':{'address':'0x804DE72C','size_bytes':152,'early_register_save_pointer':'r4 = *(uint32_t*)0x000000C0; vector stores r3, r4, r5 through it','handler_context_argument_pointer':'r4 = *(uint32_t*)0x000000D4; vector passes this through the alarm wrapper','exception_vector_index':'r3 immediate is patched per vector by FUN_804DE460; alarm setup uses index 8','dispatch_table_base':'0x80003000','dispatch_sequence':'index times 4, load vector handler, write SRR0, rfi','raw_bytes_verified':True},'handler_table_installation':'Ghidra shows FUN_804de938 reads interrupt-handler slot 8, clears alarm head/tail if the old entry is the default 0x804DF028, then calls FUN_804de708(8) with r4 still 0x804DF028; FUN_804de708 stores r4 into the indexed handler table.','wrapper_behavior':'Bytes in sized body fn_804DF028 save registers/context and tail-branch (b, LK=0) to the exact entry 0x804DEDFC. This relative branch was not represented by prior Ghidra xrefs because the wrapper had no Ghidra function object.','translated_entries':generated_hashes,'all_92_translation_shards_compiled_for_this_dol':True,'interpretation':'Prime exception vector dispatches through table slot 8 to wrapper 0x804DF028, which saves PPC OSContext and tail-branches to 0x804DEDFC. The handler receives the interrupt index and pointer loaded from low-memory 0xD4, following vector/wrapper register data flow. WiiCompiled ProcessAlarmQueue also reads 0x800000D4 for callback r4; that argument matches, but its queue base does not.','limitations':['This proves static installation and call routing only; no decrementer interrupt was executed.','The host CpuContext/guest OSContext bridge, callback timing, scheduler effects, and first-screen rendering remain unvalidated.']}
    out=ROOT/'recomp/checkpoints/20261020_prime_alarm_interrupt_route.json'
    out.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2));return 0
if __name__=='__main__':raise SystemExit(main())

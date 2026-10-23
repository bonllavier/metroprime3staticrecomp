#!/usr/bin/env python3
"""Compile the real movie readiness gate with optimized, isolated import fixtures."""
import argparse, hashlib, json, os, re, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--checkpoint',type=Path,required=True)
    a=ap.parse_args()
    work=ROOT/'recomp/.tools/rso-context-contract';work.mkdir(exist_ok=True)
    (work/'guest_abi').mkdir(exist_ok=True);(work/'hle').mkdir(exist_ok=True)
    header='''#pragma once
#include <cstdint>
#include <stdexcept>
#define MKW_RESTRICT __restrict
struct PPC_FPR { double d; };
struct CpuContext { uint32_t gpr[32]{},lr=0,cr=0,xer=0; PPC_FPR fpr[32]{}; };
extern uint32_t guestWords[16384];
namespace MemoryInline {
inline uint32_t FlatRead32(uint32_t a){ if(a>=65536 || a%4)throw std::runtime_error("fixture read bounds"); return guestWords[a/4]; }
inline void FlatWrite32(uint32_t a,uint32_t v){ if(a>=65536 || a%4)throw std::runtime_error("fixture write bounds"); guestWords[a/4]=v; }
inline void FlatWriteRam32(uint32_t a,uint32_t v){ FlatWrite32(a,v); }
}
inline void SetCRResident(uint32_t& cr,uint32_t xer,int field,int32_t a,int32_t b){
uint32_t v=(a<b?8u:0u)|(a>b?4u:0u)|(a==b?2u:0u)|((xer>>31)&1u);int s=(7-field)*4;cr=(cr&~(15u<<s))|(v<<s);}
inline void SetCRResident(uint32_t& cr,uint32_t xer,int field,uint32_t a,uint32_t b){
uint32_t v=(a<b?8u:0u)|(a>b?4u:0u)|(a==b?2u:0u)|((xer>>31)&1u);int s=(7-field)*4;cr=(cr&~(15u<<s))|(v<<s);}
'''
    (work/'guest_abi/guest_abi.h').write_text(header,encoding='utf-8')
    (work/'hle/prime_rso_runtime.h').write_text('''#pragma once
#include "guest_abi/guest_abi.h"
extern "C" uint32_t PPC_RsoRuntimeAddress(uint32_t,uint32_t);
#if LEGACY_IMPORT
extern "C" void PPC_RsoImportCall(uint32_t,uint32_t);
#else
extern "C" void PPC_RsoImportCall(CpuContext*,uint32_t,uint32_t);
#endif
''',encoding='utf-8')
    (work/'provider.cpp').write_text('''#include "hle/prime_rso_runtime.h"
uint32_t guestWords[16384]{};
CpuContext* fixtureCpu=nullptr;
extern "C" uint32_t PPC_RsoRuntimeAddress(uint32_t module,uint32_t address){if(module!=3)throw std::runtime_error("module");return address;}
extern "C" void func_80259954(CpuContext*);
#if LEGACY_IMPORT
extern "C" void PPC_RsoImportCall(uint32_t module,uint32_t instruction){CpuContext* cpu=fixtureCpu;
#else
extern "C" void PPC_RsoImportCall(CpuContext* cpu,uint32_t module,uint32_t instruction){
#endif
if(module!=3 || (instruction!=0x806AB6D8u && instruction!=0x806AB6F8u && instruction!=0x806AB718u))throw std::runtime_error("unexpected fixture import");
func_80259954(cpu);
}
''',encoding='utf-8')
    (work/'driver.cpp').write_text('''#include "guest_abi/guest_abi.h"
#include <algorithm>
#include <cstdio>
extern CpuContext* fixtureCpu;
extern "C" void rso3_func_806AB6A0(CpuContext*);
int main(){int failures=0;
for(int scenario=0;scenario<7;++scenario){
std::fill(std::begin(guestWords),std::end(guestWords),0u);
CpuContext cpu{};fixtureCpu=&cpu;cpu.gpr[1]=0xE000;cpu.gpr[3]=0x1000;cpu.lr=0x12345678;
for(int r=27;r<32;++r)cpu.gpr[r]=0xABCD0000u+r;
MemoryInline::FlatWrite32(0x1000+612,scenario==0?0:14);
unsigned n=0;for(unsigned row=0;row<14;++row)for(unsigned slot=0;slot<3;++slot){
if(row==0 || ((row==4 || row==12)&&slot>0))continue;
unsigned actor=0x2000+n*512,movie=0x8000+n*320;
MemoryInline::FlatWrite32(0x1000+616+row*28+slot*4,actor);
MemoryInline::FlatWrite32(actor+400,scenario==6?0:movie);
bool pending=(scenario==2&&n==0)||(scenario==3&&n==17)||(scenario==4&&n==34);
MemoryInline::FlatWrite32(movie+192,pending?0x1234:0);++n;
}
if(scenario==5)for(unsigned row=0;row<14;++row)for(unsigned slot=0;slot<3;++slot)MemoryInline::FlatWrite32(0x1000+616+row*28+slot*4,0);
rso3_func_806AB6A0(&cpu);
unsigned expected=scenario>=2&&scenario<=4?0:1;
bool preserved=cpu.gpr[1]==0xE000&&cpu.lr==0x12345678;
for(int r=27;r<32;++r)preserved=preserved&&cpu.gpr[r]==0xABCD0000u+r;
std::printf("scenario=%d actual=%u expected=%u preserved=%u\\n",scenario,cpu.gpr[3],expected,preserved);
failures+=(cpu.gpr[3]!=expected || !preserved);
}return failures?1:0;}
''',encoding='utf-8')
    current=ROOT/'recomp/.tools/rso-runtime/ScriptGUIScreenFrontEndUI/functions/rso3_func_806AB6A0.cpp'
    legacy=work/'legacy_gate.cpp'
    query=ROOT/'recomp/generated/RM3E01_00_conservative/functions/func_80259954.cpp'
    body=current.read_text(encoding='utf-8')
    assert body.count('PPC_RsoImportCall(ctx,')==3
    old_body=body.replace('PPC_RsoImportCall(ctx, ','PPC_RsoImportCall(')
    # Captured pre-fix translation: the negative control changes only the
    # three call arguments, and must reproduce that actual source exactly.
    assert hashlib.sha256(old_body.encode()).hexdigest()=='6cd2d2a8cb697425e0a0d251db26b32545859eef8131fee10ef7f83f0477025c'  # normalized LF
    legacy.write_text(old_body,encoding='utf-8',newline='\n')
    clang=ROOT/'recomp/.tools/portable-tools/llvm-mingw/bin/clang++.exe'
    env=dict(os.environ);env['PATH']=str(clang.parent)+os.pathsep+env.get('PATH','')
    runs=[]
    for old,source in [(True,legacy),(False,current)]:
        exe=work/('legacy.exe' if old else 'explicit.exe')
        command=[str(clang),'-std=c++20','-O3','-fno-lto',f'-DLEGACY_IMPORT={int(old)}','-I',str(work),str(source),str(query),str(work/'provider.cpp'),str(work/'driver.cpp'),'-o',str(exe)]
        build=subprocess.run(command,capture_output=True,text=True,env=env)
        assert build.returncode==0,build.stderr
        run=subprocess.run([str(exe)],capture_output=True,text=True,env=env)
        assert run.returncode==(1 if old else 0),(old,run.stdout,run.stderr)
        assert 'scenario=1 actual=0 expected=1' in run.stdout if old else 'scenario=1 actual=1 expected=1' in run.stdout
        runs.append(dict(legacy=old,exit_code=run.returncode,output=run.stdout,source_sha256=sha(source),executable_sha256=sha(exe),command=command))
    modules={}
    for name,expected_count in [('rhbm',196),('Tweaks',298),('ScriptGUIScreenFrontEndUI',249),('ScriptWorldTeleporter',50),('ScriptAchievement',23),('ScriptContextSensitiveActivator',26),('ScriptGrapplePoint',51),('ScriptMultiModelActor',47),('ScriptContextSensitiveAction',35),('ScriptContextActionButtonPressing',35),('Friendly',158),('GunTurret',125)]:
        directory=ROOT/'recomp/.tools/rso-runtime'/name/'functions'
        manifest=json.loads((directory/'translation.json').read_text())
        sources=[directory/row['path'] for row in manifest['functions']]
        assert len(sources)==expected_count, (name,len(sources),expected_count)
        assert len(set(sources))==len(sources)
        for source,row in zip(sources,manifest['functions']):
            assert sha(source)==row['sha256'],source
        sites=[]
        for source in sources:
            calls=re.findall(r'PPC_RsoImportCall\(([^;]+)\);',source.read_text())
            assert all(call.startswith('ctx, ') for call in calls),source
            sites.extend(calls)
        assert sites, f'No imports found for {name}; audit must not pass vacuously'
        modules[name]=dict(functions=len(sources),explicit_context_import_sites=len(sites))
    report=dict(result='PASS optimized original readiness gate and legacy negative control',runs=runs,modules=modules,
        scope='Real translated readiness gate and IsLoading bodies with an abstract word-memory fixture and import provider. No original movie/script state is overridden in the game.',
        sources={str(p.relative_to(ROOT)):sha(p) for p in [current,legacy,query,Path(__file__),clang]})
    a.checkpoint.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(report,indent=2))
if __name__=='__main__':main()

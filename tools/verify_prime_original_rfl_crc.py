#!/usr/bin/env python3
"""Exercise the original translated RFL CRC alarm with bounded memory/alarm fixtures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from prepare_prime_mii_profile import ROOT, HEADER_SIZE, crc16


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", type=Path, required=True)
    args = parser.parse_args()
    work = ROOT / "recomp/.tools/rfl-crc-contract"
    (work / "guest_abi").mkdir(parents=True, exist_ok=True)
    header = r'''#pragma once
#include <cstdint>
#include <stdexcept>
#define MKW_RESTRICT __restrict
struct CpuContext { uint32_t gpr[32]{},lr=0,cr=0,xer=0,ctr=0; };
extern uint8_t memory[1048576];
extern bool alarmPending, callbackDone;
extern uint32_t callbackResult;
namespace MemoryInline {
inline uint32_t Address(uint32_t a,unsigned size){
 if(a>=0x80000000u)a-=0x80000000u;
 if(a>1048576u-size)throw std::runtime_error("fixture memory bounds");return a;}
inline uint8_t FlatRead8(uint32_t a){return memory[Address(a,1)];}
inline uint16_t FlatRead16(uint32_t a){a=Address(a,2);return (uint16_t(memory[a])<<8)|memory[a+1];}
inline uint32_t FlatRead32(uint32_t a){a=Address(a,4);return (uint32_t(memory[a])<<24)|(uint32_t(memory[a+1])<<16)|(uint32_t(memory[a+2])<<8)|memory[a+3];}
inline void FlatWrite16(uint32_t a,uint16_t v){a=Address(a,2);memory[a]=v>>8;memory[a+1]=v;}
inline void FlatWrite32(uint32_t a,uint32_t v){a=Address(a,4);for(unsigned i=0;i<4;i++)memory[a+i]=uint8_t(v>>(24-i*8));}
inline void FlatWriteRam32(uint32_t a,uint32_t v){FlatWrite32(a,v);}
}
template<class T> inline void SetCRResident(uint32_t& cr,uint32_t xer,int field,T a,T b){
 uint32_t bits=(a<b?8u:0u)|(a>b?4u:0u)|(a==b?2u:0u)|((xer>>31)&1u);
 unsigned shift=(7-field)*4;cr=(cr&~(15u<<shift))|(bits<<shift);}
inline uint32_t PpcRotl32Inline(uint32_t v,uint32_t n){n&=31;return n?(v<<n)|(v>>(32-n)):v;}
template<uint32_t Target> void InvokeDirectCpu(CpuContext* ctx){
 if constexpr(Target==0x804DECE4u){alarmPending=false;}
 else if constexpr(Target==0x804DEBF0u){
  if(ctx->gpr[7]!=0x8054AE9Cu)throw std::runtime_error("unexpected alarm target");alarmPending=true;
 } else throw std::runtime_error("unexpected original direct call");
}
inline void InvokeIndirectCpu(uint32_t target,CpuContext* ctx){
 if(target!=1u)throw std::runtime_error("unexpected completion target");
 callbackDone=true;callbackResult=ctx->gpr[3];
}
'''
    (work / "guest_abi/guest_abi.h").write_text(header)
    driver = r'''#include "guest_abi/guest_abi.h"
#include <fstream>
#include <iterator>
#include <vector>
#include <algorithm>
#include <iostream>
uint8_t memory[1048576]{};
bool alarmPending=false,callbackDone=false;
uint32_t callbackResult=0;
extern "C" void func_8054AE9C(CpuContext*);
int main(int argc,char** argv){try{
 if(argc!=2)throw std::runtime_error("input file required");
 std::ifstream f(argv[1],std::ios::binary);if(!f)throw std::runtime_error("input open");
 std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),{});
 if(data.size()>200000)throw std::runtime_error("input too large");
 std::copy(data.begin(),data.end(),memory+65536);
 CpuContext ctx;ctx.gpr[1]=0xF0000;ctx.gpr[13]=0x4000;
 constexpr uint32_t manager=0x1010;
 MemoryInline::FlatWrite32(0x4000-6352,0x1000);
 MemoryInline::FlatWrite32(manager+100,uint32_t(data.size()));
 MemoryInline::FlatWrite32(manager+104,65536);
 MemoryInline::FlatWrite32(manager+108,0);
 MemoryInline::FlatWrite16(manager+112,0);
 MemoryInline::FlatWrite32(manager+168,1);
 MemoryInline::FlatWrite32(0x800000F8,243000000);
 unsigned calls=0;
 do {alarmPending=false;func_8054AE9C(&ctx);if(++calls>50)throw std::runtime_error("alarm loop bound");}
 while(alarmPending&&!callbackDone);
 if(!callbackDone||ctx.gpr[1]!=0xF0000)throw std::runtime_error("completion/stack contract");
 if(!std::equal(data.begin(),data.end(),memory+65536))throw std::runtime_error("input mutated");
 std::cout<<callbackResult<<" "<<calls<<"\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
'''
    (work / "driver.cpp").write_text(driver)
    source = ROOT / "recomp/generated/RM3E01_00_conservative/functions/func_8054AE9C.cpp"
    compiler = ROOT / "recomp/.tools/portable-tools/llvm-mingw/bin/clang++.exe"
    executable = work / "rfl_crc_contract.exe"
    command = [str(compiler), "-std=c++17", "-O3", "-I", str(work),
               str(work / "driver.cpp"), str(source), "-o", str(executable)]
    subprocess.run(command, check=True)
    env = dict(os.environ)
    env["PATH"] = str(compiler.parent) + os.pathsep + env["PATH"]
    database = (ROOT / "recomp/build/RM3E01_00_runtime_link_probe/UserData/NAND/shared2/menu/FaceLib/RFL_DB.dat").read_bytes()
    runs = []
    vectors = [("empty", b"")]
    vectors += [(f"length_{length}", bytes((i * 37 + 11) & 255 for i in range(length)))
                for length in (1, 74, 5119, 5120, 5121)]
    vectors.append(("valid_header", database[:HEADER_SIZE]))
    corrupt = bytearray(database[:HEADER_SIZE]); corrupt[28] ^= 1
    vectors.append(("corrupt_identity", bytes(corrupt)))
    for name, data in vectors:
        candidate = work / f"{name}.bin"; candidate.write_bytes(data)
        result = subprocess.run([str(executable), str(candidate)], env=env, text=True,
                                capture_output=True, check=True)
        actual, alarms = map(int, result.stdout.split())
        expected = crc16(data)
        if actual != expected:
            raise AssertionError((name, actual, expected))
        runs.append({"name": name, "bytes": len(data), "actual_crc": actual,
                     "expected_crc": expected, "alarm_calls": alarms})
    assert runs[-2]["actual_crc"] == 0 and runs[-1]["actual_crc"] != 0
    artifacts = [source, work / "guest_abi/guest_abi.h", work / "driver.cpp", executable,
                 Path(__file__).resolve()]
    report = {"result": "PASS original RFL CRC body: valid header accepted, changed identity rejected",
              "entry": "0x8054AE9C", "runs": runs,
              "scope": "Actual generated CRC routine; abstract bounded RAM and alarm scheduling only. Does not replace any game routine or prove save reload.",
              "artifacts": [{"path": str(p.relative_to(ROOT)),
                             "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for p in artifacts]}
    args.checkpoint.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

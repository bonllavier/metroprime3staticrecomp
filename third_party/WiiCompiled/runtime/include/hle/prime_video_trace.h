#pragma once
#include "guest_abi/guest_abi.h"
#include "runtime_log.h"
#include <array>
#include <atomic>
#include <cstdlib>

inline void PrimeTraceFrontEnd(CpuContext* cpu) {
    static const bool enabled=std::getenv("WII_COMPILED_TRACE_PRIME_MOVIE_CONTROL")!=nullptr;
    if(!enabled)return;
    static std::atomic_uint32_t calls{};
    const unsigned n=calls.fetch_add(1,std::memory_order_relaxed);
    if(n>=4800 || (n>=8 && n%120))return;
    const uint32_t object=cpu->gpr[3];
    Memory::GetPointer(object,616);
    const uint32_t count=Memory::Read32(object+612);
    RT_LOG(RT_TAG_RUNTIME)<<"[prime-front-state] call="<<n<<" object=0x"<<std::hex<<object
        <<" flags=0x"<<unsigned(Memory::Read8(object+608))<<std::dec
        <<" state="<<Memory::Read32(object+360)<<" movies="<<count<<" dt="<<cpu->fpr[1].d<<std::endl;
    if(count>64)return;
    Memory::GetPointer(object+616,count*28);
    for(uint32_t i=0;i<count;++i)for(uint32_t j=0;j<3;++j) {
        const uint32_t actor=Memory::Read32(object+616+i*28+j*4);
        if(!actor)continue;
        Memory::GetPointer(actor,404);
        const uint32_t movie=Memory::Read32(actor+400);
        RT_LOG(RT_TAG_RUNTIME)<<"[prime-front-movie] call="<<n<<" row="<<i<<" slot="<<j
            <<" actor=0x"<<std::hex<<actor<<" movie=0x"<<movie<<std::dec
            <<" loading="<<(movie ? Memory::Read32(movie+192) : 0)
            <<" frames="<<(movie ? Memory::Read32(movie+68) : 0)<<std::endl;
    }
}

// Observe the script messages and guards which select original movie playback.
inline void PrimeTraceMovieControl(CpuContext* cpu,uint32_t address) {
    static const bool enabled=std::getenv("WII_COMPILED_TRACE_PRIME_MOVIE_CONTROL")!=nullptr;
    if(!enabled)return;
    const unsigned slot=address==0x80259474u ? 0 : address==0x80259978u ? 1 : address==0x802598B0u ? 2 : 3;
    static std::array<std::atomic_uint32_t,4> counts{};
    if(counts[slot].fetch_add(1,std::memory_order_relaxed)>=256)return;
    const uint32_t actor=cpu->gpr[3];
    Memory::GetPointer(actor,404);
    const uint32_t movie=Memory::Read32(actor+400);
    const uint32_t event=address==0x80259474u ? Memory::Read32(cpu->gpr[5]+24) : 0;
    RT_LOG(RT_TAG_RUNTIME)<<"[prime-movie-control] function=0x"<<std::hex<<address
        <<" actor=0x"<<actor<<" movie=0x"<<movie<<" event=0x"<<event
        <<" actor_flags=0x"<<Memory::Read32(actor+56)
        <<" current=0x"<<Memory::Read32(cpu->gpr[13]-12644)
        <<" lr=0x"<<cpu->lr<<std::dec
        <<" loading="<<(movie ? Memory::Read32(movie+192) : 0)
        <<" state="<<(movie ? Memory::Read32(movie+280) : 0)
        <<" frames="<<(movie ? Memory::Read32(movie+68) : 0)<<std::endl;
}

// Observation only: no register, guest state, decoder result or frame changes.
inline void PrimeTraceVideo(CpuContext* cpu,uint32_t address,bool leaving) {
    static const bool enabled=std::getenv("WII_COMPILED_TRACE_PRIME_VIDEO")!=nullptr;
    if(!enabled)return;
    const char* requestedFrames=std::getenv("WII_COMPILED_TRACE_PRIME_VIDEO_FRAMES");
    if(address==0x803540A8u && requestedFrames) {
        if(leaving || !cpu->gpr[3] || Memory::Read32(cpu->gpr[3]+68)!=std::strtoul(requestedFrames,nullptr,10))return;
    }
    const unsigned slot=address==0x804EE4A8u ? 0 : address==0x804EB174u ? 1 : address==0x80353F70u ? 2 : 3;
    static std::array<std::atomic_uint32_t,8> counts{};
    const unsigned n=counts[slot*2+unsigned(leaving)].fetch_add(1,std::memory_order_relaxed);
    if(n>=64)return;
    RT_LOG(RT_TAG_RUNTIME)<<"[prime-video-call] address=0x"<<std::hex<<address<<" phase="<<(leaving ? "return" : "enter")
        <<" r3=0x"<<cpu->gpr[3]<<" r4=0x"<<cpu->gpr[4]<<" r5=0x"<<cpu->gpr[5]<<" r6=0x"<<cpu->gpr[6]<<" r7=0x"<<cpu->gpr[7]
        <<" lr=0x"<<cpu->lr<<" hid2=0x"<<cpu->hid2<<" gqr2=0x"<<cpu->gqr[2]
        <<" initialized="<<Memory::Read32(0x8067E580u)<<std::dec<<std::endl;
    if(address==0x803540A8u && !leaving && cpu->gpr[3]) {
        const uint32_t object=cpu->gpr[3];
        Memory::GetPointer(object,316);
        RT_LOG(RT_TAG_RUNTIME)<<"[prime-video-object] address=0x"<<std::hex<<object<<std::dec
            <<" frames="<<Memory::Read32(object+68)<<" components="<<Memory::Read32(object+96)<<" read="<<Memory::Read32(object+224)
            <<" loaded="<<Memory::Read32(object+180)<<" decode_frame="<<Memory::Read32(object+228)
            <<" decode_index="<<Memory::Read32(object+236)<<" requested="<<Memory::Read32(object+296)
            <<" buffered="<<Memory::Read32(object+252)<<" state="<<Memory::Read32(object+280)
            <<" time="<<Memory::ReadFloat32(object+288)<<" interval="<<Memory::ReadFloat32(object+284)<<" dt="<<cpu->fpr[1].d
            <<" io=0x"<<std::hex<<Memory::Read32(object+176)<<std::dec<<std::endl;
        const uint32_t io=Memory::Read32(object+176);
        if(io) {
            const uint32_t table=Memory::Read32(io);
            RT_LOG(RT_TAG_RUNTIME)<<"[prime-video-io] address=0x"<<std::hex<<io<<" vtable=0x"<<table<<" ready_method=0x"<<Memory::Read32(table+20)<<std::dec<<std::endl;
        }
    }
}

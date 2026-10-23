#pragma once
#include "guest_abi/guest_abi.h"
#include "runtime_log.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

// Private, opt-in observation of one real original decoder invocation.
// No guest bytes, registers, return values or playback state are changed.
struct PrimeVideoFrameCapture {
    const char* prefix=nullptr;
    uint32_t call=0,jpeg=0,size=0,y=0,u=0,v=0,width=0,height=0;
    explicit PrimeVideoFrameCapture(CpuContext* cpu) {
        const char* output=std::getenv("WII_COMPILED_DUMP_PRIME_VIDEO_FRAME");
        if(!output)return;
        static std::atomic_uint32_t calls{};
        call=calls.fetch_add(1,std::memory_order_relaxed)+1;
        const char* selected=std::getenv("WII_COMPILED_DUMP_PRIME_VIDEO_CALL");
        if(call!=(selected ? std::strtoul(selected,nullptr,10) : 120))return;
        jpeg=cpu->gpr[3];y=cpu->gpr[4];u=cpu->gpr[5];v=cpu->gpr[6];
        size=Memory::Read32(jpeg-4);
        if(size<4 || size>2*1024*1024)throw std::runtime_error("Unexpected THP component size in reference capture");
        const auto* data=static_cast<const uint8_t*>(Memory::GetPointer(jpeg,size));
        if(data[0]!=255 || data[1]!=216)throw std::runtime_error("THP reference capture has no JPEG SOI");
        uint32_t pos=2;
        while(pos+4<size) {
            if(data[pos++]!=255)throw std::runtime_error("Malformed JPEG marker in private capture");
            while(pos<size && data[pos]==255)++pos;
            if(pos+3>=size)break;
            const uint8_t marker=data[pos++];
            const uint32_t length=(uint32_t(data[pos])<<8)|data[pos+1];
            if(length<2 || pos+length>size)break;
            if(marker==192) {
                if(length<8)break;
                height=(uint32_t(data[pos+3])<<8)|data[pos+4];
                width=(uint32_t(data[pos+5])<<8)|data[pos+6];
                break;
            }
            if(marker==218)break;
            pos+=length;
        }
        if(!width || !height || width%16 || height%16 || width>1024 || height>1024)
            throw std::runtime_error("Unreviewed reference-capture JPEG dimensions");
        prefix=output;
    }
    void Finish(CpuContext* cpu) const {
        if(!prefix)return;
        if(cpu->gpr[3])throw std::runtime_error("Original decoder failed for private reference capture");
        const auto write=[&](const char* suffix,const void* data,size_t bytes) {
            const std::string path=std::string(prefix)+suffix;
            FILE* file=std::fopen(path.c_str(),"wb");
            if(!file)throw std::runtime_error("Cannot create private video reference capture");
            const bool ok=std::fwrite(data,1,bytes,file)==bytes;std::fclose(file);
            if(!ok)throw std::runtime_error("Incomplete private video reference capture");
        };
        write(".jpeg.bin",Memory::GetPointer(jpeg,size),size);
        write(".y.bin",Memory::GetPointer(y,width*height),width*height);
        write(".u.bin",Memory::GetPointer(u,width*height/4),width*height/4);
        write(".v.bin",Memory::GetPointer(v,width*height/4),width*height/4);
        const std::string json="{\"call\":"+std::to_string(call)+",\"jpeg_bytes\":"+std::to_string(size)
            +",\"width\":"+std::to_string(width)+",\"height\":"+std::to_string(height)+",\"return\":0,\"layout\":\"I8 tiles 8x4\"}\n";
        write(".json",json.data(),json.size());
        RT_LOG(RT_TAG_RUNTIME)<<"[prime-video-reference-capture] prefix="<<prefix<<" call="<<call
            <<" width="<<width<<" height="<<height<<" jpeg_bytes="<<size<<std::endl;
    }
};

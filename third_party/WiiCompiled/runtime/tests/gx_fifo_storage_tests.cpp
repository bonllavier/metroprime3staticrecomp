#include "hle/gx_fifo_storage.h"
#include "hle/prime_cp_fifo.h"
#include "hle/prime_fifo_burst.h"
#include "hle/prime_keyboard_mouse.h"
#include "hle/prime_raw_bp.h"
#include "hle/prime_locked_cache_dma.h"
#include "hle/prime_tlut_memory.h"
#include "hle/prime_xfb_ready.h"
#include <iostream>
#include <cstdlib>
static void require_at(bool value,int line) {
    if (!value) { std::cerr << "FIFO assertion failed at line " << line << '\n'; std::abort(); }
}
#define require(value) require_at((value),__LINE__)
int main() {
    {
        PrimeLockedCacheDma::Registers dma;
        unsigned calls=0;
        const auto copy=[&](const PrimeLockedCacheDma::Transfer& t){
            ++calls;require(t.memory==0x102C1540 && t.cache==0xE0000000 && t.bytes==4096 && !t.load);
        };
        dma.WriteUpper(0x102C1540);dma.WriteLower(0xE0000000,copy);require(calls==0);
        dma.WriteLower(0xE0000002,copy);require(calls==1 && !(dma.lower&2));
        // SDK stores 5 blocks as upper length 1, lower length 1.
        const auto partial=PrimeLockedCacheDma::Decode(0x102C1541,0xE0002006);
        require(partial.memory==0x102C1540 && partial.cache==0xE0002000 && partial.bytes==160 && !partial.load);
        const auto load=PrimeLockedCacheDma::Decode(0x102C1540,0xE0000016);
        require(load.bytes==32 && load.load);
        bool failed=false;try{PrimeLockedCacheDma::Decode(0x102C1540,0xE0003FE2);}catch(const std::runtime_error&){failed=true;}
        require(failed);
    }
    {
        using PrimeKeyboardMouse::Key;
        PrimeKeyboardMouse::State input;
        input.Button(Key::Enter,true);require(input.Read().hold==0);
        input.Focus(true);
        input.Button(Key::MouseLeft,true);input.Button(Key::MouseLeft,false);
        require(input.Read().hold==0x800); // A short click survives until one guest sample.
        require(input.Read().hold==0);     // It is not repeated on later retraces.
        input.Button(Key::Enter,true);input.Button(Key::MouseLeft,true);
        require(input.Read().hold==0x800);input.Button(Key::Enter,false);require(input.Read().hold==0x800);
        // This press was already published while held; releasing it must
        // not synthesize another click. Only an unseen short tap is latched.
        input.Button(Key::MouseLeft,false);require(input.Read().hold==0);
        require(input.Read().hold==0);
        input.Button(Key::Space,true);input.Button(Key::Space,false);
        require(input.Read().hold==0x400 && input.Read().hold==0);
        input.Button(Key::W,true);input.Button(Key::D,true);
        auto sample=input.Read();require(std::abs(sample.stickX*sample.stickX+sample.stickY*sample.stickY-1)<1e-6f);
        input.Button(Key::A,true);require(input.Read().stickX==0 && input.Read().stickY==1);
        input.Pointer(320,224,640,448);require(input.Read().pointerValid && input.Read().pointerX==0 && input.Read().pointerY==0);
        input.Pointer(-1,0,640,448);require(!input.Read().pointerValid);
        input.Pointer(320,224,0,448);require(!input.Read().pointerValid);
        input.Pointer(320,224,640,448);input.Leave();require(!input.Read().pointerValid);
        input.Button(Key::Shift,true);input.Focus(false);require(input.Read().hold==0 && !input.Read().pointerValid);
        input.Focus(true);require(input.Read().hold==0 && input.Read().stickX==0 && input.Read().stickY==0);
    }

    // Regression from the original font producer: a retained quad is missing
    // its fourth vertex Y=0 if a command fast path mistakes the burst prefix
    // for NOPs. Preserve the exact tail while stopped, then resume in order.
    const uint8_t tail[]={0,0,0,0,0x43,0x59,0,0,0x3F,0x30,0,0,0x3F,0x1C,0xCC,0xCD};
    std::vector<uint8_t> stopped{0x98,0,4};
    bool cpBlocked=true;
    std::vector<uint8_t> decoded;
    PrimeFifoBurst::Write(tail,sizeof(tail),[&](uint32_t value,unsigned width) {
        require(cpBlocked && width==4);
        for(unsigned i=width;i>0;--i) stopped.push_back(uint8_t(value>>((i-1)*8)));
    });
    require(stopped.size()==19 && stopped[3]==0 && stopped[7]==0x43 && stopped[18]==0xCD);
    cpBlocked=false;
    decoded.insert(decoded.end(),stopped.begin(),stopped.end()); stopped.clear();
    require(decoded.size()==19 && std::memcmp(decoded.data()+3,tail,sizeof(tail))==0);
    // Every unaligned tail length must preserve zeros and command-like bytes.
    const uint8_t mixed[]={0,0,0,0,0x61,0x98,0x10,0xFF,0x08,0,0xFE};
    for(size_t size=0;size<=sizeof(mixed);++size) {
        std::vector<uint8_t> actual;
        PrimeFifoBurst::Write(mixed,size,[&](uint32_t value,unsigned width) {
            require(width==1 || width==2 || width==4);
            for(unsigned i=width;i>0;--i) actual.push_back(uint8_t(value>>((i-1)*8)));
        });
        require(actual.size()==size && (size==0 || std::memcmp(actual.data(),mixed,size)==0));
    }

    PrimeXfbReady::Tracker ready;
    ready.MarkCopy(0x90000800); const auto aRevision = ready.Revision(0x90000800);
    ready.MarkCopy(0x9008C800); const auto bRevision = ready.Revision(0x9008C800);
    require(aRevision && bRevision && ready.Revision(0x90000800)==aRevision);
    ready.Presented(0x90000800,aRevision);
    require(!ready.Revision(0x90000800) && ready.Revision(0x9008C800)==bRevision);
    ready.MarkCopy(0x9008C800); const auto newBRevision = ready.Revision(0x9008C800);
    ready.Presented(0x9008C800,bRevision);
    require(ready.Revision(0x9008C800)==newBRevision);
    ready.Presented(0x9008C800,newBRevision); require(!ready.Revision(0x9008C800));
    uint8_t repeated[8*20]{};
    require(GxFifoStorage::AllDirectVerticesIdentical(repeated,sizeof(repeated),8,20));
    repeated[4*20+7]=1;
    require(!GxFifoStorage::AllDirectVerticesIdentical(repeated,sizeof(repeated),8,20));
    require(!GxFifoStorage::AllDirectVerticesIdentical(repeated,sizeof(repeated)-1,8,20));
    require(!GxFifoStorage::AllDirectVerticesIdentical(repeated,sizeof(repeated),2,80));
    require(!GxFifoStorage::AllDirectVerticesIdentical(nullptr,sizeof(repeated),8,20));
    bool consumerActive = false;
    {
        GxFifoStorage::ConsumerScope outer(consumerActive);
        require(bool(outer) && consumerActive);
        {
            GxFifoStorage::ConsumerScope nested(consumerActive);
            require(!nested && consumerActive);
        }
        require(consumerActive);
    }
    require(!consumerActive);
    try {
        GxFifoStorage::ConsumerScope owner(consumerActive);
        require(bool(owner));
        throw std::runtime_error("simulated backend failure");
    } catch (const std::runtime_error&) {}
    require(!consumerActive);
    // Reproduce the real first Prime packet: header plus 288 24-byte vertices.
    for (size_t packetSize : {6915u, 262149u}) {
        std::vector<uint8_t> data(4096); size_t offset = 0, pending = 0;
        for (size_t i = 0; i < packetSize;) {
            const size_t n = std::min(size_t(4), packetSize-i);
            GxFifoStorage::EnsureAppendSpace(data, offset, pending, n);
            for (size_t j=0;j<n;++j) data[offset+pending+j] = uint8_t((i+j)*37u);
            i+=n;pending+=n;
        }
        require(pending == packetSize);
        for (size_t i=0;i<packetSize;++i) require(data[offset+i] == uint8_t(i*37u));
    }
    std::vector<uint8_t> data(4096); size_t offset=4000;
    for(size_t i=0;i<96;++i) data[offset+i]=uint8_t(i);
    GxFifoStorage::EnsureAppendSpace(data,offset,96,4);
    require(offset==0 && data.size()==4096);
    for(size_t i=0;i<96;++i) require(data[i]==uint8_t(i));
    bool rejected=false;
    try { GxFifoStorage::EnsureAppendSpace(data,offset,96,GxFifoStorage::MaxBufferedBytes); }
    catch(const std::length_error&) { rejected=true; }
    require(rejected && offset==0 && data[95]==95);
    require(PrimeCpFifo::AdvanceBurst(0x1000,0x1040,0x1000)==0x1020);
    require(PrimeCpFifo::AdvanceBurst(0x1000,0x1040,0x1040)==0x1000);
    require(!PrimeCpFifo::AtBreakpoint(0x33,0x1000,0x1020,true));
    require(!PrimeCpFifo::AtBreakpoint(0x33,0x1000,0x1000,false));
    require(!PrimeCpFifo::AtBreakpoint(0x31,0x1000,0x1000,true));
    require(PrimeCpFifo::AtBreakpoint(0x33,0x1000,0x1000,true));
    require(PrimeCpFifo::RetainBreakpoint(0x33,0x1C,0x1020,0x1020));
    require(!PrimeCpFifo::RetainBreakpoint(0x31,0x1C,0x1020,0x1020));
    require(!PrimeCpFifo::RetainBreakpoint(0x33,0x0C,0x1020,0x1020));
    require(PrimeCpFifo::InterruptPending(0x33,0x1C));
    require(!PrimeCpFifo::InterruptPending(0x13,0x1C));
    require(!PrimeCpFifo::InterruptPending(0x32,0x1C));
    PrimeRawBp::Registers bp;
    bp.Write(0x49,0); bp.Write(0x4A,0x6FE7F); bp.Write(0x4B,0x804640);
    bp.Write(0x4D,0x28); bp.Write(0x52,0x4803);
    require(bp.Destination()==0x9008C800u && bp.Width()==640 && bp.Height()==448);
    require(bp.PitchPixels()==640 && bp.IsXfb() && bp.Clear());
    {
        // Original SDK words at the first title-movie CopyTex boundary.
        PrimeRawBp::Registers copy;
        copy.Write(0x49,0);copy.Write(0x4A,0x6FE7F);copy.Write(0x4D,0xA0);copy.Write(0x52,0x10043);
        require(copy.DecodeColorCopy().format==4 && copy.DecodeColorCopy().bytes==573440);
        copy.Write(0x52,0x10853);
        require(copy.DecodeColorCopy().format==5 && copy.Clear() && copy.DecodeColorCopy().bytes==573440);
        copy.Write(0x52,0x10063);copy.Write(0x4D,0x140);
        require(copy.DecodeColorCopy().format==6 && copy.DecodeColorCopy().bytes==1146880);
        for(uint32_t invalid:{0x10243u,0x18043u,0x14043u,0x1000Bu}) {
            copy.Write(0x52,invalid);
            bool failed=false;try{copy.DecodeColorCopy();}catch(const std::runtime_error&){failed=true;}
            require(failed);
        }
        copy.Write(0x52,0x10043);
        bool failed=false;try{copy.DecodeColorCopy();}catch(const std::runtime_error&){failed=true;}
        require(failed); // RGBA stride cannot be accepted for RGB565.
        copy.Write(0x4D,0xA0);copy.Write(0x49,1);
        failed=false;try{copy.DecodeColorCopy();}catch(const std::runtime_error&){failed=true;}
        require(failed); // Source would extend past the logical EFB width.
        copy.Write(0x49,0);copy.Write(0x43,3);
        failed=false;try{copy.DecodeColorCopy();}catch(const std::runtime_error&){failed=true;}
        require(failed); // Depth-source copies are not color RGB565 copies.
    }
    bp.Write(0xFE,1); bp.Write(0x52,0);
    require(bp.values[0x52]==0x4802 && bp.mask==0xFFFFFF);
    bp.Write(0x52,0x4803); require(bp.Write(0x52,0x4803)==0x4803);
    require(PrimeRawBp::Registers::TextureSlot(0x94)==0 && PrimeRawBp::Registers::TextureSlot(0xB7)==7);
    require(PrimeRawBp::Registers::TextureSlot(0x93)==-1);
    bp.Write(0xAB, (14u<<20) | (1023u<<10) | 1023u);
    require(bp.TextureWidth(7)==1024 && bp.TextureHeight(7)==1024 && bp.TextureFormat(7)==14);
    PrimeTlutMemory tlut; uint8_t palette[32];
    for(unsigned i=0;i<32;++i) palette[i]=uint8_t(i);
    tlut.Load(0x80000,palette,32); palette[0]=255;
    require(static_cast<const uint8_t*>(tlut.Span(0x80000,32))[0]==0);
    require(tlut.Generation()==1);
    rejected=false;
    try { tlut.Span(0x80000,64); } catch(const std::runtime_error&) { rejected=true; }
    require(rejected);
    rejected=false;
    try { tlut.Load(1024*1024-16,palette,32); } catch(const std::out_of_range&) { rejected=true; }
    require(rejected);
    // Every possible missing byte must reject a palette, including vector
    // boundaries and tails. Filling the hole must then expose fresh data.
    for(unsigned hole=0;hole<257;++hole) {
        PrimeTlutMemory partial;
        uint8_t bytes[257]{};
        partial.Load(17,bytes,hole);
        partial.Load(17+hole+1,bytes,256-hole);
        bool missing=false;
        try { partial.Span(17,257); } catch(const std::runtime_error&) { missing=true; }
        require(missing);
        bytes[0]=123; partial.Load(17+hole,bytes,1);
        require(static_cast<const uint8_t*>(partial.Span(17,257))[hole]==123);
    }
    std::cout << "FIFO storage: large draw, maximal XF, compaction, overflow rejection passed\n";
}

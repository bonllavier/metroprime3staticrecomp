#include "hle/prime_dl_vertex_expansion.h"
#include "hle/prime_deferred_draw.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

static void CheckAt(bool ok,int line){if(!ok)throw std::runtime_error("Packed DL expansion contract failed at line "+std::to_string(line));}
#define Check(...) CheckAt((__VA_ARGS__),__LINE__)
int main(){
    using namespace PrimeDlVertices;
    // Fixed-width copies preserve every byte and never overwrite a tail.
    for(size_t size=0;size<=64;++size) for(size_t alignment=0;alignment<16;++alignment) {
        std::array<uint8_t,96> input{}, actual{}, expected{};
        for(size_t i=0;i<input.size();++i)input[i]=static_cast<uint8_t>(i*73+19);
        actual.fill(0xA5);expected=actual;
        std::memcpy(expected.data()+alignment,input.data()+15-alignment,size);
        CopyComponents(actual.data()+alignment,input.data()+15-alignment,size);
        Check(actual==expected);
    }
    PublicationTracker publication;
    Check(publication.NeedsPublish(0));publication.Completed(0);
    Check(!publication.NeedsPublish(0) && publication.NeedsPublish(1));
    publication.Completed(1);Check(publication.NeedsPublish(0));
    publication.Invalidate();Check(publication.NeedsPublish(1));
    PublicationTracker nested;Check(nested.NeedsPublish(1)); // Never inherits a parent's proof.
    {
        // Palettes are a separate proof: BP texture/TLUT writes drop only them.
        PublicationTracker p;
        Check(p.NeedsPublish(3) && p.NeedsPalettes(3));
        p.Completed(3);
        Check(!p.NeedsPublish(3) && !p.NeedsPalettes(3));
        p.InvalidatePalettes();
        Check(!p.NeedsPublish(3) && p.NeedsPalettes(3));
        p.Completed(3);p.Invalidate();
        Check(p.NeedsPublish(3) && p.NeedsPalettes(3));
        p.Completed(3);
        Check(p.NeedsPalettes(4) && p.NeedsPublish(4));  // another format proves nothing
        // BP registers: texture, TLUT and texture-selection registers touch the
        // palettes, copies and PE commands everything, colour/blend nothing.
        Check(BpEffectOf(0x88)==BpEffect::Palettes && BpEffectOf(0xA8)==BpEffect::Palettes);  // image0 slots 0 / 4
        Check(BpEffectOf(0x98)==BpEffect::Palettes && BpEffectOf(0xBB)==BpEffect::Palettes);  // TLUT words
        Check(BpEffectOf(0x64)==BpEffect::Palettes && BpEffectOf(0x65)==BpEffect::Palettes);  // TLUT loads
        Check(BpEffectOf(0x00)==BpEffect::Palettes && BpEffectOf(0x28)==BpEffect::Palettes);  // gen mode, TEV order
        Check(BpEffectOf(0x52)==BpEffect::All && BpEffectOf(0x45)==BpEffect::All);            // copy, PE done
        Check(BpEffectOf(0xC0)==BpEffect::None && BpEffectOf(0xE0)==BpEffect::None);          // TEV env, konst
        Check(BpEffectOf(0x41)==BpEffect::None && BpEffectOf(0x40)==BpEffect::None);          // blend, z mode
    }
    {
        // Full-domain mappings exercise the strided path; a separate callback
        // path supplies the reference. Include both largest encoded indices,
        // direct bytes, changing source memory, and unaligned component sizes.
        std::vector<uint8_t> ram(1024*1024),encoded(129*7),fast,reference;
        for(size_t i=0;i<ram.size();++i)ram[i]=uint8_t(i*73+19);
        std::vector<Attribute> mapped{{6,1,1,0x1000,8,ram.data(),ram.size()},
                                      {12,2,1,0x1000,16,ram.data(),ram.size()},
                                      {4,0,1,0,0}};
        auto fallback=mapped;
        for(auto& a:fallback){a.arrayData=nullptr;a.arrayBytes=0;}
        unsigned reads=0;
        const auto callback=[&](uint32_t address,size_t bytes,const uint8_t*& out) {
            ++reads;
            if(address<0x1000 || address-0x1000>ram.size() || bytes>ram.size()-(address-0x1000))return false;
            out=ram.data()+address-0x1000;return true;
        };
        for(unsigned round=0;round<32;++round) {
            for(unsigned vertex=0;vertex<129;++vertex) {
                const unsigned index=vertex==0 ? 65535u : (vertex*521u+round*997u)&65535u;
                const size_t offset=vertex*7;
                encoded[offset]=vertex==0 ? 255 : uint8_t(vertex+round);
                encoded[offset+1]=uint8_t(index>>8);encoded[offset+2]=uint8_t(index);
                for(unsigned byte=3;byte<7;++byte)encoded[offset+byte]=uint8_t(vertex*byte+round);
            }
            ram[round*31]^=uint8_t(round+1);
            reads=0;
            Check(Expand(encoded.data(),encoded.size(),129,mapped,fast,callback,1024*1024));
            Check(reads==0);
            Check(Expand(encoded.data(),encoded.size(),129,fallback,reference,callback,1024*1024));
            Check(reads==258 && fast==reference);
        }
    }
    std::array<uint8_t,256> memory{};
    for(unsigned i=0;i<memory.size();++i)memory[i]=static_cast<uint8_t>(i);
    const auto read=[&](uint32_t address,size_t bytes,const uint8_t*& out){
        if(address<0x1000 || address-0x1000>memory.size() || bytes>memory.size()-(address-0x1000))return false;
        out=memory.data()+address-0x1000;return true;
    };
    std::vector<uint8_t> output;
    std::vector<Attribute> layout{{6,1,1,0x1000,8},{2,0,1,0,0},{4,2,1,0x1080,6}};
    const std::array<uint8_t,10> source{1,0xF8,0x00,0,2,0,0x07,0xE0,0,1};
    Check(Expand(source.data(),source.size(),2,layout,output,read,1024));
    const std::vector<uint8_t> expected{8,9,10,11,12,13,0xF8,0,140,141,142,143,
                                      0,1,2,3,4,5,7,0xE0,134,135,136,137};
    Check(output==expected);
    unsigned mappedReads=0;
    for(auto& attribute:layout) if(attribute.indexBytes) {
        attribute.arrayData=memory.data()+attribute.arrayBase-0x1000;
        attribute.arrayBytes=memory.size()-(attribute.arrayBase-0x1000);
    }
    const auto countedRead=[&](uint32_t address,size_t bytes,const uint8_t*& out) {
        ++mappedReads;return read(address,bytes,out);
    };
    Check(Expand(source.data(),source.size(),2,layout,output,countedRead,1024));
    Check(output==expected && mappedReads==0);
    memory[8]^=1; // A later expansion must observe changed bytes immediately.
    Check(Expand(source.data(),source.size(),2,layout,output,countedRead,1024));
    Check(output[0]==memory[8]);memory[8]^=1;
    for(auto& attribute:layout)attribute.arrayBytes=0;
    Check(Expand(source.data(),source.size(),2,layout,output,countedRead,1024));
    Check(output==expected && mappedReads==4); // Short spans use validated fallback.
    // Normal NBT3's three independent 16-bit indices, with matrix-index byte.
    layout={{1,0,1,0,0},{36,2,3,0x1000,16}};
    const std::array<uint8_t,7> nbt{9,0,2,0,0,0,1};
    Check(Expand(nbt.data(),nbt.size(),1,layout,output,read,1024));
    Check(output.size()==37 && output[0]==9);
    for(unsigned group=0;group<3;++group)for(unsigned byte=0;byte<12;++byte)
        Check(output[1+group*12+byte]==memory[std::array<unsigned,3>{32,0,16}[group]+byte]);
    // Direct IEEE payloads retain NaN and negative-zero bits exactly.
    const std::array<uint8_t,8> bits{0x7F,0xC0,0,1,0x80,0,0,0};
    layout={{8,0,1,0,0}};
    Check(Expand(bits.data(),8,1,layout,output,read,1024));
    Check(std::equal(bits.begin(),bits.end(),output.begin()));
    Check(!Expand(bits.data(),7,1,layout,output,read,1024));
    Check(!Expand(bits.data(),8,1,layout,output,read,4));
    Check(!Expand(bits.data(),8,0,layout,output,read,1024));
    const uint8_t index=255;
    layout={{4,1,1,0x1000,4}};Check(!Expand(&index,1,1,layout,output,read,1024));
    layout={{4,1,1,0xFFFFFFF0u,16}};Check(!Expand(&index,1,1,layout,output,read,1024));
    layout={{4,1,1,0x1000,0}};Check(!Expand(&index,1,1,layout,output,read,1024));
    layout={{4,3,1,0x1000,4}};Check(!Expand(&index,1,1,layout,output,read,1024));
    layout={{4,1,3,0x1000,4}};Check(!Expand(&index,1,1,layout,output,read,1024));
    // Large fully mapped draws may expand independent vertex ranges on two
    // persistent host workers. Compare every byte with the validated scalar
    // fallback, and repeat after mutating the source array to catch stale reads.
    {
        constexpr uint32_t vertices=2048;
        constexpr uint32_t sourceStride=37;
        std::vector<uint8_t> mappedRam(256u*8u),packed(size_t(vertices)*sourceStride),
                            parallelOutput,scalarOutput;
        for(size_t i=0;i<mappedRam.size();++i)mappedRam[i]=uint8_t(i*31u+7u);
        for(uint32_t vertex=0;vertex<vertices;++vertex) {
            const size_t base=size_t(vertex)*sourceStride;
            packed[base]=uint8_t(vertex);
            for(unsigned byte=1;byte<sourceStride;++byte)
                packed[base+byte]=uint8_t(vertex*17u+byte*29u);
        }
        std::vector<Attribute> mappedAttributes{{4,1,1,0x1000,8,mappedRam.data(),mappedRam.size()},
                                                {36,0,1,0,0}};
        auto scalarAttributes=mappedAttributes;
        scalarAttributes[0].arrayData=nullptr;scalarAttributes[0].arrayBytes=0;
        const auto mappedRead=[&](uint32_t address,size_t bytes,const uint8_t*& span) {
            if(address<0x1000 || address-0x1000>mappedRam.size() ||
               bytes>mappedRam.size()-(address-0x1000))return false;
            span=mappedRam.data()+address-0x1000;return true;
        };
        bool usedParallel=false;
        Check(Expand(packed.data(),packed.size(),vertices,mappedAttributes,parallelOutput,
                     mappedRead,1024*1024,&usedParallel));
        Check(Expand(packed.data(),packed.size(),vertices,scalarAttributes,scalarOutput,
                     mappedRead,1024*1024));
        Check(parallelOutput==scalarOutput);
        Check(usedParallel==(std::thread::hardware_concurrency()>2));
        const auto previousOutput=parallelOutput;
        mappedRam[8]^=0x5A;
        Check(Expand(packed.data(),packed.size(),vertices,mappedAttributes,parallelOutput,
                     mappedRead,1024*1024,&usedParallel));
        Check(Expand(packed.data(),packed.size(),vertices,scalarAttributes,scalarOutput,
                     mappedRead,1024*1024));
        Check(parallelOutput==scalarOutput);
        Check(parallelOutput!=previousOutput);
    }
    {
        // Deferred draws: ExpandCannotFail holds only for fully mapped layouts, and
        // the decoder's expansion of the decoded payload equals the producer's.
        std::vector<uint8_t> ram(2u<<20);
        for(size_t i=0;i<ram.size();++i)ram[i]=uint8_t(i*29+5);
        const std::vector<Attribute> layout{{12,2,1,0x1000,12,ram.data(),ram.size()},
                                            {4,0,1,0,0},
                                            {8,1,1,0x2000,8,ram.data(),ram.size()}};
        constexpr uint32_t count=37;
        std::vector<uint8_t> source(count*(2+4+1));
        for(size_t i=0;i<source.size();++i)source[i]=uint8_t(i*131+17);
        Check(ExpandCannotFail(source.size(),count,layout,1u<<24));
        Check(!ExpandCannotFail(source.size()-1,count,layout,1u<<24));      // wrong byte count
        auto unmapped=layout;unmapped[0].arrayData=nullptr;
        Check(!ExpandCannotFail(source.size(),count,unmapped,1u<<24));      // per-index reads: stays on the producer
        auto shortMap=layout;shortMap[2].arrayBytes=255*8;
        Check(!ExpandCannotFail(source.size(),count,shortMap,1u<<24));      // last index not covered
        auto grouped=layout;grouped[0].groups=3;
        Check(!ExpandCannotFail(source.size(),count,grouped,1u<<24));       // NBT3 groups
        const auto noRead=[](uint32_t,size_t,const uint8_t*&){return false;};
        std::vector<uint8_t> direct,payload,deferred;
        Check(Expand(source.data(),source.size(),count,layout,direct,noRead,1u<<24));
        PrimeDeferredDraw::Encode(0x98|3,count,source.data(),uint32_t(source.size()),layout,payload);
        PrimeDeferredDraw::View view{};
        std::vector<Attribute> decoded;
        Check(PrimeDeferredDraw::Decode(payload.data(),payload.size(),view,decoded));
        Check(view.command==(0x98|3) && view.count==count && view.sourceBytes==source.size());
        Check(decoded.size()==layout.size() && decoded[0].arrayData==ram.data() && decoded[2].arrayStride==8);
        Check(Expand(view.source,view.sourceBytes,view.count,decoded,deferred,noRead,1u<<24));
        Check(deferred==direct);
        Check(!PrimeDeferredDraw::Decode(payload.data(),payload.size()-1,view,decoded));   // truncated
        auto bad=payload;bad[0]^=1;
        Check(!PrimeDeferredDraw::Decode(bad.data(),bad.size(),view,decoded));             // magic

        // Snapshots: the array bytes the indices read travel in the payload, so the decoder expands
        // what the producer saw even after the game rewrote the array (Bryyo lava, 2026-11-01).
        std::vector<PrimeDeferredDraw::Snapshot> snapshots(layout.size());
        for(size_t a:{size_t(0),size_t(2)}) {
            const size_t offset=a==0 ? 0 : 2+4;
            const size_t stride=2+4+1;
            uint32_t lo=0xFFFFFFFFu,hi=0;
            for(uint32_t v=0;v<count;++v) {
                const uint8_t* at=source.data()+v*stride+offset;
                const uint32_t index=layout[a].indexBytes==2 ? (uint32_t(at[0])<<8|at[1]) : at[0];
                lo=std::min(lo,index);hi=std::max(hi,index);
            }
            snapshots[a]={lo*layout[a].arrayStride,(hi-lo)*layout[a].arrayStride+layout[a].bytes};
        }
        std::vector<uint8_t> snapPayload,snapExpanded;
        PrimeDeferredDraw::Encode(0x98|3,count,source.data(),uint32_t(source.size()),layout,snapPayload,&snapshots);
        const auto before=ram;
        for(auto& b:ram)b^=0x5A;                                                          // the game rewrites
        std::vector<PrimeDeferredDraw::SnapshotUse> uses;
        Check(PrimeDeferredDraw::Decode(snapPayload.data(),snapPayload.size(),view,decoded,&uses));
        Check(uses.size()==2);
        Check(Expand(view.source,view.sourceBytes,view.count,decoded,snapExpanded,noRead,1u<<24));
        Check(snapExpanded==direct);                                                       // the old values
        Check(std::memcmp(uses[0].live,uses[0].copy,uses[0].size)!=0);                     // and it can tell
        ram=before;
        Check(!PrimeDeferredDraw::Decode(snapPayload.data(),snapPayload.size()-1,view,decoded));
    }
    std::cout<<"PASS mixed 8/16-bit indices, packed colors, NBT3, IEEE bits, rejected bounds/layouts, deferred draws\n";
}

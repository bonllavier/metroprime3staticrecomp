#include "hle/prime_dl_vertex_expansion.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

static void Check(bool ok){if(!ok)throw std::runtime_error("Packed DL expansion contract failed");}
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
    std::cout<<"PASS mixed 8/16-bit indices, packed colors, NBT3, IEEE bits and rejected bounds/layouts\n";
}

#include "../../aurora-main/lib/gx/raw_texture_snapshot_cache.hpp"
#include <array>
#include <memory>
#include <stdexcept>
#include <iostream>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
static void Check(bool ok){if(!ok)throw std::runtime_error("raw texture snapshot contract failed");}
struct CountedKey {
  int value;
  static inline unsigned comparisons=0;
  bool operator==(const CountedKey& other) const {++comparisons;return value==other.value;}
};
int main(){
#ifdef _WIN32
  {
    SYSTEM_INFO info;GetSystemInfo(&info);const size_t page=info.dwPageSize;
    auto* a=static_cast<uint8_t*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    auto* b=static_cast<uint8_t*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    Check(a && b);DWORD old;
    Check(VirtualProtect(a+page,page,PAGE_NOACCESS,&old));
    Check(VirtualProtect(b+page,page,PAGE_NOACCESS,&old));
    for(size_t size=0;size<=512;++size) {
      Check(aurora::gx::source_bytes_equal(a+page-size,b+page-size,size));
      Check(aurora::gx::source_bytes_equal_sse2(a+page-size,b+page-size,size));
      if(size) {
        b[page-1]=1;
        Check(!aurora::gx::source_bytes_equal(a+page-size,b+page-size,size));
        b[page-1]=0;
      }
    }
    VirtualFree(a,0,MEM_RELEASE);VirtualFree(b,0,MEM_RELEASE);
  }
#endif
  {
    std::array<uint8_t,4096> a{},b{};
    for(size_t i=0;i<b.size();++i) {
      b[i]=0x80;
      Check(!aurora::gx::source_bytes_equal(a.data(),b.data(),b.size()));
      Check(!aurora::gx::source_bytes_equal_sse2(a.data(),b.data(),b.size()));
      b[i]=0;
    }
  }
  {
    // Exercise every byte, alignment and tail around the 64-byte fast loop.
    std::array<uint8_t,320> a{}, b{};
    for(size_t offset=0;offset<32;++offset) for(size_t size=0;size<260;++size) {
      Check(aurora::gx::source_bytes_equal(a.data()+offset,b.data()+offset,size));
      for(size_t changed=0;changed<size;++changed) {
        b[offset+changed]=1;
        Check(!aurora::gx::source_bytes_equal(a.data()+offset,b.data()+offset,size));
        b[offset+changed]=0;
      }
    }
  }
  {
    aurora::gx::RawTextureSnapshotCache<CountedKey,int> recent(4096,512);
    const uint8_t source=123;
    for(int i=0;i<262;++i) recent.Store({i},&source,1,i+1);
    CountedKey::comparisons=0;
    for(int i=0;i<1000;++i) Check(recent.Find({261},&source,1)==262);
    Check(CountedKey::comparisons==1000); // One probe per repeated recent binding.
    const uint8_t changed=124;
    Check(recent.Find({261},&changed,1)==0); // Recency never skips byte validation.
  }
  {
    aurora::gx::RawTextureSnapshotCache<CountedKey,int> indexed(4096,512);
    const uint8_t source=91;
    for(int i=0;i<262;++i)indexed.Store({i},&source,1,i+1,nullptr,0,static_cast<size_t>(i),true);
    CountedKey::comparisons=0;
    for(int i=0;i<1000;++i)Check(indexed.Find({0},&source,1,nullptr,0,0,true)==1);
    Check(CountedKey::comparisons==1000); // A cold LRU key takes one indexed probe, not 262 list scans.
    Check(indexed.Stats().keyProbes==1000);
    // Hash collisions still require full key and byte equality.
    indexed.Store({1000},&source,1,1001,nullptr,0,7,true);
    indexed.Store({1001},&source,1,1002,nullptr,0,7,true);
    Check(indexed.Find({1001},&source,1,nullptr,0,7,true)==1002);
    Check(indexed.Find({1000},&source,1,nullptr,0,7,true)==1001);
    aurora::gx::RawTextureSnapshotCache<CountedKey,int> eviction(4096,2);
    eviction.Store({1},&source,1,11,nullptr,0,1,true);
    eviction.Store({2},&source,1,22,nullptr,0,2,true);
    eviction.Store({3},&source,1,33,nullptr,0,3,true); // Eviction removes the stale index node too.
    Check(eviction.Find({1},&source,1,nullptr,0,1,true)==0);
    Check(eviction.Find({2},&source,1,nullptr,0,2,true)==22);
    eviction.Store({1},&source,1,111,nullptr,0,1,true);
    Check(eviction.Find({1},&source,1,nullptr,0,1,true)==111);
  }
  {
    aurora::gx::RawTextureSnapshotCache<int,int> generations(4096,8);
    std::array<uint8_t,8> source{1,2,3,4,5,6,7,8};
    generations.Store(1,source.data(),source.size(),11,nullptr,0,101,true,7,0,true);
    Check(generations.Find(1,source.data(),source.size(),nullptr,0,101,true,7,0,true)==11);
    Check(generations.Stats().generationHits==1 && generations.Stats().byteValidations==0);
    source[3]=99;
    Check(generations.Find(1,source.data(),source.size(),nullptr,0,101,true,8,0,true)==0);
    Check(generations.Stats().byteValidations==1); // A changed write generation falls back to exact bytes.
    Check(generations.Find(1,source.data(),source.size(),nullptr,0,101,true,7,0,false)==0);
    Check(generations.Stats().byteValidations==2); // Untracked sources never skip byte validation.
    std::array<uint8_t,2> palette{0xF8,0x00};
    generations.Store(2,source.data(),source.size(),22,palette.data(),palette.size(),202,true,8,3,true);
    Check(generations.Find(2,source.data(),source.size(),palette.data(),palette.size(),202,true,8,3,true)==22);
    Check(generations.Find(2,source.data(),source.size(),palette.data(),palette.size(),202,true,8,4,true)==22);
    Check(generations.Stats().byteValidations==3); // Palette writes also force exact byte validation.
  }
  using Cache=aurora::gx::RawTextureSnapshotCache<int,std::shared_ptr<int>>;
  Cache cache(12,2);
  std::array<uint8_t,6> bytes{1,2,3,4,5,6};
  auto first=std::make_shared<int>(1);
  cache.Store(1,bytes.data(),bytes.size(),first);
  Check(cache.Find(1,bytes.data(),bytes.size())==first);
  Check(!cache.Find(2,bytes.data(),bytes.size())); // Format/dimension key changed.
  Check(!cache.Find(1,bytes.data(),bytes.size()-1));
  bytes[0]^=1;Check(!cache.Find(1,bytes.data(),bytes.size()));bytes[0]^=1;
  bytes[5]^=1;Check(!cache.Find(1,bytes.data(),bytes.size()));
  auto replacement=std::make_shared<int>(2);
  cache.Store(1,bytes.data(),bytes.size(),replacement);
  Check(cache.EntryCount()==1 && cache.ByteSize()==6);
  Check(cache.Find(1,bytes.data(),bytes.size())==replacement);
  Check(*first==1); // Submitted draw's old resource remains valid.
  cache.Store(2,bytes.data(),bytes.size(),first);
  Check(cache.Find(1,bytes.data(),bytes.size())==replacement); // Promote key 1.
  cache.Store(3,bytes.data(),bytes.size(),first);
  Check(!cache.Find(2,bytes.data(),bytes.size()));
  Check(cache.Find(1,bytes.data(),bytes.size())==replacement);
  Check(cache.ByteSize()==12 && cache.EntryCount()==2);
  std::array<uint8_t,13> huge{};
  cache.Store(4,huge.data(),huge.size(),first);
  Check(cache.EntryCount()==2 && cache.ByteSize()==12);
  Cache budget(8,20);
  budget.Store(1,bytes.data(),6,first);budget.Store(2,bytes.data(),6,replacement);
  Check(budget.EntryCount()==1 && budget.ByteSize()==6 && !budget.Find(1,bytes.data(),6));
  Check(!budget.Find(2,nullptr,6));
  Cache disabled(8,0);disabled.Store(1,bytes.data(),6,first);Check(disabled.EntryCount()==0);
  // First-scene working set: the 64-entry cap thrashed below its byte budget.
  // Keep the same byte budget; extra entries must retain identical data, and
  // promotion must still evict the least recently used item at the new limit.
  Cache scene(4096,512), legacy(4096,64);
  std::array<uint8_t,4> small{1,2,3,4};
  for(int key=0;key<200;++key) {
    scene.Store(key,small.data(),small.size(),first);
    legacy.Store(key,small.data(),small.size(),first);
  }
  Check(scene.EntryCount()==200 && scene.ByteSize()==800);
  Check(!legacy.Find(0,small.data(),small.size()));
  for(int round=0;round<4;++round)
    for(int key=0;key<200;++key)Check(scene.Find(key,small.data(),small.size())==first);
  small[3]=5;Check(!scene.Find(199,small.data(),small.size()));small[3]=4;
  Check(scene.Stats().hits==800 && scene.Stats().misses==1 && scene.Stats().lookups==801);
  Check(legacy.Stats().evictions==136 && scene.Stats().evictions==0);
  Cache order(100,3);
  order.Store(1,small.data(),4,first);order.Store(2,small.data(),4,first);order.Store(3,small.data(),4,first);
  Check(order.Find(1,small.data(),4)==first); // Move oldest to newest.
  order.Store(4,small.data(),4,first);
  Check(!order.Find(2,small.data(),4) && order.Find(1,small.data(),4)==first);
  // Indexed textures require a stable snapshot of texture AND TLUT bytes.
  // Palette reloads can preserve the address and version; mutations must miss.
  Cache indexed(12,4);
  std::array<uint8_t,4> indices{0x01,0x23,0x45,0x67};
  std::array<uint8_t,4> tlut{0xF8,0x00,0x07,0xE0};
  indexed.Store(1,indices.data(),4,first,tlut.data(),4);
  Check(indexed.ByteSize()==8 && indexed.Find(1,indices.data(),4,tlut.data(),4)==first);
  tlut[3]^=1;Check(!indexed.Find(1,indices.data(),4,tlut.data(),4));tlut[3]^=1;
  indices[0]^=1;Check(!indexed.Find(1,indices.data(),4,tlut.data(),4));indices[0]^=1;
  Check(!indexed.Find(2,indices.data(),4,tlut.data(),4)); // Format/size identity differs.
  Check(!indexed.Find(1,indices.data(),4,nullptr,4));
  Check(!indexed.Find(1,indices.data(),4,tlut.data(),2));
  Check(!indexed.Find(1,indices.data(),4)); // Cannot omit the second span.
  auto retained=indexed.Find(1,indices.data(),4,tlut.data(),4);
  tlut[1]^=1;indexed.Store(1,indices.data(),4,replacement,tlut.data(),4);
  Check(indexed.EntryCount()==1 && indexed.ByteSize()==8 && retained==first);
  Check(indexed.Find(1,indices.data(),4,tlut.data(),4)==replacement);
  indexed.Store(2,indices.data(),4,first,tlut.data(),4);
  Check(indexed.EntryCount()==1 && indexed.ByteSize()==8); // Combined byte budget evicts key 1.
  Check(!indexed.Find(1,indices.data(),4,tlut.data(),4));
  indexed.Store(3,indices.data(),4,first,nullptr,4);Check(indexed.EntryCount()==1);
  indexed.Store(3,indices.data(),4,first,tlut.data(),static_cast<size_t>(-1));Check(indexed.EntryCount()==1);
  Cache alternating(24,3,true);
  tlut[0]=1;alternating.Store(1,indices.data(),4,first,tlut.data(),4);
  tlut[0]=2;alternating.Store(1,indices.data(),4,replacement,tlut.data(),4);
  Check(alternating.EntryCount()==2 && alternating.ByteSize()==16);
  for(unsigned round=0;round<100;++round) {
    tlut[0]=1;Check(alternating.Find(1,indices.data(),4,tlut.data(),4)==first);
    tlut[0]=2;Check(alternating.Find(1,indices.data(),4,tlut.data(),4)==replacement);
  }
  tlut[0]=3;Check(!alternating.Find(1,indices.data(),4,tlut.data(),4));
  alternating.Store(1,indices.data(),4,first,tlut.data(),4);
  tlut[0]=4;alternating.Store(1,indices.data(),4,first,tlut.data(),4);
  Check(alternating.ByteSize()==24 && alternating.EntryCount()==3);
  tlut[0]=1;Check(!alternating.Find(1,indices.data(),4,tlut.data(),4)); // LRU version evicted.
  std::cout<<"PASS byte mutations, metadata changes, replacement, LRU/byte bounds and retained draw ownership\n";
}

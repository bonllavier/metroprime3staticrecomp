#include <xxhash.h>
#include <vector>
#include <iostream>
#include <chrono>
#include <iomanip>
int main(int argc,char**){
 std::vector<unsigned char> bytes(1048592);uint32_t x=0x12345678;
 for(auto&b:bytes){x^=x<<13;x^=x>>17;x^=x<<5;b=static_cast<unsigned char>(x);}
 if(argc>1){
  auto fn=&XXH3_64bits_withSeed; decltype(fn) volatile call=fn;
  uint64_t checksum=0;auto t=std::chrono::steady_clock::now();
  for(unsigned i=0;i<100000;++i)checksum+=call(bytes.data()+i%16,2768,i%7);
  std::cout<<"ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count()<<" checksum="<<checksum<<"\n";return 0;
 }
 for(unsigned offset=0;offset<16;++offset)for(unsigned size=0;size<4096;size+=size<256?1:31)for(uint64_t seed:{0ull,1ull,0xFEDCBA9876543210ull}){
  auto p=bytes.data()+offset;auto h=XXH3_128bits_withSeed(p,size,seed);
  std::cout<<std::hex<<XXH32(p,size,static_cast<unsigned>(seed))<<' '<<XXH64(p,size,seed)<<' '<<XXH3_64bits_withSeed(p,size,seed)<<' '<<h.low64<<' '<<h.high64<<'\n';
 }
 for(unsigned size:{65535u,65536u,131072u,174752u,1048576u})for(unsigned offset=0;offset<16;++offset)std::cout<<std::hex<<XXH3_64bits_withSeed(bytes.data()+offset,size,1)<<'\n';
}

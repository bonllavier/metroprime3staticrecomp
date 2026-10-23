#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
#include <immintrin.h>
#endif

namespace aurora::gx {
// Equality only: unlike memcmp, no ordering or first differing byte is needed.
// All loads remain inside the supplied spans, including unaligned tails.
inline bool source_bytes_equal_sse2(const void* left, const void* right, size_t size) {
  const auto* a=static_cast<const uint8_t*>(left);
  const auto* b=static_cast<const uint8_t*>(right);
#if defined(__SSE2__) || defined(_M_X64)
  while(size>=64) {
    const auto d0=_mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a)),
                               _mm_loadu_si128(reinterpret_cast<const __m128i*>(b)));
    const auto d1=_mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+16)),
                               _mm_loadu_si128(reinterpret_cast<const __m128i*>(b+16)));
    const auto d2=_mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+32)),
                               _mm_loadu_si128(reinterpret_cast<const __m128i*>(b+32)));
    const auto d3=_mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+48)),
                               _mm_loadu_si128(reinterpret_cast<const __m128i*>(b+48)));
    const auto different=_mm_or_si128(_mm_or_si128(d0,d1),_mm_or_si128(d2,d3));
    if(_mm_movemask_epi8(_mm_cmpeq_epi8(different,_mm_setzero_si128()))!=0xffff) return false;
    a+=64;b+=64;size-=64;
  }
#endif
  return size==0 || std::memcmp(a,b,size)==0;
}
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
__attribute__((target("avx2"))) inline bool source_bytes_equal_avx2(
    const void* left,const void* right,size_t size) {
  const auto* a=static_cast<const uint8_t*>(left);
  const auto* b=static_cast<const uint8_t*>(right);
  while(size>=128) {
    const auto d0=_mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a)),
                                  _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b)));
    const auto d1=_mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+32)),
                                  _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b+32)));
    const auto d2=_mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+64)),
                                  _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b+64)));
    const auto d3=_mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+96)),
                                  _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b+96)));
    const auto different=_mm256_or_si256(_mm256_or_si256(d0,d1),_mm256_or_si256(d2,d3));
    if(!_mm256_testz_si256(different,different))return false;
    a+=128;b+=128;size-=128;
  }
  return source_bytes_equal_sse2(a,b,size);
}
#endif
inline bool source_bytes_equal(const void* left,const void* right,size_t size) {
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
  // Includes OSXSAVE/XGETBV support; unsupported hosts retain the baseline.
  static const bool avx2=__builtin_cpu_supports("avx2");
  if(size>=128 && avx2)return source_bytes_equal_avx2(left,right,size);
#endif
  return source_bytes_equal_sse2(left,right,size);
}
}

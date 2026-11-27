#pragma once

// Inline gather-pipe stores for translated code (26/11/2026). The translator emits a call to
// GX_HLE_FIFO_Write8/16/32/Float for every store it recognises to 0xCC008000; in scenes full of
// particles those calls (one per word, ~5 million a second) were ~5 % of the game thread. Shards
// include this header after guest_abi.h: the append that stays inside the open 32-byte burst
// (PrimeGatherAppendFast in memory.cpp, same checks in the same order) happens in the translated
// function, and only the store that completes a burst, or any case the fast path does not take,
// calls the out-of-line function, which tries the same append again and then commits.
// Building with -DMKW_NO_INLINE_GATHER=1 keeps the calls.

#include <atomic>
#include <cstdint>
#include <cstring>

#include "guest_abi/gather_state.h"
#include "guest_abi/memory_access.h"

// Shards are compiled without the game-profile defines (profile-neutral objects): the header does
// not test MKW_GAME_PROFILE_RM3E01_00. A runtime built without that profile defines g_mkwGatherState
// as permanently "replaying", so the inline path always falls back to the call.
#if !(defined(MKW_NO_INLINE_GATHER) && MKW_NO_INLINE_GATHER)


inline bool MkwGatherAppendInline(uint32_t value, unsigned width) {
    const MkwGatherState& st = g_mkwGatherState;
    if (*st.replaying) return false;
    const unsigned count = *st.count;
    if (count + width >= 32u) return false;  // this store completes the burst: commit path
    const uint32_t base = st.pi[0].load(std::memory_order_acquire);
    const uint32_t end = st.pi[1].load(std::memory_order_acquire);
    const uint32_t address = st.pi[2].load(std::memory_order_acquire) & ~0x20000000u;
    if (!(end > base && address >= base && address <= end && ((base | end | address) & 31u) == 0)) return false;
    uint8_t* bytes = st.pending + count;
    if (width == 4) {
        bytes[0] = uint8_t(value >> 24); bytes[1] = uint8_t(value >> 16);
        bytes[2] = uint8_t(value >> 8); bytes[3] = uint8_t(value);
    } else if (width == 2) {
        bytes[0] = uint8_t(value >> 8); bytes[1] = uint8_t(value);
    } else {
        bytes[0] = uint8_t(value);
    }
    *st.count = count + width;
    return true;
}

inline void MkwGatherWrite8(uint8_t v) { if (!MkwGatherAppendInline(v, 1)) GX_HLE_FIFO_Write8(v); }
inline void MkwGatherWrite16(uint16_t v) { if (!MkwGatherAppendInline(v, 2)) GX_HLE_FIFO_Write16(v); }
inline void MkwGatherWrite32(uint32_t v) { if (!MkwGatherAppendInline(v, 4)) GX_HLE_FIFO_Write32(v); }
inline void MkwGatherWriteFloat(float v) {
    uint32_t raw;
    std::memcpy(&raw, &v, sizeof(raw));
    if (!MkwGatherAppendInline(raw, 4)) GX_HLE_FIFO_WriteFloat(v);
}

// After the wrappers, so they still call the real functions.
#define GX_HLE_FIFO_Write8(v) MkwGatherWrite8(v)
#define GX_HLE_FIFO_Write16(v) MkwGatherWrite16(v)
#define GX_HLE_FIFO_Write32(v) MkwGatherWrite32(v)
#define GX_HLE_FIFO_WriteFloat(v) MkwGatherWriteFloat(v)

#endif

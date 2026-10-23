#pragma once
#include <array>
#include <cstdint>

namespace PrimeRsoRelocations {
inline bool MapSection(uint32_t source, uint32_t nominal, uint32_t runtime,
                       uint32_t size, bool reverse, uint32_t& destination) {
    const uint32_t first = reverse ? runtime : nominal;
    const uint32_t second = reverse ? nominal : runtime;
    if (!size || source < first || source - first >= size) return false;
    destination = second + (source - first);
    return true;
}
inline uint32_t SignedImmediate(uint32_t word, bool shifted) {
    const uint32_t value = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(word)));
    return shifted ? value << 16u : value;
}
inline uint16_t HighAdjusted(uint32_t address) { return static_cast<uint16_t>((address + 0x8000u) >> 16u); }
inline uint32_t BranchTarget(uint32_t instruction, uint32_t address) {
    uint32_t displacement = instruction & 0x03FFFFFCu;
    if (displacement & 0x02000000u) displacement |= 0xFC000000u;
    return (instruction & 2u) ? displacement : address + displacement;
}
inline bool DecodeSdkThunk(const std::array<uint32_t, 6>& words, uint32_t& target) {
    if (words[0] != 0x91810004u || (words[1] & 0xFFFF0000u) != 0x3D800000u ||
        (words[2] & 0xFFFF0000u) != 0x398C0000u || words[3] != 0x7D8903A6u ||
        words[4] != 0x81810004u || words[5] != 0x4E800420u) return false;
    target = (words[1] << 16u) + SignedImmediate(words[2], false);
    return true;
}
} // namespace PrimeRsoRelocations

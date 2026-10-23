#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>
namespace PrimeRawBp {
struct Registers {
    std::array<uint32_t, 256> values{};
    uint32_t mask = 0xFFFFFFu;
    uint32_t Write(uint8_t reg, uint32_t value) {
        value &= 0xFFFFFFu;
        if (reg == 0xFE) { mask = value; return value; }
        values[reg] = (values[reg] & ~mask) | (value & mask);
        mask = 0xFFFFFFu;
        return values[reg];
    }
    uint32_t Destination() const { return (values[0x4B] << 5) | 0x80000000u; }
    uint16_t Left() const { return uint16_t(values[0x49] & 1023u); }
    uint16_t Top() const { return uint16_t((values[0x49] >> 10) & 1023u); }
    uint16_t Width() const { return uint16_t((values[0x4A] & 1023u) + 1u); }
    uint16_t Height() const { return uint16_t(((values[0x4A] >> 10) & 1023u) + 1u); }
    uint16_t PitchPixels() const { return uint16_t((values[0x4D] & 1023u) * 16u); }
    static int TextureSlot(uint8_t reg) {
        if (reg >= 0x94 && reg <= 0x97) return reg - 0x94;
        if (reg >= 0xB4 && reg <= 0xB7) return reg - 0xB4 + 4;
        return -1;
    }
    uint32_t TextureImage0(unsigned slot) const { return values[0x88 + (slot & 3u) + (slot >= 4 ? 0x20 : 0)]; }
    uint32_t TextureMode0(unsigned slot) const { return values[0x80 + (slot & 3u) + (slot >= 4 ? 0x20 : 0)]; }
    uint32_t TextureMode1(unsigned slot) const { return values[0x84 + (slot & 3u) + (slot >= 4 ? 0x20 : 0)]; }
    uint16_t TextureWidth(unsigned slot) const { return uint16_t((TextureImage0(slot) & 1023u) + 1u); }
    uint16_t TextureHeight(unsigned slot) const { return uint16_t(((TextureImage0(slot) >> 10) & 1023u) + 1u); }
    uint32_t TextureFormat(unsigned slot) const { return (TextureImage0(slot) >> 20) & 15u; }
    uint32_t IndexedTextureMask() const {
        uint32_t mask=0;
        for (unsigned slot=0;slot<8;++slot) {
            const auto format=TextureFormat(slot);
            if (format>=8 && format<=10) mask|=1u<<slot;
        }
        return mask;
    }
    bool TextureMips(unsigned slot) const { return ((TextureMode0(slot) >> 5) & 3u) != 0; }
    bool IsXfb() const { return (values[0x52] & (1u << 14)) != 0; }
    bool Clear() const { return (values[0x52] & (1u << 11)) != 0; }
    struct ColorCopy { uint32_t format, bytes; uint16_t width, height; bool halfScale; };
    ColorCopy DecodeColorCopy() const {
        const uint32_t trigger=values[0x52];
        // Inverse of RM3E01 GXSetTexCopyDst 0x804D3184: low three
        // format bits go to BP bits 4..6; format bit 3 stays in bit 3.
        const uint32_t format=((trigger>>4)&7u)|(trigger&8u);
        if((trigger&~0x010A7Bu)!=0 || (trigger&0x018000u)!=0x010000u || (format!=0 && (format<4 || format>7)))
            throw std::runtime_error("Unsupported Prime raw color texture-copy mode");
        if((values[0x43]&7u)>2u)
            throw std::runtime_error("Prime color texture-copy adapter does not support depth or single-channel EFB sources");
        // Observed A8 edge copy: x=638,width=8. Retain its tile-sized output
        // and sample extent; the backend clamps only the out-of-EFB tail.
        const bool a8RightTail=format==7 && !(trigger&0x200u) && Left()<640 && Left()+Width()<=647;
        if(Left()>=640 || Top()>=528 || (Left()+Width()>640 && !a8RightTail) || Top()+Height()>528)
            throw std::runtime_error("Prime texture-copy rectangle exceeds logical EFB");
        const bool halfScale=(trigger&0x200u)!=0;
        // The original SDK's mipmap flag is a 2:1 box-filter copy, not a
        // request to generate a mip chain. Keep odd/degenerate sources explicit
        // until their edge sampling is verified against hardware.
        if(halfScale && ((Width()&1u) || (Height()&1u) || Width()<2 || Height()<2))
            throw std::runtime_error("Prime half-scale texture copy has unverified odd or degenerate source dimensions");
        const uint16_t width=uint16_t(Width()>>(halfScale ? 1 : 0));
        const uint16_t height=uint16_t(Height()>>(halfScale ? 1 : 0));
        const uint32_t blockBytes=format==6 ? 64 : 32;
        const uint32_t blockWidth=(format==0 || format==7) ? 8 : 4;
        const uint32_t blockHeight=format==0 ? 8 : 4;
        const uint32_t stride=((width+blockWidth-1u)/blockWidth)*blockBytes;
        if(((values[0x4D]&1023u)<<5)!=stride)
            throw std::runtime_error("Unsupported Prime texture-copy destination stride");
        // With BP bit15 clear the SDK uses channel-copy formats, not
        // intensity: nibble 0 is GX_CTF_R4 (0x20), 7 is GX_CTF_A8 (0x27).
        const uint32_t hostFormat=(format==0 || format==7) ? format|0x20u : format;
        return {hostFormat,stride*((height+blockHeight-1u)/blockHeight),width,height,halfScale};
    }
};
}

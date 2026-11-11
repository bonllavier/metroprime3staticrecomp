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
        // GXCopyTex switches the PE pixel format to Z24 (3) for depth copies.
        const bool depth=(values[0x43]&7u)==3u;
        uint32_t depthFormat=0, depthBlockWidth=4, depthBlockHeight=4, depthBlockBytes=32;
        bool intensity=false, channel=false;
        if(depth) {
            if((trigger&~0x010A7Bu)!=0 || (trigger&0x018000u)!=0x010000u)
                throw std::runtime_error("Unsupported Prime raw depth texture-copy mode");
            // GXSetTexCopyDst nibbles for the depth formats Aurora resolves from the
            // depth buffer: GX_TF_Z8 = 1, GX_TF_Z24X8 = 6, GX_TF_Z16 = 0xB (the SDK
            // remaps Z16; libogc GX_SetTexCopyDst) and the GX_CTF_Z8M/Z8L/Z16L channel
            // formats 0/9/0xA/0xC (Z4, Z8M, Z8L, Z16L). The scan visor copies Z16 and then Z8L.
            switch(format) {
            case 0x0u: depthFormat=0x30u; depthBlockWidth=8; depthBlockHeight=8; break; // GX_CTF_Z4: 8x8, 4 bpp
            case 0x1u: depthFormat=0x11u; depthBlockWidth=8; break;   // GX_TF_Z8: 8x4 blocks
            case 0x9u: depthFormat=0x39u; depthBlockWidth=8; break;   // GX_CTF_Z8M: 8x4 blocks
            case 0xAu: depthFormat=0x3Au; depthBlockWidth=8; break;   // GX_CTF_Z8L: 8x4 blocks
            case 0xBu: depthFormat=0x13u; break;                      // GX_TF_Z16: 4x4, 16 bpp
            case 0xCu: depthFormat=0x3Cu; break;                      // GX_CTF_Z16L: 4x4, 16 bpp
            case 0x6u: depthFormat=0x16u; depthBlockBytes=64; break;  // GX_TF_Z24X8: 4x4, 32 bpp
            default: throw std::runtime_error("Unsupported Prime raw depth texture-copy format");  // 2-5, 7, 8, 0xD-0xF
            }
        } else if((trigger&0x018000u)==0x018000u) {
            // BP bit 15 selects the intensity (RGB->Y) copy formats GX_TF_I4/I8/IA4/IA8,
            // nibbles 0..3 (libogc GX_SetTexCopyDst). The scan visor copies the whole
            // EFB at half scale as I8.
            if((trigger&~0x018A7Bu)!=0 || format>3)
                throw std::runtime_error("Unsupported Prime raw intensity texture-copy mode");
            if((values[0x43]&7u)>2u)
                throw std::runtime_error("Prime color texture-copy adapter does not support single-channel EFB sources");
            depthFormat=format;                       // GX_TF_I4 = 0 ... GX_TF_IA8 = 3
            depthBlockWidth=format==3 ? 4 : 8;        // I4/I8/IA4: 8 wide; IA8: 4 wide
            depthBlockHeight=format==0 ? 8 : 4;       // I4: 8x8 blocks
            intensity=true;
        } else {
            if((trigger&~0x010A7Bu)!=0 || (trigger&0x018000u)!=0x010000u || format==1 || format>0xC)
                throw std::runtime_error("Unsupported Prime raw color texture-copy mode");
            if((values[0x43]&7u)>2u)
                throw std::runtime_error("Prime color texture-copy adapter does not support single-channel EFB sources");
            // Channel copies besides R4 (0) and A8 (7), which keep the shapes below:
            // GX_CTF_RA4 (2), R8/G8/B8 (8/9/0xA) are 8 bpp in 8x4 blocks; GX_CTF_RA8 (3),
            // RG8/GB8 (0xB/0xC) are 16 bpp in 4x4 blocks. Aurora converts and RAM-encodes all.
            if(format==2 || format==3 || format>=8) {
                depthFormat=format|0x20u;
                depthBlockWidth=(format==3 || format>=0xB) ? 4 : 8;
                channel=true;
            }
        }
        // Depth and intensity copies carry their own block shape and host format.
        const bool explicitLayout=depth || intensity || channel;
        // Observed A8 edge copy: x=638,width=8. Retain its tile-sized output
        // and sample extent; the backend clamps only the out-of-EFB tail.
        const bool a8RightTail=!explicitLayout && format==7 && !(trigger&0x200u) && Left()<640 && Left()+Width()<=647;
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
        const uint32_t blockBytes=explicitLayout ? depthBlockBytes : format==6 ? 64 : 32;
        const uint32_t blockWidth=explicitLayout ? depthBlockWidth : (format==0 || format==7) ? 8 : 4;
        const uint32_t blockHeight=explicitLayout ? depthBlockHeight : format==0 ? 8 : 4;
        const uint32_t stride=((width+blockWidth-1u)/blockWidth)*blockBytes;
        if(((values[0x4D]&1023u)<<5)!=stride)
            throw std::runtime_error("Unsupported Prime texture-copy destination stride");
        // With BP bit15 clear the SDK uses channel-copy formats, not
        // intensity: nibble 0 is GX_CTF_R4 (0x20), 7 is GX_CTF_A8 (0x27).
        const uint32_t hostFormat=explicitLayout ? depthFormat : (format==0 || format==7) ? format|0x20u : format;
        return {hostFormat,stride*((height+blockHeight-1u)/blockHeight),width,height,halfScale};
    }
};
}

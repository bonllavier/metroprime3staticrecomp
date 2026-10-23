#include "hle/prime_raw_bp.h"
#include "../../aurora-main/lib/gfx/tex_copy_format_contract.hpp"
#include "../../aurora-main/lib/gx/xf_viewport.hpp"
#include <iostream>
#include <stdexcept>

static void Require(bool value) {
    if(!value) throw std::runtime_error("Prime raw BP contract failed");
}
template<class F> static void Reject(F function) {
    bool rejected=false;
    try {function();} catch(const std::runtime_error&) {rejected=true;}
    Require(rejected);
}
int main() {
    // SDK XF origin values for full 640x448 EFB and a 64x64 scratch viewport.
    // These must align with BP copy/scissor coordinates, without a +2 offset.
    Require(aurora::gx::xf_viewport_edge(662.0f,640.0f)==0.0f);
    Require(aurora::gx::xf_viewport_edge(566.0f,448.0f)==0.0f);
    Require(aurora::gx::xf_viewport_edge(758.0f,64.0f)==384.0f);
    Require(aurora::gx::xf_viewport_origin(0.0f,640.0f)==662.0f);
    Require(aurora::gx::xf_viewport_origin(384.0f,64.0f)==758.0f);
    PrimeRawBp::Registers bp;
    // Original GXData snapshot from the first in-game render-copy failure.
    bp.Write(0x43,0);
    bp.Write(0x49,0x00B000); // x=0, y=44
    bp.Write(0x4A,0x059E7F); // 640 x 360 source
    bp.Write(0x4D,0xA0);    // 5120 bytes per four destination rows
    bp.Write(0x52,0x010263);
    auto copy=bp.DecodeColorCopy();
    Require(bp.Left()==0 && bp.Top()==44 && bp.Width()==640 && bp.Height()==360);
    Require(copy.format==6 && copy.halfScale && copy.width==320 && copy.height==180);
    Require(copy.bytes==230400 && !bp.Clear());
    bp.Write(0x52,0x010A63);
    Require(bp.Clear() && bp.DecodeColorCopy().bytes==230400);

    // Prior full-resolution boot copies retain their layout and size.
    bp.Write(0x49,0);
    bp.Write(0x4A,0x06FE7F); // 640 x 448
    bp.Write(0x52,0x010043);
    copy=bp.DecodeColorCopy();
    Require(copy.format==4 && !copy.halfScale && copy.width==640 && copy.height==448);
    Require(copy.bytes==573440);
    bp.Write(0x52,0x010053);
    Require(bp.DecodeColorCopy().bytes==573440);
    bp.Write(0x52,0x010063);
    bp.Write(0x4D,0x140);
    Require(bp.DecodeColorCopy().bytes==1146880);

    // Tiny valid copies still reserve complete GX tiles.
    bp.Write(0x4A,0x401); // 2 x 2 -> 1 x 1
    bp.Write(0x4D,2);
    bp.Write(0x52,0x010263);
    Require(bp.DecodeColorCopy().bytes==64);
    bp.Write(0x4A,0x402); // Odd source sampling remains explicitly unsupported.
    Reject([&]{bp.DecodeColorCopy();});
    bp.Write(0x4A,0);
    Reject([&]{bp.DecodeColorCopy();});
    bp.Write(0x4A,0x059E7F);
    bp.Write(0x4D,0x140); // Source-width stride is wrong for a half copy.
    Reject([&]{bp.DecodeColorCopy();});
    bp.Write(0x4D,0xA0);
    for(auto trigger : {0x0102E3u,0x010663u,0x018263u}) {
        bp.Write(0x52,trigger);
        Reject([&]{bp.DecodeColorCopy();});
    }
    bp.Write(0x52,0x010263);
    bp.Write(0x43,3);
    Reject([&]{bp.DecodeColorCopy();});
    // Original later-cinematic snapshot: BP bit15 clear means R4, not I4.
    bp.Write(0x43,1);bp.Write(0x49,0);bp.Write(0x4A,0x3FCFF);
    bp.Write(0x4D,0x10);bp.Write(0x52,0x10A03);
    copy=bp.DecodeColorCopy();
    Require(copy.format==0x20 && copy.width==128 && copy.height==128);
    Require(copy.bytes==8192 && copy.halfScale && bp.Clear());
    bp.Write(0x4D,0x20);Reject([&]{bp.DecodeColorCopy();});
    bp.Write(0x4A,0x401);bp.Write(0x4D,1);
    copy=bp.DecodeColorCopy();Require(copy.width==1 && copy.height==1 && copy.bytes==32);
    // Next live snapshot: A8 channel copy, 72x58, 9 tiles per row.
    bp.Write(0x49,0x2B010);bp.Write(0x4A,0xE447);bp.Write(0x4D,9);bp.Write(0x52,0x10873);
    copy=bp.DecodeColorCopy();
    Require(copy.format==0x27 && copy.width==72 && copy.height==58);
    Require(copy.bytes==4320 && !copy.halfScale && bp.Left()==16 && bp.Top()==172);
    // Live hologram snapshot: retain 8x70 output even though its last six
    // source columns extend beyond EFB. Never stretch two valid pixels to eight.
    bp.Write(0x49,0x2627E);bp.Write(0x4A,0x11407);bp.Write(0x4D,1);bp.Write(0x52,0x10873);
    copy=bp.DecodeColorCopy();
    Require(bp.Left()==638 && bp.Top()==152 && copy.width==8 && copy.height==70 && copy.bytes==576);
    using aurora::gfx::tex_copy_conv::detail::preserve_a8_right_edge;
    Require(preserve_a8_right_edge(0x27,638,152,8,70,640,528));
    Require(preserve_a8_right_edge(0x27,1276,304,16,140,1280,1056));
    Require(!preserve_a8_right_edge(6,638,152,8,70,640,528));
    Require(!preserve_a8_right_edge(0x27,640,152,8,70,640,528));
    Require(!preserve_a8_right_edge(0x27,638,500,8,70,640,528));
    bp.Write(0x49,(152u<<10)|640u);Reject([&]{bp.DecodeColorCopy();});
    bp.Write(0x49,(152u<<10)|639u);bp.Write(0x4A,(69u<<10)|8u);Reject([&]{bp.DecodeColorCopy();});
    PrimeRawBp::Registers textureBp;
    Require(textureBp.IndexedTextureMask()==0);
    // Both BP banks, all three indexed formats, and non-indexed CMPR.
    textureBp.Write(0x88,8u<<20);textureBp.Write(0x8B,9u<<20);
    textureBp.Write(0xA8,10u<<20);textureBp.Write(0xAB,14u<<20);
    Require(textureBp.IndexedTextureMask()==0x19);
    textureBp.Write(0x88,6u<<20);textureBp.Write(0x8B,0);
    textureBp.Write(0xA8,5u<<20);Require(textureBp.IndexedTextureMask()==0);
    std::cout<<"PASS copy snapshots, unsupported-mode bounds and indexed-texture BP banks\n";
}

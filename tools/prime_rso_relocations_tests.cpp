#include "hle/prime_rso_relocations.h"
#include <cassert>
#include <iostream>

int main() {
    using namespace PrimeRsoRelocations;
    assert(SignedImmediate(0x38637FFF, false) == 0x7FFF);
    assert(SignedImmediate(0x38638000, false) == 0xFFFF8000);
    assert(SignedImmediate(0x3C609017, true) == 0x90170000);
    for (uint32_t address : {0x90007FFFu, 0x90008000u, 0x9016E844u, 0x9023CB80u, 0xFFFFF123u}) {
        const uint32_t high = static_cast<uint32_t>(HighAdjusted(address)) << 16u;
        assert(high + SignedImmediate(address, false) == address);
    }
    assert(BranchTarget(0x4BFFFF70, 0x9016E770) == 0x9016E6E0);
    assert(BranchTarget(0x480DA28D, 0x9015ECF4) == 0x90238F80);
    assert(BranchTarget(0x48000013, 0x9015ECF4) == 0x10);
    std::array<uint32_t, 6> stub{0x91810004, 0x3D808030, 0x398C5A94, 0x7D8903A6, 0x81810004, 0x4E800420};
    uint32_t target = 0;
    assert(DecodeSdkThunk(stub, target) && target == 0x80305A94);
    stub[2] = 0x398CFFF0;
    assert(DecodeSdkThunk(stub, target) && target == 0x802FFFF0);
    stub[4] ^= 4;
    assert(!DecodeSdkThunk(stub, target)); // Must preserve the exact R12 restore.
    for (uint32_t runtime : {0x9015E9A0u, 0x91278020u, 0x92048000u}) {
        for (uint32_t offset = 0; offset < 0xFE88; offset += 4) {
            uint32_t actual = 0, nominal = 0;
            assert(MapSection(0x806985E0 + offset, 0x806985E0, runtime, 0xFE88, false, actual));
            assert(actual == runtime + offset);
            assert(MapSection(actual, 0x806985E0, runtime, 0xFE88, true, nominal));
            assert(nominal == 0x806985E0 + offset);
        }
        uint32_t result = 0;
        assert(!MapSection(runtime + 0xFE88, 0x806985E0, runtime, 0xFE88, true, result));
        assert(!MapSection(0x806985DF, 0x806985E0, runtime, 0xFE88, false, result));
    }
    // BSS has its own allocation and cannot be derived from the text base.
    uint32_t bss = 0;
    assert(MapSection(0x806AA344, 0x806AA340, 0x93408000, 0x1904, false, bss) && bss == 0x93408004);
    std::cout << "PASS RSO LO/HA carry, signed branches, exact import thunk and three independent section placements\n";
}

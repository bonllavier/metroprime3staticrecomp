#include "hle/prime_fov.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

#define require(ok) do { if (!(ok)) { std::cerr << "FAIL line " << __LINE__ << ": " << #ok << "\n"; std::exit(1); } } while (false)

int main() {
    using namespace PrimeFov;

    // Range: PrimeHack's 170 cap, a sane floor, NaN back to the default.
    require(ClampFov(200.0f) == kMaxFov && ClampFov(10.0f) == kMinFov && ClampFov(90.0f) == 90.0f);
    require(ClampFov(std::nanf("")) == kDefaultFov);

    // Culling is bypassed strictly above 94 degrees, or whenever asked.
    require(!CullingBypassed(94.0f, false) && CullingBypassed(94.5f, false) && CullingBypassed(60.0f, true));

    // Only the three calls PrimeHack nops skip the game's FOV setter.
    require(HeldSetFovCaller(0x800DD880u) && HeldSetFovCaller(0x800DCEB4u) && HeldSetFovCaller(0x8017D410u));
    require(!HeldSetFovCaller(0x800DD87Cu) && !HeldSetFovCaller(0x8005430Cu) && !HeldSetFovCaller(0));

    // Arm: unchanged below 75 (z-near too), eased between 75 and 125, wide above.
    const Arm base;
    bool zNear = true;
    Arm arm = AutoArm(60.0f, base, zNear);
    require(!zNear && arm.left == base.left && arm.forward == base.forward && arm.up == base.up);
    arm = AutoArm(75.0f, base, zNear);
    require(zNear && arm.left == base.left && arm.forward == base.forward);
    arm = AutoArm(100.0f, base, zNear);
    require(zNear && std::fabs(arm.left - 0.235f) < 1e-6f && std::fabs(arm.forward - 0.14f) < 1e-6f && arm.up == base.up);
    arm = AutoArm(150.0f, base, zNear);
    require(zNear && arm.left == kWideArm.left && arm.forward == kWideArm.forward && arm.up == base.up);
    // Continuous at both ends of the easing.
    const Arm at125 = AutoArm(125.0f, base, zNear), past125 = AutoArm(125.01f, base, zNear);
    require(std::fabs(at125.left - past125.left) < 1e-4f && std::fabs(at125.forward - past125.forward) < 1e-4f);

    std::cout << "PASS Prime FOV: range, culling threshold, held setter callers, arm placement\n";
    return 0;
}

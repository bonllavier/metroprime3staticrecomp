#pragma once
// RM3E01 engine lights for Aurora's modern lighting, level 1 (recomp/docs/iluminacion_motor.md).
//
// 1. After 0x801ACAB8 (once per game frame: it fills the state manager's dynamic light lists), the
//    dynamic world lights (+0x5D8, rstl::vector<CLight>) and the static lights of every loaded area
//    (CWorld +0x30 count, +0x38 entries of 8 bytes with the CGameArea at +4; an area is loaded when
//    +0x98 == 0x11; its post-constructed data +0xB4 holds light set 0 at +0xA8) are sent to Aurora in
//    world space (aurora_set_engine_lights). CLight is 0x54 bytes: position +0x00, direction +0x0C,
//    RGBA8 colour +0x1C, type +0x20 (0 spot, 1 point, 2 directional, 3 local ambient, 4 custom), spot
//    cutoff +0x24, distance attenuation +0x28..+0x30, angle attenuation +0x34..+0x3C, cached range
//    +0x48 (recomputed when bit 0x20 of +0x50 is set, as 0x8037A580 does).
// 2. Around 0x8000C918 (CActorLights::ActivateLights) the area lights of characters and objects fade
//    in and out over about a sixth of a second instead of switching at once when the engine's choice
//    changes (BuildAreaLightList re-ranks them as the actor moves). Only the lights loaded for that
//    call are changed, and the CActorLights block is restored right after the call.
#include <cstdint>

namespace PrimeEngineLights {
// From F1 > Video (any thread): send the light lists to Aurora; fade the lights of characters.
void SetOptions(bool sendLights, bool actorFade);
}  // namespace PrimeEngineLights

// Runtime hook entry points (recomp/tools/prepare_prime_runtime_hooks.py).
extern "C" void PrimeEngineLightsAfterWorldLights(uint32_t self);
extern "C" uint32_t PrimeActorLightsBegin(uint32_t actorLights);
extern "C" void PrimeActorLightsEnd(uint32_t token);

#include "hle/prime_engine_lights.h"
#include "hle/prime_materials.h"
#include "post_effects.h"

#include "runtime_config.h"

#include <cstdlib>
#include <iterator>
#include <vector>

namespace PostEffects {

AuroraPostProcess FromConfig() {
    const auto& c = RuntimeConfigFile::Get();
    AuroraPostProcess p{};
    p.sharpen = c.effectsSharpen.value_or(false);
    p.sharpenAmount = c.effectsSharpenAmount.value_or(0.5f);
    p.fxaa = c.effectsFxaa.value_or(false);
    p.bloom = c.effectsBloom.value_or(false);
    p.bloomThreshold = c.effectsBloomThreshold.value_or(0.7f);
    p.bloomIntensity = c.effectsBloomIntensity.value_or(0.35f);
    p.bloomRadius = c.effectsBloomRadius.value_or(1.0f);
    p.bloomWorldOnly = c.effectsBloomWorldOnly.value_or(true);
    p.ssao = c.effectsSsao.value_or(true);  // on by default since 0.1.4 (tuned for contact shading)
    p.ssaoRadius = c.effectsSsaoRadius.value_or(1.0f);
    p.ssaoIntensity = c.effectsSsaoIntensity.value_or(0.70f);
    p.ssaoSoftness = c.effectsSsaoSoftness.value_or(2.0f);
    p.ssaoSamples = c.effectsSsaoSamples.value_or(8.0f);
    p.ssaoBias = c.effectsSsaoBias.value_or(1.2f);
    p.ssaoPower = c.effectsSsaoPower.value_or(0.99f);
    p.ssaoFadeDistance = c.effectsSsaoFade.value_or(0.0f);
    p.ssaoEdgeSharpness = c.effectsSsaoEdge.value_or(0.52f);
    p.ssaoGlowProtect = c.effectsSsaoGlow.value_or(1.0f);
    p.ssaoSkipMarked = c.effectsSsaoSkipFace.value_or(true);
    p.color = c.effectsColor.value_or(false);
    p.brightness = c.effectsBrightness.value_or(0.0f);
    p.contrast = c.effectsContrast.value_or(1.0f);
    p.saturation = c.effectsSaturation.value_or(1.0f);
    p.gamma = c.effectsGamma.value_or(1.0f);
    p.temperature = c.effectsTemperature.value_or(0.0f);
    p.vignette = c.effectsVignette.value_or(0.0f);
    // Floor reflections: experimental, on by default (user's decision, 31/10/2026).
    p.ssr = c.effectsSsr.value_or(true);
    p.ssrStrength = c.effectsSsrStrength.value_or(0.7f);
    p.ssrRoughness = c.effectsSsrRoughness.value_or(0.35f);
    p.ssrRelief = c.effectsSsrRelief.value_or(1.0f);
    p.specOcclusion = c.effectsSpecOcclusion.value_or(true);
    p.lightShafts = c.effectsLightShafts.value_or(false);
    p.lightShaftsIntensity = c.effectsLightShaftsIntensity.value_or(0.5f);
    // The game's glow without ghost copies at high resolution (2026-11-01): on by default.
    p.glowFix = c.effectsGlowFix.value_or(true);
    // Reflections on smooth walls: experimental, off by default (2026-11-01).
    p.wallSsr = c.effectsWallSsr.value_or(true);
    p.wallSsrStrength = c.effectsWallSsrStrength.value_or(0.8f);
    return p;
}

#if defined(MKW_GAME_PROFILE_RM3E01_00)
// Enhanced materials (RM3E01). Textures identified from F7 draw dumps of the opening space scene
// and the arrival at the G.F.S. Olympus (27/10/2026, 22:51 and 23:16-23:17): all are lit, per-pixel
// materials without a reflection map. Glass, holographic panels and light strips are left out.
void ApplyMaterials() {
    // Per-character and surface materials, with their F1 > Materials values (hle/prime_materials.h).
    PrimeMaterials::ApplyAll();
}
#endif

void Apply() {
    const AuroraPostProcess p = FromConfig();
    aurora_set_post_process(&p);
    {
        // World lighting levels 1 and 2 (experimental, on by default). The generic world material is
        // F1 > Materials > "World surfaces" (class 15, hle/prime_materials.cpp).
        const auto& c = RuntimeConfigFile::Get();
        aurora_set_environment_lighting(c.effectsEnvMaterial.value_or(true), c.effectsEnvLights.value_or(true));
        // Further improvements (31/10/2026): all on by default except linear blending, which changes
        // the look of every enhanced material.
        AuroraLightingFeatures f{};
        f.litRooms = c.effectsLitRooms.value_or(true);
        f.softHighlights = c.effectsSoftHighlights.value_or(true);
        f.linearBlend = c.effectsLinearLighting.value_or(false);
        f.smoothLightmaps = c.effectsSmoothLightmaps.value_or(true);
        f.areaReflections = c.effectsAreaReflections.value_or(false);  // off by default since 2026-11-01: little visible gain
        f.softShadows = c.effectsSoftShadows.value_or(true);
        aurora_set_lighting_features(&f);
        // Modern lighting, level 1 (F1 > Video, 08/11/2026; docs/iluminacion_motor.md): a master switch
        // and one per part. With the master switch off every part is off and the picture is as before.
        const bool modern = c.modernLighting.value_or(true);
        AuroraModernLighting m{};
#if defined(MKW_GAME_PROFILE_RM3E01_00)
        m.engineLights = modern && c.modernEngineLights.value_or(true);
        m.extraDynamicLights = modern && c.modernExtraDynamicLights.value_or(true);
#endif
        m.contactShadows = modern && c.modernContactShadows.value_or(true);
        m.contactStrength = c.modernContactStrength.value_or(0.6f);
        m.contactLength = c.modernContactLength.value_or(0.6f);
        m.temporal = modern && c.modernTemporal.value_or(true);
        m.lightmapDither = modern && c.modernLightmapDither.value_or(true);
        m.exposure = modern && c.modernAutoExposure.value_or(true);
        m.exposureRange = c.modernExposureRange.value_or(0.6f);
        m.exposureSpeed = c.modernExposureSpeed.value_or(1.5f);
        aurora_set_modern_lighting(&m);
#if defined(MKW_GAME_PROFILE_RM3E01_00)
        PrimeEngineLights::SetOptions(m.engineLights || m.extraDynamicLights,
                                      modern && c.modernActorLightFade.value_or(true));
#endif
    }
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    // No SSAO on Samus' body out of the suit and her face behind the visor glass (RM3E01). From F7
    // draw dumps: suit close-up 27/10/2026 21:38 (face through the visor) and 22:49 (Samus without
    // the suit). The helmet and suit keep their occlusion.
    static constexpr uint64_t kNoSsao[] = {
        0xaa70669d17e36772ull,  // face seen through the visor, 256x256
        0x0dc7a7f288795c1aull,  // eye seen through the visor, 128x128
        0x1420cb1e12575132ull,  // face without the helmet, 256x256
        0xfabed183c7759daaull,  // skin atlas (hands, ear, closed eyelid), 512x512
        0xa908b39735a0c171ull,  // eye without the helmet, 64x64
        0xf3e54ec244d7485dull,  // Zero Suit, 512x512
        0x18fe1fa63030e519ull,  // hair strands, 128x256
        0xd3f1dcf47b0ae1baull,  // hair strand alpha, 128x256
        0x314bb9bb1f511da7ull,  // hair atlas, 256x256
        0x939f8dd9bfe1fe85ull,  // hair, 256x256
        0xa6b9a48dfdaa5fc4ull,  // hair seen from behind, 128x128
        0x76e19f6204a21afbull,  // eyelashes, 128x64
        // PED suit (F7 31/10/2026 06:51): its helmet has its own face model. Its environment
        // reflection 0175f385 is drawn with the face and stays out (shared maps mark other objects).
        0xa1b6bddc9cda340bull,  // face seen through the PED visor, 256x128
    };
    // WII_COMPILED_SSAO_EXCLUDE_EXTRA=<hex>,<hex>: try more textures without rebuilding (diagnostics).
    std::vector<uint64_t> noSsao(std::begin(kNoSsao), std::end(kNoSsao));
    if (const char* extra = std::getenv("WII_COMPILED_SSAO_EXCLUDE_EXTRA")) {
        for (const char* at = extra; *at != 0;) {
            char* end = nullptr;
            const uint64_t hash = std::strtoull(at, &end, 16);
            if (end == at) { ++at; continue; }
            noSsao.push_back(hash);
            at = end;
        }
    }
    aurora_set_ssao_exclude_textures(noSsao.data(), noSsao.size());
    // Samus' visor glass (green visor atlas, also used by the suit's glowing strips): transparent, no
    // depth written, so the helmet's inner rim behind it showed its occlusion through the glass
    // (F7 28/10/2026 00:57). Earlier tries with textures drawn beside the face (four gradients and
    // the 9aca649d lining) did not reach that area. Confirmed with the magenta view.
    static constexpr uint64_t kSsaoCover[] = {
        0xa9ca4b803677683full,  // visor glass / inner visor atlas, 128x128
        // PED suit's visor glass (F7 31/10/2026 06:51): two 128x128 atlases drawn in pairs after the
        // opaque helmet without depth writes, alpha-blended and additive.
        0xce0cf067ed0ba1d7ull,  // PED visor glass, alpha-blended layer, 128x128
        0xc26b65383eafc212ull,  // PED visor glass, additive layer, 128x128
    };
    aurora_set_ssao_cover_textures(kSsaoCover, sizeof(kSsaoCover) / sizeof(kSsaoCover[0]));
    // Skin and eyes never get the enhanced-metal reflection (the game gives them the same
    // environment-reflection texgen as metal). Samus out of the suit: F7 dumps of 27/10/2026, 22:49.
    static constexpr uint64_t kSkin[] = {
        0xaa70669d17e36772ull,  // Samus' face seen through the visor, 256x256
        0x0dc7a7f288795c1aull,  // Samus' eye seen through the visor, 128x128
        0x1420cb1e12575132ull,  // Samus' face without the helmet, 256x256
        0xfabed183c7759daaull,  // Samus' skin atlas (hands, ear, closed eyelid), 512x512
        0xa908b39735a0c171ull,  // Samus' eye without the helmet, 64x64
        0xa1b6bddc9cda340bull,  // Samus' face seen through the PED visor, 256x128 (F7 31/10/2026 06:51)
        0x031fbb0783e1baaaull,  // Commander Dane's face, ears and neck, 512x256 (F7 28/10/2026 01:22)
        0x6629d875d7a329baull,  // Gandrayda's eye, 64x64 (F7 28/10/2026 01:21)
        0xac0d38f2f5056e20ull,  // G.F.S. Olympus operators: men's hood and face, 256x256 (F7 08/11/2026 22:06)
        0xd1c29b175aa1c1c2ull,  // G.F.S. Olympus operators: woman's hood, face and hair, 256x256 (same)
    };
    aurora_set_metal_exclude_textures(kSkin, sizeof(kSkin) / sizeof(kSkin[0]));
    ApplyMaterials();
#endif
}

} // namespace PostEffects

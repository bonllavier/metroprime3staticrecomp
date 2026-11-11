// Custom materials of RM3E01 (hle/prime_materials.h).
#if defined(MKW_GAME_PROFILE_RM3E01_00)
#include "hle/prime_materials.h"

#include "runtime_config.h"

#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace PrimeMaterials {
namespace {
// Values of an entry: the five base parameters, then AuroraMaterialExt in declaration order.
enum Field : int {
    kStrength, kRoughness, kMetallic, kEdge, kDetail,
    kBrightness, kSaturation, kSpecular, kReflection, kTintR, kTintG, kTintB,
    kDustAmount, kDustCells, kDustSize, kDustDensity, kDustLayer2, kDustDrift, kDustTwinkle, kDustGlow,
    kDustGlint, kDustSharpness, kDustHue, kDustHueSpread, kDustSaturation, kDustWhiteCore,
    kNebula, kNebulaScale, kNebulaSpeed, kDustOnGlow, kDustMinPixels,
    kCoat, kCoatRoughness, kPaintSplit, kBareMetallic, kBareRoughness,
    kFieldCount
};
static_assert(kFieldCount - kBrightness == sizeof(AuroraMaterialExt) / sizeof(float));

enum Group : uint32_t {
    kGroupSurface = 1u << 0, kGroupColour = 1u << 1, kGroupHighlights = 1u << 2,
    kGroupDustFlakes = 1u << 3, kGroupDustMotion = 1u << 4, kGroupDustLight = 1u << 5,
    kGroupDustColour = 1u << 6, kGroupNebula = 1u << 7, kGroupCoat = 1u << 8, kGroupPaint = 1u << 9,
    kGroupLast = kGroupPaint,
};
constexpr uint32_t kGroupsBasic = kGroupSurface | kGroupColour | kGroupHighlights;
constexpr uint32_t kGroupsStarDust = kGroupsBasic | kGroupDustFlakes | kGroupDustMotion | kGroupDustLight |
                                     kGroupDustColour | kGroupNebula;

struct ParamDef {
    Field field;
    Group group;
    const char* key;
    const char* label;
    float min, max;
    const char* format;
    const char* help;
};
const ParamDef kParams[] = {
    {kStrength, kGroupSurface, "strength", "Strength", 0.0f, 2.0f, "%.2f",
     "How much of the enhanced shading is mixed over the game's own."},
    {kRoughness, kGroupSurface, "roughness", "Roughness", 0.0f, 1.0f, "%.2f",
     "Low = polished, small sharp highlights. High = matte, wide soft highlights."},
    {kMetallic, kGroupSurface, "metallic", "Metallic", 0.0f, 1.0f, "%.2f",
     "How metal-like the surface is: coloured reflections and a darker painted base."},
    {kEdge, kGroupSurface, "edge", "Edge sheen", 0.0f, 1.0f, "%.2f",
     "Extra light at grazing angles (Fresnel), outlining the silhouette."},
    {kDetail, kGroupSurface, "detail", "Surface relief", 0.0f, 1.0f, "%.2f",
     "Relief taken from the texture so seams and panel lines catch the light."},
    {kBrightness, kGroupColour, "brightness", "Brightness", 0.0f, 2.0f, "%.2f", "Multiplies the shaded colour."},
    {kSaturation, kGroupColour, "saturation", "Saturation", 0.0f, 2.0f, "%.2f",
     "0 = grey, 1 = the game's colour, above 1 = more vivid."},
    {kTintR, kGroupColour, "tint", "Tint", 0.0f, 2.0f, "", "Colour filter over the material (white = none)."},
    {kSpecular, kGroupHighlights, "specular", "Highlights", 0.0f, 3.0f, "%.2f", "Strength of the light highlights."},
    {kReflection, kGroupHighlights, "reflection", "Reflections", 0.0f, 3.0f, "%.2f",
     "Strength of the surrounding reflection on metallic parts."},
    {kDustAmount, kGroupDustFlakes, "dust_amount", "Star dust", 0.0f, 2.0f, "%.2f", "Overall brightness of the flakes; 0 = off."},
    {kDustCells, kGroupDustFlakes, "dust_cells", "Grain", 32.0f, 1024.0f, "%.0f",
     "Flake cells per texture repeat: higher = finer, more numerous flakes."},
    {kDustSize, kGroupDustFlakes, "dust_size", "Flake size", 0.2f, 3.0f, "%.2f", "Radius of each flake inside its cell."},
    {kDustDensity, kGroupDustFlakes, "dust_density", "Density", 0.0f, 1.0f, "%.2f", "Share of cells that hold a flake."},
    {kDustLayer2, kGroupDustFlakes, "dust_layer2", "Second layer scale", 0.0f, 3.0f, "%.2f",
     "Grain of a second, deeper layer relative to the first (0 = one layer)."},
    {kDustMinPixels, kGroupDustFlakes, "dust_min_pixels", "Fade below (pixels)", 0.0f, 4.0f, "%.2f",
     "Flakes smaller than this on screen fade out, so distant views do not shimmer."},
    {kDustDrift, kGroupDustMotion, "dust_drift", "Drift speed", 0.0f, 6.0f, "%.2f",
     "How fast the flakes travel across the skin (cells per second; the layers move in different directions)."},
    {kDustTwinkle, kGroupDustMotion, "dust_twinkle", "Twinkle speed", 0.0f, 4.0f, "%.2f",
     "Twinkles per second; each flake keeps its own rhythm."},
    {kDustGlow, kGroupDustLight, "dust_glow", "Own glow", 0.0f, 1.5f, "%.2f",
     "Light the flakes give on their own, stronger on the lit side."},
    {kDustGlint, kGroupDustLight, "dust_glint", "Light glints", 0.0f, 3.0f, "%.2f",
     "Bright glints when a light meets a flake at the right angle."},
    {kDustSharpness, kGroupDustLight, "dust_sharpness", "Glint sharpness", 4.0f, 400.0f, "%.0f",
     "Low = broad glints that light up often. High = rare, pin-point glints."},
    {kDustOnGlow, kGroupDustLight, "dust_on_glow", "On glowing areas", 0.0f, 1.0f, "%.2f",
     "How much of the dust shows where the game makes the surface glow (her purple body)."},
    {kDustHue, kGroupDustColour, "dust_hue", "Hue", 0.0f, 1.0f, "%.2f", "Base colour of the flakes (0.75 violet, 0.83 magenta)."},
    {kDustHueSpread, kGroupDustColour, "dust_hue_spread", "Hue variation", 0.0f, 1.0f, "%.2f",
     "How far each flake's colour may stray from the base hue."},
    {kDustSaturation, kGroupDustColour, "dust_saturation", "Colour saturation", 0.0f, 1.0f, "%.2f",
     "0 = white flakes, 1 = fully coloured."},
    {kDustWhiteCore, kGroupDustColour, "dust_white_core", "White core", 0.0f, 1.0f, "%.2f",
     "How white the centre of each flake burns."},
    {kNebula, kGroupNebula, "nebula", "Nebula", 0.0f, 1.0f, "%.2f",
     "Slow clouds that tint the skin around the dust's hue; 0 = off."},
    {kNebulaScale, kGroupNebula, "nebula_scale", "Nebula scale", 1.0f, 24.0f, "%.1f", "Number of clouds per texture repeat."},
    {kNebulaSpeed, kGroupNebula, "nebula_speed", "Nebula speed", 0.0f, 1.0f, "%.3f", "How fast the clouds drift."},
    {kCoat, kGroupCoat, "coat", "Clear coat", 0.0f, 1.0f, "%.2f",
     "A smooth varnish over the surface: sharp, untinted highlights and a reflection that grows at grazing angles."},
    {kCoatRoughness, kGroupCoat, "coat_roughness", "Coat roughness", 0.0f, 1.0f, "%.2f",
     "Low = mirror-like varnish with pin-point highlights. High = satin."},
    {kPaintSplit, kGroupPaint, "paint_split", "Paint / metal split", 0.0f, 1.0f, "%.2f",
     "1 = coloured texels are paint (Surface values plus the clear coat) and grey texels bare metal (below). "
     "0 = the whole surface is paint."},
    {kBareMetallic, kGroupPaint, "bare_metallic", "Bare metal: metallic", 0.0f, 1.0f, "%.2f",
     "How metal-like the grey, unpainted parts are."},
    {kBareRoughness, kGroupPaint, "bare_roughness", "Bare metal: roughness", 0.0f, 1.0f, "%.2f",
     "Low = polished unpainted metal. High = brushed, matte."},
};

const char* GroupTitle(Group group) {
    switch (group) {
    case kGroupSurface: return "Surface";
    case kGroupColour: return "Colour";
    case kGroupHighlights: return "Highlights";
    case kGroupDustFlakes: return "Star dust: flakes";
    case kGroupDustMotion: return "Star dust: motion";
    case kGroupDustLight: return "Star dust: light";
    case kGroupDustColour: return "Star dust: colour";
    case kGroupNebula: return "Nebula";
    case kGroupCoat: return "Clear coat";
    case kGroupPaint: return "Paint and bare metal";
    }
    return "";
}

using Values = std::array<float, kFieldCount>;
constexpr Values Neutral(float strength, float roughness, float metallic, float edge, float detail) {
    Values v{};
    v[kStrength] = strength; v[kRoughness] = roughness; v[kMetallic] = metallic; v[kEdge] = edge; v[kDetail] = detail;
    v[kBrightness] = v[kSaturation] = v[kSpecular] = v[kReflection] = 1.0f;
    v[kTintR] = v[kTintG] = v[kTintB] = 1.0f;
    v[kDustCells] = 224.0f; v[kDustSize] = 1.0f; v[kDustDensity] = 0.45f; v[kDustLayer2] = 1.65f;
    v[kDustSharpness] = 130.0f; v[kDustHueSpread] = 0.2f; v[kDustSaturation] = 0.75f; v[kDustWhiteCore] = 0.5f;
    v[kNebulaScale] = 6.0f; v[kDustMinPixels] = 0.35f;
    return v;
}
constexpr Values GandraydaDefaults() {
    Values v = Neutral(0.18f, 0.55f, 0.05f, 0.30f, 0.30f);
    v[kDustAmount] = 1.0f; v[kDustDrift] = 0.45f; v[kDustTwinkle] = 0.35f; v[kDustGlow] = 0.6f; v[kDustGlint] = 1.0f;
    v[kDustHue] = 0.78f; v[kNebula] = 0.2f; v[kNebulaSpeed] = 0.06f;
    v[kDustOnGlow] = 1.0f;  // her glowing purple body is where most of the dust belongs
    v[kDustSharpness] = 4.0f;  // broad glints that light up often (user tuning, 31/10/2026)
    return v;
}

// Texture lists: F7 draw dumps (recomp/docs/mejoras_graficas.md).
constexpr uint64_t kShipHull[] = {
    0x4a0fa819c8244dbdull, 0x65a12bb61572c449ull, 0x7a55557e4de1e6d3ull, 0x88682054b808ff79ull,
    0xf5d0b906e25d5b07ull, 0x2da1a2ffd4bca0f9ull, 0x76b38298f6a2d666ull, 0xb895d087cb00bf1eull,
    0x87ccc7d95f235dfdull, 0xffeed3d5b5832aa5ull, 0x2d9359197847adeeull, 0x9748bedf52b0d2b4ull,
    0xa255954b6e920986ull, 0xb0e170bb9d17389full, 0x0063c25891997e43ull, 0x1b8cce9e5a68e86eull,
    0x21b89e20a8052e46ull, 0x2b526d55bb386791ull, 0x7f3b54f95fd485cdull, 0x9c56a8fce537cf23ull,
    0x9f35cb99b51d322bull, 0xb2d0b1ecf1a24a20ull, 0xbd7368ee97a8b617ull, 0xe498a235d864c085ull,
    0xe717a008946914e0ull,
};
// Samus' gunship outside (F7 30/10/2026 18:30 and 31/10/2026 18:07-18:08, space and hangar). Each
// paint atlas mixes yellow/red paint with dark grey bare metal: the paint/metal split separates them.
// Left out: the light map drawn with 12521a53 (17368ba5c432c6e7, emissive), the shared reflection map
// b2d14185d46dfbcd, and the Federation hull and hangar textures beside the ship.
constexpr uint64_t kGunshipPaint[] = {
    0x84c129a3cbb758cdull, 0x12521a534f726f17ull, 0x8e4cdae8ef6424c8ull, 0x50dfc739e8c8ea80ull,
};
constexpr uint64_t kGunshipCanopy[] = {0xc44b34a485dcf621ull, 0x6e26862675eeb96cull};  // green glass
constexpr uint64_t kGhor[] = {0x730ddbecf4cf6b33ull};
constexpr uint64_t kRundas[] = {0xa877691538c72be2ull};
// Opaque skin atlas, then the translucent purple body layer drawn over it (torso, arms, neck, crest;
// blended, so its dust shows weaker where it is more transparent). Eyes, lashes and the additive glow
// passes stay out.
constexpr uint64_t kGandrayda[] = {0x2037db7fad0b0396ull, 0x62d55c9b72987c67ull};
// Samus out of the suit (F7 27/10/2026 22:49 and 31/10/2026 17:15-17:16, opening on the gunship).
// The skin and eye textures are also on the skin list (post_effects.cpp, no metal); a class wins over
// it, and the skin list still keeps them neutral when enhanced materials are off.
constexpr uint64_t kZeroSuit[] = {0xf3e54ec244d7485dull};  // suit atlas, 512x512
constexpr uint64_t kSamusSkin[] = {
    0x1420cb1e12575132ull,  // face, 256x256
    0xfabed183c7759daaull,  // hands, ear, eyelid, mouth, 512x512
};
constexpr uint64_t kSamusEyes[] = {0xa908b39735a0c171ull};  // 64x64
constexpr uint64_t kSamusHair[] = {
    0x314bb9bb1f511da7ull,  // hair atlas and the red hair tie, 256x256
    0x939f8dd9bfe1fe85ull,  // hair, 256x256
    0x18fe1fa63030e519ull,  // loose strands (alpha-blended), 128x256
    0xa6b9a48dfdaa5fc4ull,  // hair seen from behind, 128x128
};
// G.F.S. Olympus operators (F7 08/11/2026 22:06-22:10: two standing men, a woman in green, men seated at
// the consoles). Every part of them is drawn with the game's environment-reflection texgen, so until now
// they got the generic enhanced metal (metallic 0.62) and their faces shone like the armour. The suit
// atlases also hold the hands; the head atlases hold the hood, the face and the woman's hair. Left out:
// the near-black light maps of the suit (7aea837c, 2a9bd4e3), the reflection masks (636802fd, c1232516),
// the 32x32 environment maps shared by suit and face, the eyes (1c9f40b6) and the teeth (59d8604c).
constexpr uint64_t kOperatorSuit[] = {
    0xc634e941368a5675ull,  // men's blue suit, 512x512
    0xd79d8ef7c3ad56fcull,  // woman's green suit and hands, 512x512
    0xd549b6602b2a5275ull,  // second layer of the woman's suit, 512x512
};
constexpr uint64_t kOperatorSkin[] = {
    0xac0d38f2f5056e20ull,  // men's hood, face and ear, 256x256
    0xd1c29b175aa1c1c2ull,  // woman's hood, face, eye, lips and hair, 256x256
};
// Dielectric materials (metallic near 0, so no environment reflection and no darker base): only the
// GX lights' GGX highlight changes, and with f0 = 0.04 it stays soft. Surface relief comes from the
// texture's luminance, so it stays low on skin (texture noise would read as pores).
constexpr Values ZeroSuitDefaults() {
    // Rubbery suit: a satin sheen that follows the body, tinted by the suit colour, with the panel seams
    // catching the light. User tuning of 31/10/2026: fully metallic reflectance and a stronger edge.
    Values v = Neutral(0.61f, 0.32f, 1.0f, 0.46f, 0.58f);
    v[kReflection] = 0.60f;
    return v;
}
constexpr Values SamusSkinDefaults() {
    // Skin: a faint sheen and nothing else (metallic 0: no reflection). User tuning of 31/10/2026:
    // roughness 0.29, a tighter highlight that still stays dim at f0 = 0.04.
    Values v = Neutral(0.70f, 0.29f, 0.0f, 0.0f, 0.15f);
    v[kSpecular] = 1.6f;
    return v;
}
constexpr Values SamusEyesDefaults() {
    // Wet eyes: a small, sharp catch-light when a light lines up.
    Values v = Neutral(0.40f, 0.15f, 0.0f, 0.15f, 0.0f);
    v[kSpecular] = 0.8f;
    return v;
}
constexpr Values GunshipDefaults() {
    // Painted hull: base paint slightly metallic under a glossy clear coat (sharp sun glints and a
    // reflection that grows towards the silhouette); the grey parts are bare, rougher metal.
    Values v = Neutral(0.55f, 0.30f, 0.20f, 0.35f, 0.70f);
    v[kCoat] = 0.85f; v[kCoatRoughness] = 0.07f;
    v[kPaintSplit] = 1.0f; v[kBareMetallic] = 0.85f; v[kBareRoughness] = 0.38f;
    return v;
}
constexpr Values GunshipCanopyDefaults() {
    // Glass: no metal, everything comes from the clear coat (mirror highlight, Fresnel reflection).
    Values v = Neutral(0.60f, 0.10f, 0.0f, 0.30f, 0.0f);
    v[kSpecular] = 1.2f;
    v[kCoat] = 1.0f; v[kCoatRoughness] = 0.04f;
    return v;
}
constexpr Values WorldDefaults() {
    // Every lightmapped wall and floor without a material of its own (F1 > Effects > World lighting).
    // Kept mild: rock, metal, glass and organic surfaces all share it. A light varnish gives the sheen
    // at grazing angles; slight metal for a faint reflection of the surroundings.
    Values v = Neutral(0.45f, 0.55f, 0.15f, 0.0f, 0.75f);  // relief 0.75: panels and seams catch the light
    v[kReflection] = 0.8f;
    v[kCoat] = 0.25f; v[kCoatRoughness] = 0.35f;
    return v;
}
constexpr Values OperatorSuitDefaults() {
    // Rubber: no metal (no coloured reflection), a broad soft highlight that follows the body, a little
    // sheen at grazing angles and a thin satin layer. Seams catch some light.
    Values v = Neutral(0.60f, 0.50f, 0.0f, 0.35f, 0.45f);
    v[kSpecular] = 1.1f;
    v[kReflection] = 0.3f;
    v[kCoat] = 0.25f; v[kCoatRoughness] = 0.40f;
    return v;
}
constexpr Values OperatorSkinDefaults() {
    // Human skin: matte, a wide faint highlight (f0 = 0.04) and almost no relief, so texture noise does
    // not read as pores or sweat. Dimmer than Samus' skin (roughness 0.29, highlights 1.6).
    Values v = Neutral(0.65f, 0.55f, 0.0f, 0.10f, 0.10f);
    v[kSpecular] = 0.7f;
    return v;
}
constexpr Values SamusHairDefaults() {
    // Blonde hair: a golden sheen along the strands (slight metallic tints the highlight), relief from
    // the strand texture. User tuning of 31/10/2026: roughness 0.14, a sharper sheen.
    Values v = Neutral(0.50f, 0.14f, 0.25f, 0.15f, 0.70f);
    v[kReflection] = 0.50f;
    return v;
}

struct Entry {
    const char* id;
    const char* name;
    const char* description;
    uint8_t cls;  // 0 = the generic enhanced metal
    const uint64_t* textures;
    size_t textureCount;
    uint32_t groups;
    Values defaults;
    Values values{};
};
std::array<Entry, 14> g_entries{{
    {"samus", "Samus and reflective metal",
     "The game's reflection-mapped surfaces: Samus' suits and most machinery. Same surface values as "
     "F1 > Video > Enhanced metal.",
     0, nullptr, 0, kGroupsBasic, Neutral(0.30f, 0.25f, 0.62f, 0.50f, 1.0f)},
    {"zero_suit", "Samus: Zero Suit", "Her blue suit out of the armour: rubbery, with a soft satin sheen.", 6,
     kZeroSuit, sizeof(kZeroSuit) / sizeof(kZeroSuit[0]), kGroupsBasic, ZeroSuitDefaults()},
    {"samus_skin", "Samus: skin", "Face, hands and ears out of the suit: matte, with a faint sheen only.", 7,
     kSamusSkin, sizeof(kSamusSkin) / sizeof(kSamusSkin[0]), kGroupsBasic, SamusSkinDefaults()},
    {"samus_eyes", "Samus: eyes", "A small wet catch-light.", 8, kSamusEyes,
     sizeof(kSamusEyes) / sizeof(kSamusEyes[0]), kGroupsBasic, SamusEyesDefaults()},
    {"samus_hair", "Samus: hair", "Blonde hair and its tie: a golden sheen along the strands.", 9, kSamusHair,
     sizeof(kSamusHair) / sizeof(kSamusHair[0]), kGroupsBasic, SamusHairDefaults()},
    {"ghor", "Ghor", "Bounty hunter: brushed machine exoskeleton.", 3, kGhor, 1, kGroupsBasic,
     Neutral(0.45f, 0.30f, 0.85f, 0.40f, 1.0f)},
    {"rundas", "Rundas", "Bounty hunter: dark, polished crystalline armour.", 4, kRundas, 1, kGroupsBasic,
     Neutral(0.40f, 0.12f, 0.25f, 0.80f, 0.40f)},
    {"gandrayda", "Gandrayda",
     "Bounty hunter: blue skin and pink crystals, with animated star dust over her skin and glowing body.", 5,
     kGandrayda, sizeof(kGandrayda) / sizeof(kGandrayda[0]), kGroupsStarDust, GandraydaDefaults()},
    {"hulls", "Federation ship hulls", "Grey plating and structure of the G.F.S. ships.", 1, kShipHull,
     sizeof(kShipHull) / sizeof(kShipHull[0]), kGroupsBasic, Neutral(0.45f, 0.40f, 0.70f, 0.30f, 1.0f)},
    {"gunship", "Samus' gunship", "Her ship's hull: yellow and red paint under a clear coat, and bare grey metal.", 2,
     kGunshipPaint, sizeof(kGunshipPaint) / sizeof(kGunshipPaint[0]), kGroupsBasic | kGroupCoat | kGroupPaint,
     GunshipDefaults()},
    {"world", "World surfaces (experimental)",
     "Walls and floors of every level with baked lighting and no material of their own. Needs F1 > Effects > "
     "Environment material.",
     15, nullptr, 0, kGroupsBasic | kGroupCoat | kGroupPaint, WorldDefaults()},
    {"gunship_canopy", "Samus' gunship: canopy", "The green cockpit glass.", 10, kGunshipCanopy,
     sizeof(kGunshipCanopy) / sizeof(kGunshipCanopy[0]), kGroupsBasic | kGroupCoat, GunshipCanopyDefaults()},
    {"operator_suit", "G.F.S. Olympus operators: suit",
     "The crew's blue and green suits (and the hands drawn with them): rubber with a soft satin sheen.", 11,
     kOperatorSuit, sizeof(kOperatorSuit) / sizeof(kOperatorSuit[0]), kGroupsBasic | kGroupCoat,
     OperatorSuitDefaults()},
    {"operator_skin", "G.F.S. Olympus operators: face",
     "Faces, hoods and hair of the crew: matte like human skin, with a faint highlight only.", 12, kOperatorSkin,
     sizeof(kOperatorSkin) / sizeof(kOperatorSkin[0]), kGroupsBasic, OperatorSkinDefaults()},
}};
bool g_loaded = false;

std::string Key(const Entry& entry, const char* param) { return std::string(entry.id) + "_" + param; }

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    const auto& config = RuntimeConfigFile::Get();
    for (auto& entry : g_entries) {
        entry.values = entry.defaults;
        for (const auto& param : kParams) {
            if (param.field == kTintR) {
                for (int c = 0; c < 3; ++c) {
                    const char* channel[] = {"tint_r", "tint_g", "tint_b"};
                    if (const auto it = config.primeMaterials.find(Key(entry, channel[c])); it != config.primeMaterials.end())
                        entry.values[kTintR + c] = std::clamp(it->second, 0.0f, 2.0f);
                }
                continue;
            }
            if (const auto it = config.primeMaterials.find(Key(entry, param.key)); it != config.primeMaterials.end())
                entry.values[param.field] = std::clamp(it->second, param.min, param.max);
        }
        if (entry.cls == 0) {  // Samus: the Video tab's values
            entry.values[kStrength] = RuntimeConfigFile::EnhancedMetalStrength(0.30f);
            entry.values[kRoughness] = config.enhancedMetalRoughness.value_or(0.25f);
            entry.values[kMetallic] = config.enhancedMetalMetallic.value_or(0.62f);
            entry.values[kEdge] = config.enhancedMetalEdge.value_or(0.50f);
            entry.values[kDetail] = config.enhancedMetalDetail.value_or(1.0f);
        }
    }
}

void Apply(const Entry& entry) {
    const Values& v = entry.values;
    if (entry.cls == 0) {
        aurora_set_enhanced_metal(RuntimeConfigFile::EnhancedMetal(true), v[kStrength]);
        aurora_set_enhanced_metal_params(v[kRoughness], v[kMetallic], v[kEdge], v[kDetail]);
    } else {
        aurora_set_material_class(entry.cls, v[kStrength], v[kRoughness], v[kMetallic], v[kEdge], v[kDetail]);
        aurora_set_material_textures(entry.cls, entry.textures, entry.textureCount);
    }
    AuroraMaterialExt ext{};
    std::memcpy(&ext, &v[kBrightness], sizeof(ext));
    if ((entry.groups & kGroupDustFlakes) == 0) ext.dustAmount = 0.0f;
    if ((entry.groups & kGroupNebula) == 0) ext.nebula = 0.0f;
    if ((entry.groups & kGroupCoat) == 0) ext.coat = 0.0f;
    if ((entry.groups & kGroupPaint) == 0) ext.paintSplit = 0.0f;
    aurora_set_material_ext(entry.cls, &ext);
}

void SaveValue(const Entry& entry, const char* param, float value) {
    RuntimeConfigFile::Mutable().primeMaterials[Key(entry, param)] = value;
    std::ostringstream text;
    text << value;
    RuntimeConfigFile::WriteSetting("prime_materials", Key(entry, param), text.str());
}

void Save(const Entry& entry, const ParamDef& param) {
    if (entry.cls == 0 && param.group == kGroupSurface) {
        // Samus' surface values are the Video tab's enhanced metal settings.
        switch (param.field) {
        case kStrength: RuntimeConfigFile::SetEnhancedMetalStrength(entry.values[kStrength]); break;
        case kRoughness: RuntimeConfigFile::SetVideoFloat(&RuntimeUserConfig::enhancedMetalRoughness, "enhanced_metal_roughness", entry.values[kRoughness]); break;
        case kMetallic: RuntimeConfigFile::SetVideoFloat(&RuntimeUserConfig::enhancedMetalMetallic, "enhanced_metal_metallic", entry.values[kMetallic]); break;
        case kEdge: RuntimeConfigFile::SetVideoFloat(&RuntimeUserConfig::enhancedMetalEdge, "enhanced_metal_edge", entry.values[kEdge]); break;
        case kDetail: RuntimeConfigFile::SetVideoFloat(&RuntimeUserConfig::enhancedMetalDetail, "enhanced_metal_detail", entry.values[kDetail]); break;
        default: break;
        }
        return;
    }
    if (param.field == kTintR) {
        SaveValue(entry, "tint_r", entry.values[kTintR]);
        SaveValue(entry, "tint_g", entry.values[kTintG]);
        SaveValue(entry, "tint_b", entry.values[kTintB]);
        return;
    }
    SaveValue(entry, param.key, entry.values[param.field]);
}

// Every value of the entry in one write of Config.toml (Reset to defaults).
void SaveAll(const Entry& entry) {
    std::vector<std::pair<std::string, std::string>> keyValues;
    auto add = [&](const char* param, float value) {
        RuntimeConfigFile::Mutable().primeMaterials[Key(entry, param)] = value;
        std::ostringstream text;
        text << value;
        keyValues.emplace_back(Key(entry, param), text.str());
    };
    for (const auto& param : kParams) {
        if ((entry.groups & param.group) == 0) continue;
        if (entry.cls == 0 && param.group == kGroupSurface) {
            Save(entry, param);  // Samus' surface values live in [video]
        } else if (param.field == kTintR) {
            add("tint_r", entry.values[kTintR]);
            add("tint_g", entry.values[kTintG]);
            add("tint_b", entry.values[kTintB]);
        } else {
            add(param.key, entry.values[param.field]);
        }
    }
    RuntimeConfigFile::WriteSettings("prime_materials", keyValues);
}

void DrawEntry(Entry& entry) {
    ImGui::PushID(entry.id);
    ImGui::TextWrapped("%s", entry.description);
    if (entry.textureCount) ImGui::TextDisabled("%zu texture%s identified from F7 dumps.", entry.textureCount, entry.textureCount == 1 ? "" : "s");
    bool changed = false;
    for (uint32_t group = 1; group <= kGroupLast; group <<= 1) {
        if ((entry.groups & group) == 0) continue;
        ImGui::SeparatorText(GroupTitle(static_cast<Group>(group)));
        for (const auto& param : kParams) {
            if (param.group != group) continue;
            ImGui::SetNextItemWidth(300.0f);
            if (param.field == kTintR) {
                if (ImGui::ColorEdit3(param.label, &entry.values[kTintR], ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)) {
                    changed = true;
                }
            } else if (ImGui::SliderFloat(param.label, &entry.values[param.field], param.min, param.max, param.format)) {
                changed = true;
            }
            // Write the file once a drag ends, not on every frame of it.
            if (ImGui::IsItemDeactivatedAfterEdit()) Save(entry, param);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", param.help);
        }
    }
    if (changed) Apply(entry);
    ImGui::Spacing();
    if (ImGui::Button("Reset to defaults")) {
        entry.values = entry.defaults;
        Apply(entry);
        SaveAll(entry);
    }
    ImGui::PopID();
}
}  // namespace

void ApplyAll() {
    Load();
    for (const auto& entry : g_entries) Apply(entry);
    aurora_set_enhanced_materials(RuntimeConfigFile::EnhancedMaterials(true));
}

void DrawTab() {
    Load();
    // Samus' surface values may have been changed in F1 > Video meanwhile.
    for (auto& entry : g_entries) {
        if (entry.cls != 0) continue;
        const auto& config = RuntimeConfigFile::Get();
        entry.values[kStrength] = RuntimeConfigFile::EnhancedMetalStrength(0.30f);
        entry.values[kRoughness] = config.enhancedMetalRoughness.value_or(0.25f);
        entry.values[kMetallic] = config.enhancedMetalMetallic.value_or(0.62f);
        entry.values[kEdge] = config.enhancedMetalEdge.value_or(0.50f);
        entry.values[kDetail] = config.enhancedMetalDetail.value_or(1.0f);
    }
    const bool materials = RuntimeConfigFile::EnhancedMaterials(true);
    const bool metal = RuntimeConfigFile::EnhancedMetal(true);
    ImGui::TextWrapped("Custom materials of the characters and surfaces tuned so far. Changes show at once; values are "
                       "saved to UserData/Config.toml [prime_materials].");
    if (!materials || !metal)
        ImGui::TextColored({1.0f, 0.65f, 0.35f, 1.0f}, "%s",
                           !metal ? "Enhanced metal is off (F1 > Video): none of these materials is drawn."
                                  : "Enhanced materials is off (F1 > Video): only Samus' entry applies.");
    for (auto& entry : g_entries) {
        if (ImGui::CollapsingHeader(entry.name)) DrawEntry(entry);
    }
}

}  // namespace PrimeMaterials
#endif

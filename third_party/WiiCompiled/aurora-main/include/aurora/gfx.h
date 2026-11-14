#ifndef AURORA_GFX_H
#define AURORA_GFX_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stddef.h"
#include "stdint.h"
#endif

#ifndef NDEBUG
#define AURORA_GFX_DEBUG_GROUPS
#endif

void aurora_push_debug_group(const char* label);
void aurora_pop_debug_group();

typedef struct {
  uint32_t queuedPipelines;
  uint32_t createdPipelines;
  uint32_t drawCallCount;
  uint32_t mergedDrawCallCount;
  uint32_t lastVertSize;
  uint32_t lastUniformSize;
  uint32_t lastIndexSize;
  uint32_t lastStorageSize;
  uint32_t lastTextureUploadSize;
  uint32_t presentedFrameCount;
  uint32_t interpolatedFrameCount;
} AuroraStats;

typedef struct {
  uint64_t totalPresentCount;
  uint32_t sampleCount;
  double framesPerSecond;
  double averageFrameTimeMs;
  double p95FrameTimeMs;
  double jitterMs;
  // framesPerSecond with duplicated presentation slots scaled out, so this is the rate of frames
  // that carried new motion. Equal to framesPerSecond when every slot replayed real interpolation.
  double effectiveFramesPerSecond;
} AuroraPresentTiming;

const AuroraStats* aurora_get_stats();
// Cumulative counters for timelines (callable from any thread).
uint64_t aurora_get_present_count();
// Native thread ids of the frame worker and presenter (Windows); returns how many were written.
uint32_t aurora_get_render_thread_ids(uint32_t* ids, uint32_t capacity);
uint64_t aurora_get_texture_upload_bytes_total();
void aurora_get_present_timing(AuroraPresentTiming* timing);
typedef struct {
  double windowMs;
  double estimatedCpuMs[5]; // draw, textures, static texture, bind groups, uniforms
  uint64_t calls[5];
  uint64_t samples[5];
} AuroraProducerTiming;
void aurora_get_producer_timing(AuroraProducerTiming* timing);
void aurora_set_producer_profiling(bool enabled);

// Interpolation health: the per-frame fields describe the last sealed frame, the counters
// accumulate since it was configured. This answers "output FPS dropped but the game held 60".
typedef struct {
  uint32_t targetFps;          // configured target, 0 when interpolation is off
  uint32_t targetSamples;      // slots the pacing controller currently aims for
  uint32_t activeSamples;      // slots latched for the latest sealed frame
  uint32_t candidates;         // perspective draws in the latest sealed frame
  uint32_t matchable;          // candidates whose identity also existed last frame
  uint32_t matches;            // draws matched to the previous frame
  uint32_t eligible;           // latest frame inserted interpolated slots
  uint32_t replaySafe;         // latest frame could replay its command stream
  uint64_t framesSealed;
  uint64_t framesLowMatch;
  uint64_t framesReplayUnsafe;
  uint64_t slotReductions;
  uint64_t lateSealDrops;
  uint64_t sealsWithBoundary;  // sealed batches that held a display copy
  uint64_t multiBoundary;      // of those, batches with two or more copies
  uint64_t sealsCarried;       // batches whose draws after the copy moved to the next batch
  uint64_t sealsComplete;      // batches that began at a game frame start
  uint64_t carriedDraws;       // draws moved to the next batch, summed
  uint64_t sealsReplayed;      // batches whose inserted images replayed interpolation
  uint64_t cameraCuts;         // presented frames left without inserted images: camera cut
  uint64_t borrowedDraws;      // unpaired draws moved with a matched draw of the same matrix
  uint64_t probeRestores;      // rate probes undone: the game stayed late without inserted images
  uint64_t cameraEstimates;    // game frames whose camera motion was found by vote
} AuroraFrameInterpolationDiagnostics;

void aurora_get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics* diagnostics);

// Generates transform-interpolated perspective frames between consecutive 60 Hz logical frames.
// Targets are 0 (off), any rate above MIN up to MAX images per second, or UNCAPPED, which climbs
// as far as the machine sustains. Guest simulation and VI timing are unchanged.
#define AURORA_FRAME_INTERPOLATION_MIN_FPS 60u
#define AURORA_FRAME_INTERPOLATION_MAX_FPS 480u
#define AURORA_FRAME_INTERPOLATION_UNCAPPED 0xFFFFFFFFu
void aurora_set_frame_interpolation_fps(uint32_t targetFps);
uint32_t aurora_get_frame_interpolation_fps();
// Low latency: each real frame is shown as soon as it is ready and the inserted images predict past it
// (extrapolated from its last motion) instead of leading up to it. Saves about half a frame of delay;
// motion that stops or turns overshoots for one inserted image.
void aurora_set_frame_interpolation_low_latency(bool enabled);
bool aurora_get_frame_interpolation_low_latency();
// Display sync: each image is presented a fixed fraction into its own refresh of the monitor, measured
// from its vertical blanks, so presents that drift against the refresh no longer show one image twice and
// lose the next. Only with interpolation on and a regular refresh (it stops itself under variable refresh).
void aurora_set_frame_interpolation_display_sync(bool enabled);
bool aurora_get_frame_interpolation_display_sync();
// The refresh rate presents are aligned to, 0 while they are not.
float aurora_get_display_sync_rate();
// True while many images miss their refresh and are presented as they come (alignment returns by itself).
bool aurora_get_display_sync_paused();

// Newly encountered GX pipelines compile on the bounded worker queue. Draws whose pipeline is not
// ready are skipped rather than stalling submission, and pick it up once compilation finishes.
void aurora_set_skip_unready_pipelines(bool enabled);
// Hybrid ubershader: a draw whose specialised pipeline is still compiling uses the ubershader instead of waiting.
void aurora_set_ubershader(bool enabled);
bool aurora_get_ubershader(void);
// Total draws encoded with an ubershader since start (the F1 overlay shows its rate).
uint64_t aurora_get_ubershader_draws(void);
bool aurora_get_skip_unready_pipelines();
uint32_t aurora_get_queued_pipeline_count();

// Controls whether display copies bypass the Wii's vertical copy filter.
void aurora_set_disable_copy_filter(bool disabled);
bool aurora_get_disable_copy_filter();

// Host image effects applied to the presented frame (all live). See lib/gfx/post_process.hpp.
typedef struct AuroraPostProcess {
  bool sharpen; float sharpenAmount;
  bool fxaa;
  bool bloom; float bloomThreshold; float bloomIntensity; float bloomRadius;
  bool ssao; float ssaoRadius; float ssaoIntensity;
  bool color; float brightness; float contrast; float saturation; float gamma; float temperature; float vignette;
  float ssaoSoftness;  // 1..4: blur step of the occlusion in pixels
  float ssaoSamples;        // 8, 12, 16 or 24
  float ssaoBias;           // percent of the distance below which a sample does not occlude
  float ssaoPower;          // contrast curve applied to the occlusion
  float ssaoFadeDistance;   // occlusion fades out up to this distance (0 = never)
  float ssaoEdgeSharpness;  // 0..1: how strictly the blur keeps object edges
  bool bloomWorldOnly;       // glow sourced from the 3D scene before the HUD is drawn
  float ssaoGlowProtect;     // 0..1: no occlusion on glowing surfaces (visor glass, lights)
  bool ssaoSkipMarked;       // no occlusion where a texture of the exclusion list is visible
  bool ssr;                  // experimental: screen-space reflections on floors
  float ssrStrength;         // 0..1
  float ssrRoughness;        // 0..1: reflection blur with distance
  float ssrRelief;           // 0..2: texture relief bending the reflection
  bool specOcclusion;        // ambient occlusion also dims the floor reflections in creases
  bool lightShafts;          // screen-space light shafts from the scene's lights
  float lightShaftsIntensity;  // 0..2
  bool glowFix;              // the game's glow blurred at its native footprint (no ghost copies)
  bool wallSsr;              // experimental: screen-space reflections on smooth walls too
  float wallSsrStrength;     // 0..1
  bool ssaoUnderEffects;     // fog, explosions and particles drawn over the world do not get occlusion
} AuroraPostProcess;
void aurora_set_post_process(const AuroraPostProcess* settings);
void aurora_get_post_process(AuroraPostProcess* settings);
// Textures (content hashes, as in texture replacement file names) whose surfaces never receive SSAO.
void aurora_set_ssao_exclude_textures(const uint64_t* hashes, size_t count);
// Transparent textures (no depth write, e.g. visor glass): no SSAO wherever they change the colour.
void aurora_set_ssao_cover_textures(const uint64_t* hashes, size_t count);
// Textures that never get the enhanced-metal reflection (skin, eyes).
void aurora_set_metal_exclude_textures(const uint64_t* hashes, size_t count);
// Enhanced materials: per-class PBR parameters (each 0..1, strength 0..2) and the textures of each
// class (1..15). Listed textures get the metal shading even without a reflection map; needs
// per-pixel lighting. Off by default.
void aurora_set_enhanced_materials(bool enabled);
// World surfaces with baked lightmaps (experimental). material: the environment material (the
// enhanced-material block lit by the lightmap; unclassified surfaces use class 15, set it with
// aurora_set_material_class / aurora_set_material_ext). borrowedLights: highlights from the lights
// the game loaded for its lit draws in the previous frame. Each works on its own.
void aurora_set_environment_lighting(bool material, bool borrowedLights);
// Further lighting improvements, each on its own (see gx.hpp): lit rooms, soft highlights, linear
// blending, smoothed lightmaps, area-coloured reflections, soft projected shadows.
typedef struct AuroraLightingFeatures {
  bool litRooms;
  bool softHighlights;
  bool linearBlend;
  bool smoothLightmaps;
  bool areaReflections;
  bool softShadows;
} AuroraLightingFeatures;
void aurora_set_lighting_features(const AuroraLightingFeatures* features);
// Modern lighting, level 1 (F1 > Video; docs: recomp/docs/iluminacion_motor.md). New entry points only:
// the structures above keep their layout.
// A light of the game's engine in world space (RM3E01 CLight, 0x54 bytes): the runtime sends the
// dynamic world lights and the static lights of every loaded area once per game frame.
typedef struct AuroraEngineLight {
  float pos[3];
  float dir[3];
  uint8_t color[4];   // RGBA8 as the game loads it into GX
  uint32_t type;      // 0 spot, 1 point, 2 directional, 3 local ambient, 4 custom
  float spotCutoff;   // degrees (spot)
  float distAtt[3];   // k0, k1, k2
  float angleAtt[3];  // a0, a1, a2 (custom)
  float radius;       // world units where the game stops considering the light; <= 0 = unbounded
  uint32_t flags;     // AURORA_ENGINE_LIGHT_*
} AuroraEngineLight;
#define AURORA_ENGINE_LIGHT_DYNAMIC 1u  // not baked into the lightmaps (shots, explosions, scripted lights)
void aurora_set_engine_lights(const AuroraEngineLight* lights, uint32_t count);
typedef struct AuroraModernLighting {
  bool engineLights;        // the engine's real lights for world highlights, light shafts and contact shadows
  bool extraDynamicLights;  // lit world pieces with 4 lights get the dynamic lights the engine had to leave out
  bool contactShadows;      // screen-space shadows towards the scene's lights
  float contactStrength;    // 0..1
  float contactLength;      // metres a shadow ray travels (0.1..2)
  bool temporal;            // temporal stabilisation of the contact shadows (off while frames are interpolated)
  bool lightmapDither;      // dither the baked lightmaps (no banding in dark gradients)
  bool exposure;            // automatic exposure (eye adaptation) and a highlight-preserving curve
  float exposureRange;      // largest correction in EV (0..2)
  float exposureSpeed;      // seconds to adapt about two thirds of the way
} AuroraModernLighting;
// Modern lighting, level 2 (13/11/2026; docs: recomp/docs/iluminacion_motor.md). Everything comes from
// what the game already draws: its own textures, material stages and frame. A new entry point; the
// structures above keep their layout.
typedef struct AuroraModernLighting2 {
  bool textureRelief;    // the materials' relief comes from the surface texture, not from the lit colour
  bool defaultMaterial;  // lit objects without a reflection map or a class get a soft default material (class 14)
  bool materialMasks;    // the game's reflection masks and emissive maps steer the material per pixel
  bool bounceLight;      // screen-space bounce light: what the frame shows lights what is next to it
  float bounceStrength;  // 0..2
  float bounceEmissive;  // 0..2 light from the surfaces the game marks as glowing
  float bounceSurfaces;  // 0..1 light bounced by ordinary lit surfaces
  float bounceRadius;    // metres (0.5..8)
  float bounceSamples;   // 4..16 per pixel at half resolution
  bool actorLights;      // characters and objects get every engine light around them (free GX slots)
  float worldRelight;    // 0..2 diffuse light from the engine's lights on lightmapped walls and floors (0 = off)
  bool hdrBloom;         // bloom over six scales from the world picture, glow mask as HDR brightness
  float bloomStrength;   // 0..2
  float bloomThreshold;  // linear luminance where it starts
  bool fog;              // light scattered in the air around the engine's lights
  float fogDensity;      // 0..2
} AuroraModernLighting2;
void aurora_set_modern_lighting2(const AuroraModernLighting2* settings);
void aurora_set_modern_lighting(const AuroraModernLighting* settings);
void aurora_set_material_class(uint8_t cls, float strength, float roughness, float metallic, float edge, float detail);
// Extended parameters of a material class, read by the enhanced-metal shader of its draws. Slot 0 is
// the generic enhanced metal (every surface with the game's reflection map, Samus' suits included).
// Neutral values: brightness, saturation, specular, reflection and tint 1; star dust amount 0.
typedef struct AuroraMaterialExt {
  // Colour and highlights.
  float brightness, saturation, specular, reflection;
  float tintR, tintG, tintB;
  // "Star dust": two layers of tiny flakes fixed to the texture coordinates that drift, twinkle,
  // glow faintly on their own and glint in the lights, over a slow nebula that tints the shading.
  float dustAmount;     // 0 = off
  float dustCells;      // flake cells per texture unit (layer 1)
  float dustSize;       // flake radius multiplier
  float dustDensity;    // fraction of cells holding a flake, 0..1
  float dustLayer2;     // layer 2 cell scale relative to layer 1 (0 = one layer)
  float dustDrift;      // drift speed, cells per second
  float dustTwinkle;    // twinkles per second
  float dustGlow;       // the flakes' own glow
  float dustGlint;      // light glints
  float dustSharpness;  // glint exponent
  float dustHue, dustHueSpread, dustSaturation, dustWhiteCore;
  float nebula, nebulaScale, nebulaSpeed;
  float dustOnGlow;     // 0 = never on glowing surfaces (EFB alpha >= 0.6), 1 = everywhere
  float dustMinPixels;  // flakes fade out below this diameter in pixels
  // Clear coat: a smooth dielectric layer over the base (aerospace/car paint, canopy glass). It adds
  // an untinted GGX highlight and a Fresnel environment reflection from the smooth geometric normal,
  // and dims the base where it reflects.
  float coat;           // 0 = off, 1 = full
  float coatRoughness;  // 0..1
  // Paint and bare metal in one texture: texels with saturated colour are "paint" (the class's
  // roughness and metallic, plus the clear coat); grey texels are bare metal with these values.
  float paintSplit;     // 0 = whole surface is paint, 1 = split by saturation
  float bareMetallic, bareRoughness;
} AuroraMaterialExt;
void aurora_set_material_ext(uint8_t cls, const AuroraMaterialExt* params);
void aurora_set_material_textures(uint8_t cls, const uint64_t* hashes, size_t count);
// Empty when every enabled effect runs; otherwise why one is skipped (e.g. SSAO with MSAA).
const char* aurora_post_process_status_note();
// True while the sealed frames contain the 3D world camera (false in menus, map, videos).
bool aurora_post_process_scene_active();

// Guest-RAM write tracking. `generation` changes whenever guest RAM covering a host range was
// written (or returns AURORA_GUEST_WRITE_UNTRACKED); `notify` reports writes aurora made itself.
#define AURORA_GUEST_WRITE_UNTRACKED UINT64_MAX
typedef uint64_t (*AuroraGuestWriteGenerationCallback)(const void* hostPtr, size_t size);
typedef void (*AuroraGuestWriteNotifyCallback)(const void* hostPtr, size_t size);
void aurora_set_guest_write_hooks(AuroraGuestWriteGenerationCallback generation,
                                  AuroraGuestWriteNotifyCallback notify);

#ifdef __cplusplus
}
#endif

#endif

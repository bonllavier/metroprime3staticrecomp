#pragma once

#include <cstdlib>

#include "../webgpu/gpu.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

// Host-side image effects applied to the presented frame (the "snapshot" image
// at output resolution, after the game's own image is composed and before the
// ImGui overlay). Every effect is a full-screen pass over that image; none of
// them touches game state, textures or pipelines. All options are live.
namespace aurora::gfx {
struct TextureRef;
}

namespace aurora::gfx::post {

struct Config {
  bool sharpen = false;      // AMD FidelityFX CAS style contrast-adaptive sharpening
  float sharpenAmount = 0.5f;  // 0..1
  bool fxaa = false;         // post-process antialiasing (FXAA 3.11 quality preset)
  bool bloom = false;        // extra glow on bright areas (the game keeps its own bloom)
  float bloomThreshold = 0.70f;  // 0..1 luminance where glow starts
  float bloomIntensity = 0.35f;  // 0..2
  float bloomRadius = 1.0f;      // 0.5..3 blur spread
  bool ssao = false;         // screen-space ambient occlusion from the EFB depth (MSAA off only)
  float ssaoRadius = 1.0f;   // 0.25..3 world-ish units (scaled by linear depth)
  float ssaoIntensity = 0.70f;  // 0..2
  float ssaoSoftness = 2.0f;   // 1..4 blur step (pixels)
  float ssaoSamples = 8.0f;
  float ssaoBias = 1.2f;       // percent
  float ssaoPower = 0.99f;
  float ssaoFadeDistance = 0.0f;
  float ssaoEdgeSharpness = 0.52f;
  float ssaoGlowProtect = 1.0f;  // 0..1: no occlusion on surfaces the game marks as glowing (EFB alpha)
  bool ssaoSkipMarked = true;    // no occlusion on pixels drawn with a listed texture (Samus' face)
  // Screen-space reflections on floors (experimental): world pixels facing up reflect what the
  // frame shows, weighted by Fresnel and by how smooth the floor texture looks.
  bool ssr = false;
  float ssrStrength = 0.5f;      // 0..1
  float ssrRoughness = 0.35f;    // 0..1: blur of the reflection with distance (worn, brushed metal)
  float ssrRelief = 1.0f;        // 0..2: the floor texture's relief bends the reflection
  bool specOcclusion = false;    // ambient occlusion also dims the floor reflections in creases
  bool lightShafts = false;      // screen-space light shafts from the scene's lights (GPU Gems 3, ch. 13)
  float lightShaftsIntensity = 0.5f;  // 0..2
  bool glowFix = true;           // soften the game's glow source copy to its native footprint
  bool wallSsr = false;          // experimental: the floor reflections also on smooth walls
  float wallSsrStrength = 0.35f; // 0..1
  bool bloomWorldOnly = true;
  bool color = false;        // colour grading
  float brightness = 0.0f;   // -0.5..0.5
  float contrast = 1.0f;     // 0.5..1.5
  float saturation = 1.0f;   // 0..2
  float gamma = 1.0f;        // 0.5..2
  float temperature = 0.0f;  // -1 (cool) .. 1 (warm)
  float vignette = 0.0f;     // 0..1
};

void set_config(const Config& config);
Config config();
bool any_enabled();

// Modern lighting, level 1 (aurora_set_modern_lighting; docs/iluminacion_motor.md). Kept apart from
// Config, which set_config replaces whole. Both run inside the frame with the SSAO application: they
// change the world snapshot that application writes back, so its own passes stay as they were.
struct Modern {
  bool contact = false;          // screen-space shadows towards the frame's strongest lights
  float contactStrength = 0.6f;  // 0..1
  float contactLength = 0.6f;    // metres
  bool temporal = false;         // reproject and blend the contact shadows (native frames, no interpolation)
  bool exposure = false;         // automatic exposure with a highlight-preserving curve
  float exposureRange = 0.8f;    // EV
  float exposureSpeed = 1.5f;    // seconds
};
void set_modern(const Modern& modern);
Modern modern();
// The frame's strongest engine lights (GX view space; a directional light is a point far against its
// direction), their range in world units, luminance and directional flag. count 0 = none this frame.
void note_engine_lights(uint32_t count, const float (*positions)[3], const float* radius, const float* luma,
                        const uint8_t* directional);
// The world's view matrix of the frame (3x4, rows), noted at its first static lightmapped draw.
void note_world_view(const float* rows);

// First perspective GX projection of each frame: xf[4], xf[5] are its Z scale/offset;
// with Aurora's reversed Z the view distance is xf5 / (xf4 - z).
void note_projection(const float* xf6, bool perspective);
// EFB rectangle (render pixels) that the display copy presents; set by copy_disp.
void note_display_source(float x, float y, float width, float height);
// Called when a frame is sealed (producer excluded): freezes that frame's projection for SSAO.
void latch_frame();
// Called for every GX draw: counts draws per camera so the world camera wins.
void note_draw();
// World "up" in view space (GX axes) from the static world geometry of the frame (lightmapped
// draws); the SSR uses it to find floors. Latched with the camera.
void note_world_up(float x, float y, float z);
// Light shafts: up to four lights of the frame (GX view space position, luminance), from the lights
// the game loaded for its lit draws; projected with the world camera when the frame is sealed.
void note_lights(uint32_t count, const float (*positions)[3], const float* luminance);
bool light_shafts_wanted();
// Area colours for the material reflections: the frame's average colour above and below the screen
// centre, read back from the GPU a few frames late and smoothed. False until measured.
void set_area_probe(bool enabled);
bool area_colours(float up[3], float down[3]);
// Called after each queue submission (maps the area probe's readback buffers).
void after_submit();
// True while recent sealed frames contain the world camera.
bool scene_active();
// Sealed world camera {xf[4], xf[5], viewport znear, zfar}; false without a 3D scene.
bool world_camera(std::array<float, 4>& camera);
// Current GX viewport (logical EFB pixels and depth range); pairs with the projection.
void note_viewport(float left, float top, float width, float height, float znear, float zfar, float efbWidth);

// Runs the enabled passes in place on `image`. `viewport*` is the sub-rectangle
// (pixels) that holds the game picture inside the image (letterboxing).
void apply(const wgpu::CommandEncoder& encoder, const webgpu::TextureWithSampler& image,
           float viewportLeft, float viewportTop, float viewportWidth, float viewportHeight);

// Human-readable note for the UI (e.g. why SSAO is unavailable); empty when fine.
const char* status_note();

// The game clears the EFB depth several times per frame (effect copies, HUD, display copy),
// so SSAO accumulates it: begin_depth() resets the accumulation at the start of the frame and
// merge_depth() folds the current EFB depth in (keeping the nearest value) right before every
// clear and at the end of the frame. The result is stable whatever the pass layout is.
bool wants_depth();
// Snapshot of the colour buffer right after the 3D world pass (before visor/HUD); bloom source.
bool wants_scene_color();
void capture_scene_color(const wgpu::CommandEncoder& encoder);
// SSAO applied inside the frame to the EFB colour at the end of each block of world-camera passes,
// before the visor / HUD passes. Uses the scene snapshot taken just before as its colour source.
// Returns true when it ran; reset_depth() then clears the accumulation for the next block.
// `firstInGameFrame`: light shafts and area colours (whole-picture effects) run only on the first
// application after the EFB was cleared; later blocks of the same frame get SSAO / reflections only.
// `allowProbe`: the area colour readback, taken once per presented frame (the native render).
bool apply_ssao_in_frame(const wgpu::CommandEncoder& encoder, bool firstInGameFrame = true, bool allowProbe = true);
// Why the last apply_ssao_in_frame returned false (diagnostics; "" after a successful one).
const char* last_apply_skip();
// WII_COMPILED_POST_DEBUG_DEPTH (depth inset and post-process logs): test builds only.
inline bool debug_depth_requested() {
#if defined(MKW_PRIME_QA_TOOLS)
  static const bool value = std::getenv("WII_COMPILED_POST_DEBUG_DEPTH") != nullptr;
  return value;
#else
  return false;
#endif
}
void reset_depth(const wgpu::CommandEncoder& encoder);
// True when the batch ends inside a world block: the next begin_depth keeps the accumulation.
// `draws` = world-camera draws of that open block so far.
void set_depth_carry(bool carry, uint32_t draws);
uint32_t carried_world_draws();
void begin_depth(const wgpu::CommandEncoder& encoder, const webgpu::TextureWithSampler& depth);
// `depth` may be the EFB depth or an offscreen target's; sizes are scaled to the accumulation.
void merge_depth(const wgpu::CommandEncoder& encoder, const wgpu::Texture& depth, const wgpu::TextureView& depthView,
                 const wgpu::Extent3D& depthSize);
// The game's glow (bloom) starts from a half-size copy of the whole EFB that it blurs by drawing it
// several times at offsets measured for the Wii's 640-pixel picture. At a higher resolution that copy
// is sharper than the offsets, so thin lights come out as separate ghost copies beside them (Dolphin's
// "bloom offset at higher IR"). This blurs the copy over the texels one Wii texel covers (`scaleX` x
// `scaleY` = copy size / the game's size), so the offset draws blend as on the console while the
// rest of the picture keeps its resolution. Only render-format copies; off with glow_ghost_fix.
void soften_copy(const wgpu::CommandEncoder& encoder, const TextureRef& target, float scaleX, float scaleY);
// A face run left exclusion depth that only the next merge_depth turns into the mask.
bool face_depth_pending();

// Textures whose surfaces never receive SSAO (content hashes, as in texture replacement names).
// Draws that sample one are flagged when recorded (gx::DrawData::ssaoExclude); render_impl
// brackets each run of flagged draws with face_before / face_after, and merge_depth turns the
// result into a per-pixel mask of where those surfaces are still visible. See docs/ssao_aurora.md.
void set_ssao_exclude_textures(const uint64_t* hashes, size_t count);
// Transparent textures (no depth write): the mask takes every pixel whose colour they change.
void set_ssao_cover_textures(const uint64_t* hashes, size_t count);
bool ssao_exclude_active();             // SSAO and the option are on and the list is not empty
// `cover`: the run is of transparent draws; it is compared by colour instead of depth.
void face_before(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& depthView, const wgpu::Extent3D& size,
                 bool cover = false);
void face_after(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& depthView, const wgpu::Extent3D& size,
                bool cover = false);

void shutdown();

} // namespace aurora::gfx::post

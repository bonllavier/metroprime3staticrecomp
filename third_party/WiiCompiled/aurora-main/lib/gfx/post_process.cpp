#include "post_process.hpp"

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "common.hpp"
#include "texture_replacement.hpp"
#include "texture.hpp"
#include "gpu_timing.hpp"
#include "../gx/frame_interpolation.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::gfx::post {
namespace {
Module Log("aurora::gfx::post");

std::mutex g_configMutex;
Config g_config;
const char* g_statusNote = "";
std::atomic<float> g_displaySrc[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // EFB rect presented (render px)

// ------------------------------------------------------------------------------------------
// Shaders. Every pass draws a fullscreen triangle; Params is one uniform block of 6 vec4.
// ------------------------------------------------------------------------------------------
constexpr std::string_view kPrelude = R"(
struct Params { a: vec4<f32>, b: vec4<f32>, c: vec4<f32>, d: vec4<f32>, e: vec4<f32>, f: vec4<f32> };
@group(0) @binding(0) var samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;
@group(0) @binding(2) var<uniform> P: Params;
struct VSOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32> };
var<private> kPos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(vec2(-1.0, 1.0), vec2(-1.0, -3.0), vec2(3.0, 1.0));
var<private> kUv: array<vec2<f32>, 3> = array<vec2<f32>, 3>(vec2(0.0, 0.0), vec2(0.0, 2.0), vec2(2.0, 0.0));
@vertex fn vs_main(@builtin(vertex_index) i: u32) -> VSOut {
  var o: VSOut; o.pos = vec4<f32>(kPos[i], 0.0, 1.0); o.uv = kUv[i]; return o;
}
fn luma(c: vec3<f32>) -> f32 { return dot(c, vec3<f32>(0.299, 0.587, 0.114)); }
fn tap(uv: vec2<f32>) -> vec3<f32> { return textureSampleLevel(src, samp, uv, 0.0).rgb; }
)";

// AMD FidelityFX CAS (MIT) in its common simplified form. P.a.xy = texel, P.a.z = sharpness 0..1.
constexpr std::string_view kSharpen = R"(
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let t = P.a.xy;
  let e = tap(in.uv);
  let a = tap(in.uv + vec2(0.0, -t.y));
  let b = tap(in.uv + vec2(-t.x, 0.0));
  let c = tap(in.uv + vec2(t.x, 0.0));
  let d = tap(in.uv + vec2(0.0, t.y));
  let mn = min(min(min(a, b), min(c, d)), e);
  let mx = max(max(max(a, b), max(c, d)), e);
  let rcpM = 1.0 / max(mx, vec3<f32>(1e-4));
  let amp = sqrt(clamp(min(mn, vec3<f32>(1.0) - mx) * rcpM, vec3<f32>(0.0), vec3<f32>(1.0)));
  let peak = -1.0 / mix(8.0, 5.0, clamp(P.a.z, 0.0, 1.0));
  let w = amp * peak;
  let outc = (w * (a + b + c + d) + e) / (4.0 * w + vec3<f32>(1.0));
  return vec4<f32>(clamp(outc, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}
)";

// FXAA 3.11 quality preset (Timothy Lottes, NVIDIA).
constexpr std::string_view kFxaa = R"(
fn lum_at(uv: vec2<f32>) -> f32 { return luma(tap(uv)); }
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let ts = P.a.xy;
  let uv = in.uv;
  let colorCenter = tap(uv);
  let lC = luma(colorCenter);
  let lD = lum_at(uv + vec2(0.0, ts.y));
  let lU = lum_at(uv - vec2(0.0, ts.y));
  let lL = lum_at(uv - vec2(ts.x, 0.0));
  let lR = lum_at(uv + vec2(ts.x, 0.0));
  let lMin = min(lC, min(min(lD, lU), min(lL, lR)));
  let lMax = max(lC, max(max(lD, lU), max(lL, lR)));
  let lRange = lMax - lMin;
  if (lRange < max(0.0312, lMax * 0.125)) { return vec4<f32>(colorCenter, 1.0); }
  let lDL = lum_at(uv + vec2(-ts.x, ts.y));
  let lUR = lum_at(uv + vec2(ts.x, -ts.y));
  let lUL = lum_at(uv - ts);
  let lDR = lum_at(uv + ts);
  let lDU = lD + lU; let lLR = lL + lR;
  let lLC = lDL + lUL; let lDC = lDL + lDR; let lRC = lDR + lUR; let lUC = lUR + lUL;
  let edgeH = abs(-2.0 * lL + lLC) + abs(-2.0 * lC + lDU) * 2.0 + abs(-2.0 * lR + lRC);
  let edgeV = abs(-2.0 * lU + lUC) + abs(-2.0 * lC + lLR) * 2.0 + abs(-2.0 * lD + lDC);
  let isH = edgeH >= edgeV;
  // Side 1 is the negative direction (left / up, -x / -y in texture space) so that a negative step
  // moves towards it on both axes.
  let l1 = select(lL, lU, isH);
  let l2 = select(lR, lD, isH);
  let g1 = l1 - lC; let g2 = l2 - lC;
  let is1 = abs(g1) >= abs(g2);
  let gScaled = 0.25 * max(abs(g1), abs(g2));
  var stepLength = select(ts.x, ts.y, isH);
  var lLocalAvg = 0.0;
  if (is1) { stepLength = -stepLength; lLocalAvg = 0.5 * (l1 + lC); } else { lLocalAvg = 0.5 * (l2 + lC); }
  var currentUv = uv;
  if (isH) { currentUv.y += stepLength * 0.5; } else { currentUv.x += stepLength * 0.5; }
  let offset = select(vec2(0.0, ts.y), vec2(ts.x, 0.0), isH);
  var uv1 = currentUv - offset;
  var uv2 = currentUv + offset;
  var lEnd1 = lum_at(uv1) - lLocalAvg;
  var lEnd2 = lum_at(uv2) - lLocalAvg;
  var reached1 = abs(lEnd1) >= gScaled;
  var reached2 = abs(lEnd2) >= gScaled;
  if (!reached1) { uv1 -= offset; }
  if (!reached2) { uv2 += offset; }
  let quality = array<f32, 12>(1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
  for (var i = 2; i < 12; i++) {
    if (reached1 && reached2) { break; }
    if (!reached1) { lEnd1 = lum_at(uv1) - lLocalAvg; }
    if (!reached2) { lEnd2 = lum_at(uv2) - lLocalAvg; }
    reached1 = abs(lEnd1) >= gScaled;
    reached2 = abs(lEnd2) >= gScaled;
    if (!reached1) { uv1 -= offset * quality[i]; }
    if (!reached2) { uv2 += offset * quality[i]; }
  }
  // Distances along the edge: x for a horizontal edge, y for a vertical one. (They were swapped: for
  // horizontal edges both came out as +-step/2, summed to 0 and the division made black pixels.)
  let dist1 = select(uv.y - uv1.y, uv.x - uv1.x, isH);
  let dist2 = select(uv2.y - uv.y, uv2.x - uv.x, isH);
  let isDir1 = dist1 < dist2;
  let distFinal = min(dist1, dist2);
  let edgeThickness = dist1 + dist2;
  let pixelOffset = -distFinal / max(edgeThickness, 1.0e-6) + 0.5;
  let isLCSmaller = lC < lLocalAvg;
  let correctVariation = (select(lEnd2, lEnd1, isDir1) < 0.0) != isLCSmaller;
  var finalOffset = select(0.0, pixelOffset, correctVariation);
  let lAvg = (1.0 / 12.0) * (2.0 * (lDU + lLR) + lLC + lRC);
  let sp1 = clamp(abs(lAvg - lC) / lRange, 0.0, 1.0);
  let sp2 = (-2.0 * sp1 + 3.0) * sp1 * sp1;
  finalOffset = max(finalOffset, sp2 * sp2 * 0.75);
  var finalUv = uv;
  if (isH) { finalUv.y += finalOffset * stepLength; } else { finalUv.x += finalOffset * stepLength; }
  return vec4<f32>(tap(finalUv), 1.0);
}
)";

// Bloom bright pass into the quarter-size texture (P.a.z threshold). With P.b.x set the source is
// the 3D-scene snapshot in EFB space: P.c = game picture rect in the image (uv), P.e = presented
// EFB rect (normalized).
constexpr std::string_view kBright = R"(
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let t = P.a.xy;
  var uv = in.uv;
  if (P.b.x > 0.5) {
    let duv = (in.uv - P.c.xy) / P.c.zw;
    if (any(duv < vec2<f32>(0.0)) || any(duv > vec2<f32>(1.0))) { return vec4<f32>(0.0, 0.0, 0.0, 1.0); }
    uv = P.e.xy + duv * P.e.zw;
  }
  var c = tap(uv) + tap(uv + vec2(t.x, 0.0)) + tap(uv + vec2(0.0, t.y)) + tap(uv + t);
  c *= 0.25;
  let l = luma(c);
  let w = smoothstep(P.a.z - 0.1, P.a.z + 0.1, l);
  return vec4<f32>(c * w, 1.0);
}
)";
// Separable Gaussian blur, P.a.xy texel, P.a.zw direction * radius.
constexpr std::string_view kBlur = R"(
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let step = P.a.xy * P.a.zw;
  let w = array<f32, 5>(0.2270270, 0.1945946, 0.1216216, 0.0540541, 0.0162162);
  var c = tap(in.uv) * w[0];
  for (var i = 1; i < 5; i++) {
    let o = step * f32(i);
    c += (tap(in.uv + o) + tap(in.uv - o)) * w[i];
  }
  return vec4<f32>(c, 1.0);
}
)";
constexpr std::string_view kComposite = R"(
@group(0) @binding(3) var glow: texture_2d<f32>;
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let c = tap(in.uv) + textureSampleLevel(glow, samp, in.uv, 0.0).rgb * P.a.z;
  return vec4<f32>(clamp(c, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}
)";

// SSAO. Binding 3 = accumulated EFB depth (reversed Z: 1 near, 0 far) of the world band only.
// P.a.xy image texel, P.a.z radius, P.a.w intensity; P.b.xy depth dims, P.b.z = A (xf[4]),
// P.b.w = B (xf[5]); P.c = game picture rect in the image (uv); P.d.z/w = world band znear/zfar;
// P.e = presented EFB rect (normalized); P.f = (samples, bias fraction, fade distance, power).
// Output: r = occlusion factor, g = linear depth (-1 = no world here).
constexpr std::string_view kSsao = R"(
@group(0) @binding(3) var depthTex: texture_2d<f32>;
// Depth texel under a picture position. The depth buffer usually has fewer rows than the output
// image, so everything works on whole depth texels: per-image-pixel positions repeat a row every
// few lines and those repeats turned into dark horizontal lines.
fn texel_of(duv: vec2<f32>) -> vec2<i32> {
  let suv = P.e.xy + clamp(duv, vec2<f32>(0.0), vec2<f32>(1.0)) * P.e.zw;
  return clamp(vec2<i32>(floor(suv * P.b.xy)), vec2<i32>(0), vec2<i32>(P.b.xy) - vec2<i32>(1));
}
// View-space position (x right, y up, z = distance) of a texel centre from the stored 1 / distance;
// z < 0 = none. P.d.zw = horizontal / vertical scale of the world projection (xf[0], xf[2]).
fn texel_pos(t: vec2<i32>) -> vec3<f32> {
  let dims = vec2<i32>(P.b.xy);
  if (any(t < vec2<i32>(0)) || any(t >= dims)) { return vec3<f32>(0.0, 0.0, -1.0); }
  let inv = textureLoad(depthTex, t, 0).r;
  if (inv <= 0.0) { return vec3<f32>(0.0, 0.0, -1.0); }
  let d = 1.0 / inv;
  let suv = (vec2<f32>(t) + vec2<f32>(0.5)) / P.b.xy;
  let ndc = vec2<f32>(suv.x * 2.0 - 1.0, 1.0 - suv.y * 2.0);
  return vec3<f32>(ndc.x / P.d.z * d, ndc.y / P.d.w * d, d);
}
fn view_pos(duv: vec2<f32>) -> vec3<f32> { return texel_pos(texel_of(duv)); }
fn pick_delta(c: vec3<f32>, a: vec3<f32>, b: vec3<f32>) -> vec3<f32> {
  // Smaller of the two one-sided differences, so object edges do not tilt the normal.
  let da = select(vec3<f32>(1.0e6), a - c, a.z > 0.0);
  let db = select(vec3<f32>(1.0e6), c - b, b.z > 0.0);
  return select(db, da, abs(da.z) < abs(db.z));
}
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let duv = (in.uv - P.c.xy) / P.c.zw;
  if (any(duv < vec2<f32>(0.0)) || any(duv > vec2<f32>(1.0))) { return vec4<f32>(1.0, -1.0, 0.0, 1.0); }
  let t0 = texel_of(duv);
  let c = texel_pos(t0);
  if (c.z <= 0.0 || c.z > 4000.0) { return vec4<f32>(1.0, -1.0, 0.0, 1.0); }
  // Surface normal from the neighbouring depth texels.
  let dx = pick_delta(c, texel_pos(t0 + vec2<i32>(1, 0)), texel_pos(t0 - vec2<i32>(1, 0)));
  let dy = pick_delta(c, texel_pos(t0 + vec2<i32>(0, 1)), texel_pos(t0 - vec2<i32>(0, 1)));
  var n = normalize(cross(dx, dy));
  if (dot(n, -c) < 0.0) { n = -n; }
  // Sample radius in pixels, tuned on a 1080-row picture; scaled so it covers the same screen area
  // at any internal resolution (SSAO now runs at EFB size).
  let resScale = (1.0 / P.a.y) / 1080.0;
  let rpx = clamp(P.a.z * 180.0 / c.z, 2.0, 40.0) * resScale;
  let ip = vec2<u32>(in.pos.xy) % vec2<u32>(4u);
  let idx = f32(ip.x + ip.y * 4u);
  let noise = idx * 0.39269908;
  let jitter = fract(idx * 0.618034) / 12.0;
  let radius = P.a.z * 1.5;
  let cosBias = clamp(P.f.y * 8.0, 0.0, 0.6);   // bias slider: ignore near-tangent samples
  let count = i32(clamp(P.f.x, 4.0, 32.0));
  let fcount = f32(count);
  var occ = 0.0;
  for (var i = 0; i < count; i++) {
    let fi = f32(i);
    let ang = noise + fi * 2.39996;
    let r = (fi + 0.5) / fcount + jitter * 12.0 / fcount;
    let off = vec2<f32>(cos(ang), sin(ang)) * r * rpx * P.a.xy;
    let s = view_pos(duv + off / P.c.zw);
    if (s.z <= 0.0) { continue; }
    let v = s - c;
    let len = length(v);
    if (len < 1.0e-4) { continue; }
    // Only geometry rising above the surface plane occludes; flat floors never shade themselves.
    let cosA = dot(v / len, n);
    // Squared falloff: nearby geometry (creases, contact with walls, the ship's legs) dominates,
    // so broad raised parts of a model are not darkened as a whole.
    let f = 1.0 - clamp(len / radius, 0.0, 1.0);
    let falloff = f * f;
    occ += max(0.0, cosA - cosBias) * falloff;
  }
  var ao = clamp(1.0 - P.a.w * occ / fcount * 3.0, 0.0, 1.0);
  ao = pow(ao, P.f.w);
  if (P.f.z > 0.0) { ao = mix(ao, 1.0, smoothstep(P.f.z * 0.5, P.f.z, c.z)); }
  // Diagnostic views (WII_COMPILED_POST_DEBUG_VIEW): 3 = normal y, 4 = normal x, shown in grey.
  if (P.d.x > 3.5) { return vec4<f32>(n.x * 0.5 + 0.5, c.z, 0.0, 1.0); }
  if (P.d.x > 2.5) { return vec4<f32>(n.y * 0.5 + 0.5, c.z, 0.0, 1.0); }
  return vec4<f32>(ao, c.z, 0.0, 1.0);
}
)";

// Depth-aware 5x5 Gaussian blur over the AO texture; P.a.z = step (px), P.a.w = edge sharpness.
constexpr std::string_view kSsaoBlur = R"(
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let c = textureLoad(src, vec2<i32>(in.pos.xy), 0);
  if (c.g <= 0.0) { return c; }
  var sum = 0.0;
  var wsum = 0.0;
  let dims = vec2<i32>(textureDimensions(src));
  let stepPx = max(P.a.z, 1.0);
  let tol = mix(0.25, 0.01, clamp(P.a.w, 0.0, 1.0));
  for (var y = -2; y <= 2; y++) {
    for (var x = -2; x <= 2; x++) {
      let o = vec2<f32>(f32(x), f32(y)) * stepPx;
      let q = clamp(vec2<i32>(in.pos.xy + o), vec2<i32>(0), dims - vec2<i32>(1));
      let s = textureLoad(src, q, 0);
      let gw = exp(-0.25 * f32(x * x + y * y));
      let w = select(0.0, gw, s.g > 0.0 && abs(s.g - c.g) < c.g * tol);
      sum += s.r * w;
      wsum += w;
    }
  }
  return vec4<f32>(sum / max(wsum, 1.0e-4), c.g, 0.0, 1.0);
}
)";

// Applies the blurred occlusion (binding 3); P.c = picture rect; debug inset when P.d.y is set.
// Runs inside the frame on the EFB (P.c = P.e = whole texture), so no visor masking is needed.
constexpr std::string_view kSsaoApply = R"(
@group(0) @binding(3) var aoTex: texture_2d<f32>;
@group(0) @binding(4) var faceMask: texture_2d<f32>;
@group(0) @binding(5) var reflTex: texture_2d<f32>;
@group(0) @binding(6) var shaftTex: texture_2d<f32>;
// The source is the EFB snapshot at the same size: read texels exactly and keep the alpha channel,
// which the game uses as its own glow mask (writing 1 made its bloom flare over everything).
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let texel = textureLoad(src, vec2<i32>(in.pos.xy), 0);
  let duv = (in.uv - P.c.xy) / P.c.zw;
  if (P.d.y > 0.5 && duv.x >= 0.0 && duv.x < 0.4 && duv.y > 0.6 && duv.y <= 1.0) {
    let iuv = vec2<f32>(duv.x / 0.4, (duv.y - 0.6) / 0.4);
    let s = textureSampleLevel(aoTex, samp, P.c.xy + iuv * P.c.zw, 0.0);
    if (s.g <= 0.0) { return vec4<f32>(0.0, 0.3, 0.0, texel.a); }
    return vec4<f32>(clamp(s.g / 60.0, 0.0, 1.0) * mix(0.35, 1.0, s.r), 0.0, 0.0, texel.a);
  }
  // P.f.x = 1 when SSAO ran (otherwise aoTex is a placeholder), P.f.y = 1 with floor reflections.
  var aoTexel = vec4<f32>(1.0, -1.0, 0.0, 1.0);
  if (P.f.x > 0.5) { aoTexel = textureLoad(aoTex, vec2<i32>(in.pos.xy), 0); }
  var ao = aoTexel.r;
  // Glow protection (P.e.x = strength): Prime writes a glow amount in the EFB alpha. Measured on
  // 27/10/2026: ordinary surfaces 0.1-0.55, glowing ones (visor glass, light strips, the cannon's
  // energy) close to 1. Emissive surfaces do not receive ambient occlusion; without this the face
  // behind Samus' visor glass showed its occlusion through the glow.
  let glow = smoothstep(0.6, 0.9, texel.a) * clamp(P.e.x, 0.0, 1.0);
  ao = mix(ao, 1.0, glow);
  // Exclusion mask (P.e.y = 1 when this block has one): Samus' face behind the visor glass. It also
  // keeps the screen-space reflections off it: their normal comes from the depth, flat per triangle,
  // so the low-poly face counted as "wall" facet by facet (darker triangles along the nose, F7
  // 05/11/2026 23:16).
  var marked = false;
  var effect = 0.0;
  if (P.e.y > 0.5) {
    let md = vec2<f32>(textureDimensions(faceMask));
    let mq = vec2<i32>(in.pos.xy * md / vec2<f32>(textureDimensions(src)));
    let m = textureLoad(faceMask, clamp(mq, vec2<i32>(0), vec2<i32>(md) - vec2<i32>(1)), 0).r;
    marked = m > 0.5;
    if (marked) { ao = 1.0; }
    // 0..0.49: an effect (fog, explosion, particles) drawn over the world here, by how much it changed
    // the picture. Its colour is in this texel too, so the occlusion of what lies behind fades with it.
    effect = select(clamp(m / 0.49, 0.0, 1.0), 0.0, marked);
    ao = mix(ao, 1.0, effect);
    // Mask view: magenta = excluded (face, visor glass), cyan = effect coverage (fog, explosions).
    if (P.d.x > 5.5) {
      let shown = mix(texel.rgb * 0.3, vec3<f32>(0.1, 0.9, 1.0), effect);
      return vec4<f32>(select(shown, vec3<f32>(1.0, 0.2, 0.8), marked), texel.a);
    }
  }
  if (P.d.x > 4.5 && P.d.x < 5.5) { return vec4<f32>(vec3<f32>(texel.a), texel.a); }  // diagnostic: glow mask
  // Diagnostic: occlusion / normal only, on pixels that have world depth in this block.
  if (P.d.x > 1.5 && aoTexel.g > 0.0) { return vec4<f32>(vec3<f32>(ao), texel.a); }
  var outc = texel.rgb * ao;
  if (P.f.y > 0.5 && !marked) {
    let r = textureLoad(reflTex, vec2<i32>(in.pos.xy), 0);
    // Specular occlusion (P.f.z): creases the ambient occlusion darkens reflect less too.
    let occl = select(1.0, ao, P.f.z > 0.5 && P.f.x > 0.5);
    outc = mix(outc, r.rgb, clamp(r.a, 0.0, 1.0) * occl * (1.0 - effect));
  }
  // Light shafts (P.f.w), added at quarter resolution with bilinear upsampling.
  if (P.f.w > 0.5) {
    outc += textureSampleLevel(shaftTex, samp, in.uv, 0.0).rgb;
  }
  return vec4<f32>(outc, texel.a);
}
)";

// Screen-space reflections on floors (experimental). Binding 1 = scene colour (EFB snapshot),
// binding 3 = accumulated 1 / distance. P.a.xy EFB texel, P.a.z strength, P.a.w max ray length;
// P.b.xy depth dims, P.b.z relief, P.b.w roughness; P.d.zw = projection scales; P.f.xyz = world up in this view space (x right,
// y up, z forward), P.f.w = 1 when known. Output: reflected colour, weight.
constexpr std::string_view kSsr = R"(
@group(0) @binding(3) var depthTex: texture_2d<f32>;
fn ssr_pos(t: vec2<i32>) -> vec3<f32> {
  let dims = vec2<i32>(P.b.xy);
  if (any(t < vec2<i32>(0)) || any(t >= dims)) { return vec3<f32>(0.0, 0.0, -1.0); }
  let inv = textureLoad(depthTex, t, 0).r;
  if (inv <= 0.0) { return vec3<f32>(0.0, 0.0, -1.0); }
  let d = 1.0 / inv;
  let suv = (vec2<f32>(t) + vec2<f32>(0.5)) / P.b.xy;
  let ndc = vec2<f32>(suv.x * 2.0 - 1.0, 1.0 - suv.y * 2.0);
  return vec3<f32>(ndc.x / P.d.z * d, ndc.y / P.d.w * d, d);
}
fn ssr_delta(c: vec3<f32>, a: vec3<f32>, b: vec3<f32>) -> vec3<f32> {
  let da = select(vec3<f32>(1.0e6), a - c, a.z > 0.0);
  let db = select(vec3<f32>(1.0e6), c - b, b.z > 0.0);
  return select(db, da, abs(da.z) < abs(db.z));
}
fn ssr_uv(q: vec3<f32>) -> vec2<f32> {
  let ndc = vec2<f32>(q.x * P.d.z / q.z, q.y * P.d.w / q.z);
  return vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let none = vec4<f32>(0.0);
  if (P.f.w < 0.5) { return none; }
  let suv0 = in.uv;
  let t0 = clamp(vec2<i32>(floor(suv0 * P.b.xy)), vec2<i32>(0), vec2<i32>(P.b.xy) - vec2<i32>(1));
  let c = ssr_pos(t0);
  if (c.z <= 0.0 || c.z > 4000.0) { return none; }
  let dx = ssr_delta(c, ssr_pos(t0 + vec2<i32>(1, 0)), ssr_pos(t0 - vec2<i32>(1, 0)));
  let dy = ssr_delta(c, ssr_pos(t0 + vec2<i32>(0, 1)), ssr_pos(t0 - vec2<i32>(0, 1)));
  var n = normalize(cross(dx, dy));
  if (dot(n, -c) < 0.0) { n = -n; }
  // Floors: the surface faces the world's up (P.a.z = floor strength, 0 = off). Walls (P.e.x = wall
  // strength, 0 = off, experimental): surfaces about perpendicular to it.
  let up = normalize(P.f.xyz);
  let floorness = smoothstep(0.80, 0.93, dot(n, up)) * select(0.0, 1.0, P.a.z > 0.0);
  let wallness = (1.0 - smoothstep(0.20, 0.40, abs(dot(n, up)))) * select(0.0, 1.0, P.e.x > 0.0);
  if (floorness <= 0.0 && wallness <= 0.0) { return none; }
  let isWall = floorness <= 0.0;
  // Glowing surfaces (EFB alpha, Prime's glow mask) keep their own look.
  let here = textureSampleLevel(src, samp, suv0, 0.0);
  if (here.a > 0.6) { return none; }
  // The floor texture's own detail, from the colour around this pixel (2 px apart).
  let px = P.a.xy * 2.0;
  let l0 = luma(here.rgb);
  let l1 = luma(textureSampleLevel(src, samp, suv0 + vec2<f32>(px.x, 0.0), 0.0).rgb);
  let l2 = luma(textureSampleLevel(src, samp, suv0 - vec2<f32>(px.x, 0.0), 0.0).rgb);
  let l3 = luma(textureSampleLevel(src, samp, suv0 + vec2<f32>(0.0, px.y), 0.0).rgb);
  let l4 = luma(textureSampleLevel(src, samp, suv0 - vec2<f32>(0.0, px.y), 0.0).rgb);
  let mean = (l0 + l1 + l2 + l3 + l4) * 0.2;
  let variance = ((l0 - mean) * (l0 - mean) + (l1 - mean) * (l1 - mean) + (l2 - mean) * (l2 - mean) +
                  (l3 - mean) * (l3 - mean) + (l4 - mean) * (l4 - mean)) * 0.2;
  // Smoothness from the contrast relative to the local brightness, so a dark rock floor counts as
  // busy as a bright one: rock, rubble and grating reflect little, plain panels a lot.
  let contrast = variance / max(mean * mean, 0.0025);
  // Walls only where the texture is really smooth (panels, glass); worn or busy walls stay matte.
  let smoothness = select(1.0 - smoothstep(0.02, 0.2, contrast), 1.0 - smoothstep(0.01, 0.06, contrast), isWall);
  if (smoothness <= 0.0) { return none; }
  // Cavities (seams, grooves, grime: darker than their surroundings) reflect less.
  let cavity = mix(0.35, 1.0, smoothstep(0.55, 0.95, l0 / max(mean, 1.0e-3)));
  // Relief (P.b.z): the luminance gradient as a height map tilts the normal along the floor, so the
  // reflection bends at panel edges and seams instead of lying on a perfect mirror.
  let gx = (l1 - l2) / max(mean, 0.05);
  let gy = (l3 - l4) / max(mean, 0.05);
  let tx = normalize(select(vec3<f32>(1.0, 0.0, 0.0), dx, dot(dx, dx) > 1.0e-12));
  let ty = normalize(select(vec3<f32>(0.0, -1.0, 0.0), dy, dot(dy, dy) > 1.0e-12));
  let bump = clamp(P.b.z, 0.0, 2.0) * 0.12;
  let nb = normalize(n - (tx * clamp(gx, -1.5, 1.5) + ty * clamp(gy, -1.5, 1.5)) * bump);
  let v = normalize(c);
  let r = reflect(v, nb);
  // March along the reflected ray in view space, growing steps, then refine the hit.
  var t = 0.05 + c.z * 0.01;
  var prevT = 0.0;
  var hit = false;
  var hitUv = vec2<f32>(0.0);
  for (var i = 0; i < 32; i++) {
    let q = c + r * t;
    if (q.z <= 0.05) { break; }
    let uv = ssr_uv(q);
    if (any(uv < vec2<f32>(0.0)) || any(uv > vec2<f32>(1.0))) { break; }
    let s = ssr_pos(vec2<i32>(floor(uv * P.b.xy)));
    if (s.z > 0.0 && q.z > s.z + 0.02 && q.z - s.z < max(0.25, t * 0.35)) {
      var lo = prevT;
      var hi = t;
      for (var k = 0; k < 5; k++) {
        let mid = (lo + hi) * 0.5;
        let qm = c + r * mid;
        let sm = ssr_pos(vec2<i32>(floor(ssr_uv(qm) * P.b.xy)));
        if (sm.z > 0.0 && qm.z > sm.z) { hi = mid; } else { lo = mid; }
      }
      hitUv = ssr_uv(c + r * hi);
      t = hi;
      hit = true;
      break;
    }
    prevT = t;
    t = t * 1.2 + 0.02;
    if (t > P.a.w) { break; }
  }
  if (!hit) { return none; }
  // Walls fade out further from the screen edges, and for rays heading back towards the camera
  // (what they would show is behind it): the game's own reflection stays there instead of a hole.
  let edge = smoothstep(0.0, select(0.08, 0.2, isWall), min(min(hitUv.x, hitUv.y), min(1.0 - hitUv.x, 1.0 - hitUv.y)));
  let away = select(1.0, smoothstep(0.05, 0.35, r.z), isWall);
  let distanceFade = 1.0 - smoothstep(P.a.w * 0.5, P.a.w, t);
  let fres = 0.04 + 0.96 * pow(1.0 - max(dot(-v, nb), 0.0), 5.0);
  // Roughness (P.b.w): the reflection blurs with the distance it travelled, like brushed or worn
  // metal; eight taps on a disc rotated per pixel (4x4 pattern), radius in screen space.
  let rough = clamp(P.b.w, 0.0, 1.0);
  let hitQ = c + r * t;
  let radiusUv = clamp(t * rough * 0.18 * P.d.z / max(hitQ.z, 0.05) * 0.5, 0.0, 0.06);
  let ip = vec2<u32>(in.pos.xy) % vec2<u32>(4u);
  let spin = f32(ip.x + ip.y * 4u) * 0.39269908;
  var refl = textureSampleLevel(src, samp, hitUv, 0.0).rgb;
  if (radiusUv > P.a.x * 0.75) {
    var sum = refl;
    for (var k = 0; k < 8; k++) {
      let ang = spin + f32(k) * 0.78539816;
      let rad = radiusUv * (0.35 + 0.65 * f32(k & 1));
      let o = vec2<f32>(cos(ang), sin(ang) * P.a.y / P.a.x) * rad;
      sum += textureSampleLevel(src, samp, clamp(hitUv + o, vec2<f32>(0.0), vec2<f32>(1.0)), 0.0).rgb;
    }
    refl = sum / 9.0;
  }
  // Self-lit surfaces (lava, light panels) do not show a dark mirror image over their own light: the
  // reflection fades out on very bright texels. Lava is smooth enough to count as a polished floor,
  // and the rock mixed into it flickered as the number of applications per frame changed (F5 of
  // 2026-11-01, Bryyo lava pool).
  let selfLit = 1.0 - smoothstep(0.50, 0.72, l0);
  let strength = select(P.a.z * floorness, P.e.x * wallness * away, isWall) * selfLit;
  let w = clamp(strength * mix(0.3, 1.0, fres) * smoothness * cavity * edge * distanceFade *
                mix(1.0, 0.7, rough), 0.0, 1.0);
  return vec4<f32>(refl, w);
}
)";

// Area probe (reflection colours): pixel 0 = average colour of the upper half of the scene, pixel 1 =
// of the lower half; 8 x 6 bilinear samples each over the scene colour (binding 1).
constexpr std::string_view kAreaProbe = R"(
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let half = select(0.0, 0.5, in.pos.x >= 1.0);
  var sum = vec3<f32>(0.0);
  for (var y = 0; y < 6; y++) {
    for (var x = 0; x < 8; x++) {
      let uv = vec2<f32>((f32(x) + 0.5) / 8.0, half + (f32(y) + 0.5) / 12.0);
      sum += tap(uv);
    }
  }
  return vec4<f32>(sum / 48.0, 1.0);
}
)";

// Light shafts (GPU Gems 3, ch. 13, "Volumetric Light Scattering as a Post-Process"), at quarter
// resolution: for each light on screen, the bright and glowing parts of the scene are summed along
// the line towards the light with a decay. P.a.z intensity, P.a.w light count; P.b = light 0 / 1 uv,
// P.c = light 2 / 3 uv, P.e = per-light weight; P.f = (decay, density, threshold, glow weight), P.d.x
// = picture aspect (width / height).
constexpr std::string_view kShafts = R"(
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let lights = array<vec2<f32>, 4>(P.b.xy, P.b.zw, P.c.xy, P.c.zw);
  let count = i32(P.a.w);
  var acc = vec3<f32>(0.0);
  for (var li = 0; li < 4; li++) {
    if (li >= count) { break; }
    let w = P.e[li];
    if (w <= 0.0) { continue; }
    let lp = lights[li];
    let delta = (in.uv - lp) * (P.f.y / 24.0);
    var uv = in.uv;
    var decay = 1.0;
    var sum = vec3<f32>(0.0);
    for (var k = 0; k < 24; k++) {
      uv -= delta;
      if (any(uv < vec2<f32>(0.0)) || any(uv > vec2<f32>(1.0))) { break; }
      let s = textureSampleLevel(src, samp, uv, 0.0);
      let bright = max(s.rgb - vec3<f32>(P.f.z), vec3<f32>(0.0)) / max(1.0 - P.f.z, 0.05);
      let glow = smoothstep(0.6, 0.9, s.a) * P.f.w;
      sum += (bright + s.rgb * glow) * decay;
      decay *= P.f.x;
    }
    let d = length((in.uv - lp) * vec2<f32>(P.d.x, 1.0));
    acc += sum / 24.0 * w * (1.0 - smoothstep(0.15, 1.1, d));
  }
  return vec4<f32>(acc * P.a.z, 1.0);
}
)";

// Colour grading: P.a.z brightness, P.a.w contrast, P.b.x saturation, P.b.y gamma,
// P.b.z temperature, P.b.w vignette, P.c = viewport in uv.
constexpr std::string_view kColor = R"(
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  var c = tap(in.uv);
  c = (c - vec3<f32>(0.5)) * P.a.w + vec3<f32>(0.5 + P.a.z);
  let l = luma(c);
  c = mix(vec3<f32>(l), c, P.b.x);
  c = c * vec3<f32>(1.0 + 0.12 * P.b.z, 1.0, 1.0 - 0.12 * P.b.z);
  c = pow(max(c, vec3<f32>(0.0)), vec3<f32>(1.0 / max(P.b.y, 0.05)));
  let duv = (in.uv - P.c.xy) / P.c.zw;
  let dist = distance(clamp(duv, vec2<f32>(0.0), vec2<f32>(1.0)), vec2<f32>(0.5)) * 1.41421;
  c *= 1.0 - P.b.w * smoothstep(0.45, 1.05, dist);
  return vec4<f32>(clamp(c, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}
)";

// Depth accumulation: the EFB depth restricted to the world band (P.a.zw = band znear/zfar),
// combined with the previous accumulation (binding 1) by max (the nearest surface wins). No blending:
// R32Float is not blendable without a device feature, and enabling one changes the pipeline cache key.
// Stores 1 / view distance (P.b.zw = A, B of the world camera); the nearest surface is the largest
// value, which the MAX blend keeps. 0 = nothing of the world here. The target is R32Float: with half
// floats the step size doubles at every power of two, which showed up as bands and a seam on floors.
constexpr std::string_view kDepthMerge = R"(
@group(0) @binding(3) var efbDepth: texture_depth_2d;
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let q = vec2<i32>(in.pos.xy * P.a.xy);
  let current = textureLoad(efbDepth, q, 0);
  let w = 1.0 - current;
  let inBand = current > 0.0 && w >= P.a.z - 1.0e-4 && w <= P.a.w + 1.0e-4;
  let ndc = (w - P.a.z) / max(P.a.w - P.a.z, 1.0e-6) - 1.0;
  let den = ndc + P.b.z;
  let d = select(-1.0, P.b.w / den, abs(den) > 1.0e-7);
  let ok = inBand && ndc >= -1.0001 && ndc <= -0.00005 && d > 0.0 && d < 4000.0;
  let prev = textureLoad(src, vec2<i32>(in.pos.xy), 0).r;
  return vec4<f32>(max(prev, select(0.0, 1.0 / max(d, 1.0e-3), ok)), 0.0, 0.0, 1.0);
}
)";

// Separable tent blur over P.a.z texels along P.b.xy (one texel step), for soften_copy.
constexpr std::string_view kCopySoften = R"(
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let r = clamp(P.a.z, 0.0, 8.0);
  let n = i32(ceil(r));
  var sum = vec4<f32>(0.0);
  var wsum = 0.0;
  for (var k = -8; k <= 8; k++) {
    if (abs(k) > n) { continue; }
    let w = max(0.0, 1.0 - abs(f32(k)) / (r + 1.0));
    sum += textureSampleLevel(src, samp, in.uv + P.b.xy * f32(k), 0.0) * w;
    wsum += w;
  }
  return sum / max(wsum, 1.0e-4);
}
)";

// Exclusion mask (Samus' face). kFaceSnapshot keeps the raw EFB depth before a run of listed draws;
// kFaceDiff stores the depth those draws wrote (the texels that changed) into the pass' face depth;
// kFaceFinalize, when the pass is merged, marks the texels where that depth is still the final one,
// i.e. the face was not covered later by the helmet or anything else in front of it.
constexpr std::string_view kFaceSnapshot = R"(
@group(0) @binding(3) var efbDepth: texture_depth_2d;
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  return vec4<f32>(textureLoad(efbDepth, vec2<i32>(in.pos.xy), 0), 0.0, 0.0, 1.0);
}
)";
constexpr std::string_view kFaceDiff = R"(
@group(0) @binding(3) var efbDepth: texture_depth_2d;
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let q = vec2<i32>(in.pos.xy);
  let current = textureLoad(efbDepth, q, 0);
  if (current == textureLoad(src, q, 0).r) { discard; }
  return vec4<f32>(current, 0.0, 0.0, 1.0);
}
)";
constexpr std::string_view kFaceFinalize = R"(
@group(0) @binding(3) var efbDepth: texture_depth_2d;
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let q = vec2<i32>(in.pos.xy);
  let face = textureLoad(src, q, 0).r;
  if (face == 0.0 || textureLoad(efbDepth, q, 0) != face) { discard; }
  return vec4<f32>(1.0, 0.0, 0.0, 1.0);
}
)";

// Cover runs (transparent visor glass): every texel whose EFB colour changed during the run.
constexpr std::string_view kCoverDiff = R"(
@group(0) @binding(3) var efbColor: texture_2d<f32>;
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let q = vec2<i32>(in.pos.xy);
  let d = abs(textureLoad(efbColor, q, 0) - textureLoad(src, q, 0));
  if (max(max(d.r, d.g), d.b) < 1.5 / 255.0) { discard; }
  return vec4<f32>(1.0, 0.0, 0.0, 1.0);
}
)";

// Effect runs (fog, explosions, particles drawn without depth over the world): how strongly the run
// changed each texel, from 0 (untouched) to full coverage at a change of 48/255. Stored as 0..0.49 and
// combined with max blending, so it never reaches the binary marks (1.0, read as > 0.5) of the face and
// cover runs: kSsaoApply fades the occlusion and the reflections by it instead of removing them.
constexpr std::string_view kEffectDiff = R"(
@group(0) @binding(3) var efbColor: texture_2d<f32>;
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let q = vec2<i32>(in.pos.xy);
  let d = abs(textureLoad(efbColor, q, 0) - textureLoad(src, q, 0));
  let cover = smoothstep(1.5 / 255.0, 48.0 / 255.0, max(max(d.r, d.g), d.b));
  if (cover <= 0.0) { discard; }
  return vec4<f32>(0.49 * cover, 0.0, 0.0, 1.0);
}
)";

// ------------------------------------------------------------------------------------------
// Modern lighting, level 1 (docs/iluminacion_motor.md). These passes only change the world snapshot
// that kSsaoApply writes back into the EFB; the SSAO passes above are untouched.
// ------------------------------------------------------------------------------------------

// Contact shadows: from each world pixel a short ray is marched through the accumulated depth towards
// each of the frame's strongest lights (binding 4: RGBA32F, row 0 = position in this view, x right,
// y up, z = distance, w = range; row 1 = luminance, directional flag). A hit darkens the share of the
// pixel's light that light gives (N.L and its falloff against an ambient weight). Binding 1 = scene
// snapshot (glow mask in alpha), binding 3 = accumulated 1 / distance, binding 5 = exclusion mask.
// P.a.xy EFB texel, P.a.z strength, P.a.w ray length; P.b.xy depth dims, P.b.z light count, P.b.w
// ambient weight; P.c.x jitter offset; P.d.zw projection scales; P.e.x = 1 with the mask.
// Output: r = light factor (1 = unshadowed), g = distance (-1 = no world here).
constexpr std::string_view kContact = R"(
@group(0) @binding(3) var depthTex: texture_2d<f32>;
@group(0) @binding(4) var lightTex: texture_2d<f32>;
@group(0) @binding(5) var maskTex: texture_2d<f32>;
fn cs_pos(t: vec2<i32>) -> vec3<f32> {
  let dims = vec2<i32>(P.b.xy);
  if (any(t < vec2<i32>(0)) || any(t >= dims)) { return vec3<f32>(0.0, 0.0, -1.0); }
  let inv = textureLoad(depthTex, t, 0).r;
  if (inv <= 0.0) { return vec3<f32>(0.0, 0.0, -1.0); }
  let d = 1.0 / inv;
  let suv = (vec2<f32>(t) + vec2<f32>(0.5)) / P.b.xy;
  let ndc = vec2<f32>(suv.x * 2.0 - 1.0, 1.0 - suv.y * 2.0);
  return vec3<f32>(ndc.x / P.d.z * d, ndc.y / P.d.w * d, d);
}
fn cs_delta(c: vec3<f32>, a: vec3<f32>, b: vec3<f32>) -> vec3<f32> {
  let da = select(vec3<f32>(1.0e6), a - c, a.z > 0.0);
  let db = select(vec3<f32>(1.0e6), c - b, b.z > 0.0);
  return select(db, da, abs(da.z) < abs(db.z));
}
fn cs_uv(q: vec3<f32>) -> vec2<f32> {
  let ndc = vec2<f32>(q.x * P.d.z / q.z, q.y * P.d.w / q.z);
  return vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let none = vec4<f32>(1.0, -1.0, 0.0, 1.0);
  let dims = vec2<i32>(P.b.xy);
  let t0 = clamp(vec2<i32>(floor(in.uv * P.b.xy)), vec2<i32>(0), dims - vec2<i32>(1));
  let c = cs_pos(t0);
  if (c.z <= 0.0 || c.z > 4000.0) { return none; }
  // Glowing surfaces (the game's glow mask) and excluded texels (Samus' face) keep their light.
  let here = textureLoad(src, vec2<i32>(in.pos.xy), 0);
  if (here.a > 0.6) { return vec4<f32>(1.0, c.z, 0.0, 1.0); }
  if (P.e.x > 0.5) {
    let md = vec2<f32>(textureDimensions(maskTex));
    let mq = vec2<i32>(in.pos.xy * md / vec2<f32>(textureDimensions(src)));
    if (textureLoad(maskTex, clamp(mq, vec2<i32>(0), vec2<i32>(md) - vec2<i32>(1)), 0).r > 0.5) {
      return vec4<f32>(1.0, c.z, 0.0, 1.0);
    }
  }
  let dx = cs_delta(c, cs_pos(t0 + vec2<i32>(1, 0)), cs_pos(t0 - vec2<i32>(1, 0)));
  let dy = cs_delta(c, cs_pos(t0 + vec2<i32>(0, 1)), cs_pos(t0 - vec2<i32>(0, 1)));
  var n = normalize(cross(dx, dy));
  if (dot(n, -c) < 0.0) { n = -n; }
  let ip = vec2<u32>(in.pos.xy) % vec2<u32>(4u);
  let jitter = fract(f32(ip.x * 4u + ip.y) * 0.3819660 + P.c.x);
  let rayLen = P.a.w * (1.0 + c.z * 0.015);
  let bias = 0.02 + c.z * 0.0015;
  let thick = 0.25 + c.z * 0.02;
  var wsum = P.b.w;
  var osum = 0.0;
  let count = i32(P.b.z);
  for (var i = 0; i < 4; i++) {
    if (i >= count) { break; }
    let l0 = textureLoad(lightTex, vec2<i32>(i, 0), 0);
    let l1 = textureLoad(lightTex, vec2<i32>(i, 1), 0);
    var ldir = vec3<f32>(0.0);
    var dist = 1.0e6;
    var att = 1.0;
    if (l1.y > 0.5) {
      ldir = normalize(l0.xyz);
    } else {
      let dv = l0.xyz - c;
      dist = length(dv);
      if (dist >= l0.w || dist < 1.0e-3) { continue; }
      ldir = dv / dist;
      let x = dist / l0.w;
      att = clamp(1.0 - x * x, 0.0, 1.0);
      att = att * att;
    }
    let ndl = dot(n, ldir);
    if (ndl <= 0.02) { continue; }
    let e = l1.x * att * ndl;
    if (e < 0.005) { continue; }
    let maxT = min(rayLen, dist * 0.8);
    var occ = 0.0;
    for (var k = 0; k < 12; k++) {
      let tt = bias + (f32(k) + jitter) / 12.0 * maxT;
      let q = c + ldir * tt;
      if (q.z <= 0.05) { break; }
      let uv = cs_uv(q);
      if (any(uv < vec2<f32>(0.0)) || any(uv > vec2<f32>(1.0))) { break; }
      let s = cs_pos(vec2<i32>(floor(uv * P.b.xy)));
      if (s.z <= 0.0) { continue; }
      let dz = q.z - s.z;
      if (dz > bias && dz < thick) {
        occ = 1.0 - f32(k) / 24.0;
        break;
      }
    }
    wsum += e;
    osum += e * occ;
  }
  if (osum <= 0.0) { return vec4<f32>(1.0, c.z, 0.0, 1.0); }
  let fade = 1.0 - smoothstep(30.0, 60.0, c.z);
  return vec4<f32>(1.0 - clamp(P.a.z * osum / wsum, 0.0, 0.85) * fade, c.z, 0.0, 1.0);
}
)";

// Temporal stabilisation of the contact shadows: the previous result (binding 3) is reprojected with
// the camera's motion and blended in, clamped to this frame's 3x3 neighbourhood; rejected where the
// distance does not match (disocclusion). P.a.z history weight (0 = none); P.b.xy projection scales;
// P.c / P.d / P.e = rows of (previous view * inverse(current view)) in GX view space.
constexpr std::string_view kContactTemporal = R"(
@group(0) @binding(3) var histTex: texture_2d<f32>;
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let p = vec2<i32>(in.pos.xy);
  let cur = textureLoad(src, p, 0);
  if (cur.g <= 0.0 || P.a.z <= 0.0) { return cur; }
  let ndc = vec2<f32>(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0);
  let z = cur.g;
  let v = vec4<f32>(ndc.x / P.b.x * z, ndc.y / P.b.y * z, -z, 1.0);
  let q = vec3<f32>(dot(P.c, v), dot(P.d, v), dot(P.e, v));
  let zq = -q.z;
  if (zq <= 0.05) { return cur; }
  let uvq = vec2<f32>(q.x * P.b.x / zq * 0.5 + 0.5, 0.5 - q.y * P.b.y / zq * 0.5);
  if (any(uvq < vec2<f32>(0.0)) || any(uvq > vec2<f32>(1.0))) { return cur; }
  let h = textureSampleLevel(histTex, samp, uvq, 0.0);
  if (h.g <= 0.0 || abs(h.g - zq) > 0.03 * zq + 0.05) { return cur; }
  let dims = vec2<i32>(textureDimensions(src));
  var lo = cur.r;
  var hi = cur.r;
  for (var y = -1; y <= 1; y++) {
    for (var x = -1; x <= 1; x++) {
      let s = textureLoad(src, clamp(p + vec2<i32>(x, y), vec2<i32>(0), dims - vec2<i32>(1)), 0);
      if (s.g > 0.0) { lo = min(lo, s.r); hi = max(hi, s.r); }
    }
  }
  return vec4<f32>(mix(cur.r, clamp(h.r, lo, hi), P.a.z), cur.g, 0.0, 1.0);
}
)";

// Exposure metering: log2 of the average linear luminance over 16 x 9 points of the world snapshot,
// weighted towards the centre; glowing texels (lights, energy) count less.
constexpr std::string_view kLumMeasure = R"(
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  var sum = 0.0;
  var wsum = 0.0;
  for (var y = 0; y < 9; y++) {
    for (var x = 0; x < 16; x++) {
      let uv = vec2<f32>((f32(x) + 0.5) / 16.0, (f32(y) + 0.5) / 9.0);
      let s = textureSampleLevel(src, samp, uv, 0.0);
      let lin = pow(max(s.rgb, vec3<f32>(0.0)), vec3<f32>(2.2));
      let l = dot(lin, vec3<f32>(0.2126, 0.7152, 0.0722));
      let w = (1.0 + 2.0 * (1.0 - clamp(length(uv - vec2<f32>(0.5)) * 2.0, 0.0, 1.0))) * select(1.0, 0.3, s.a > 0.6);
      sum += log2(max(l, 1.0e-4)) * w;
      wsum += w;
    }
  }
  return vec4<f32>(sum / wsum, 0.0, 0.0, 1.0);
}
)";
// Eye adaptation: moves the adapted value (binding 3) towards the measured one (binding 1) by P.a.x;
// P.a.y = 1 when the adapted value is valid (otherwise it starts at the measurement).
constexpr std::string_view kAdapt = R"(
@group(0) @binding(3) var prevTex: texture_2d<f32>;
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let m = textureLoad(src, vec2<i32>(0), 0).r;
  let p = textureLoad(prevTex, vec2<i32>(0), 0).r;
  return vec4<f32>(select(m, mix(p, m, P.a.x), P.a.y > 0.5), 0.0, 0.0, 1.0);
}
)";
// The world snapshot with the contact shadows (binding 3, EFB size) and the exposure (binding 4, the
// adapted log2 luminance) applied; alpha (the game's glow mask) kept. P.a.x contact on, P.a.y exposure
// on, P.a.z range (EV: up to +range brighter, up to -range/2 darker), P.a.w reference log2 luminance;
// P.b.x share of the difference corrected.
// Brightening uses an extended Reinhard curve whose white point is the gain: what was white stays
// white and bright areas roll off instead of clipping.
constexpr std::string_view kSceneLight = R"(
@group(0) @binding(3) var contactTex: texture_2d<f32>;
@group(0) @binding(4) var expTex: texture_2d<f32>;
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
  let p = vec2<i32>(in.pos.xy);
  let texel = textureLoad(src, p, 0);
  var c = texel.rgb;
  if (P.a.x > 0.5) {
    let s = textureLoad(contactTex, p, 0);
    if (s.g > 0.0) { c = c * s.r; }
  }
  if (P.a.y > 0.5) {
    let avg = textureLoad(expTex, vec2<i32>(0), 0).r;
    // Darkening is limited to half the range: bright places are already lit as the artists meant.
    let ev = clamp((P.a.w - avg) * P.b.x, -P.a.z * 0.5, P.a.z);
    let k = exp2(ev);
    var lin = pow(max(c, vec3<f32>(0.0)), vec3<f32>(2.2)) * k;
    if (k > 1.0) {
      let l = dot(lin, vec3<f32>(0.2126, 0.7152, 0.0722));
      let ld = l * (1.0 + l / (k * k)) / (1.0 + l);
      lin = lin * (ld / max(l, 1.0e-5));
    }
    c = pow(clamp(lin, vec3<f32>(0.0), vec3<f32>(1.0)), vec3<f32>(1.0 / 2.2));
  }
  return vec4<f32>(c, texel.a);
}
)";

struct Params {
  float a[4]{}, b[4]{}, c[4]{}, d[4]{}, e[4]{}, f[4]{};
};

// Contact: binding 3 and 4 unfilterable (R32Float depth, RGBA32Float lights), binding 5 a float mask.
enum class Layout { Basic, TwoTextures, Depth, DepthValue, ThreeTextures, FourTextures, FiveTextures, Contact };

struct Pass {
  wgpu::RenderPipeline pipeline;
  wgpu::Buffer uniforms;
  wgpu::BindGroupLayout layout;
  Layout kind = Layout::Basic;
};

Pass g_sharpen, g_fxaa, g_bright, g_blurH, g_blurV, g_composite, g_ssao, g_ssaoBlur, g_ssaoBlur2, g_ssaoApply,
    g_color, g_depthMerge, g_copySoftH, g_copySoftV, g_faceSnapshot, g_faceDiff, g_faceFinalize, g_coverDiff, g_ssr, g_areaProbe, g_shafts;
webgpu::TextureWithSampler g_shaftsTex;  // RGBA16Float quarter-resolution light shafts
webgpu::TextureWithSampler g_probeTex;   // RGBA8Unorm 2x1 area colours (above, below)
// Area probe readback (render thread encodes, after_submit maps, the callback stores the colours).
enum class ProbeState { Available, CopySubmitted, MapPending };
struct ProbeSlot {
  wgpu::Buffer buffer;
  ProbeState state = ProbeState::Available;
};
std::array<ProbeSlot, 3> g_probeSlots{};
std::mutex g_probeMutex;
std::atomic<bool> g_probeWanted{false};
std::atomic<bool> g_shaftsWanted{false};
float g_areaUp[3] = {0.0f, 0.0f, 0.0f}, g_areaDown[3] = {0.0f, 0.0f, 0.0f};
bool g_areaValid = false;
webgpu::TextureWithSampler g_ssrEfb;  // RGBA16Float reflected colour + weight at EFB size
webgpu::TextureWithSampler g_coverBefore;  // EFB colour before a cover run
wgpu::TextureFormat g_coverFormat = wgpu::TextureFormat::Undefined;
// Exclusion mask state (render thread). g_faceBefore / g_faceDepth: R32Float raw depth at EFB depth
// size; g_faceMask: R8Unorm, 1 = excluded, kept for the whole world block like the accumulation.
std::atomic<bool> g_effectsOption{false};  // (ssao || ssr || wallSsr) && ssaoUnderEffects
std::atomic<bool> g_excludeOption{false};  // (ssao || ssr || wallSsr) && ssaoSkipMarked, read per draw without the lock
webgpu::TextureWithSampler g_faceBefore, g_faceDepth, g_faceMask;
Pass g_effectDiff;  // kEffectDiff, max-blended into g_faceMask
bool g_faceDepthPending = false;  // the current pass has face depth to finalize at its merge
bool g_faceMaskDirty = false;     // the mask has marked texels (cleared with the accumulation)
bool g_faceMaskNew = false;       // just created: cleared before the first texel is marked
uint32_t g_faceRuns = 0;          // runs bracketed since the last SSAO application (diagnostics)
wgpu::TextureFormat g_ssaoApplyFormat = wgpu::TextureFormat::Undefined;  // EFB colour format
webgpu::TextureWithSampler g_aoEfbA, g_aoEfbB;  // RGBA16Float occlusion + distance at EFB size
wgpu::Sampler g_linearClamp;
wgpu::TextureFormat g_builtFormat = wgpu::TextureFormat::Undefined;

struct Scratch {
  webgpu::TextureWithSampler full;
  webgpu::TextureWithSampler quarterA, quarterB;
  webgpu::TextureWithSampler aoA, aoB;  // RGBA16Float: occlusion + linear depth
  uint32_t width = 0, height = 0;
};
Scratch g_scratch;

webgpu::TextureWithSampler g_depthAccum;     // R32Float accumulated world 1/distance of the frame
webgpu::TextureWithSampler g_depthMergeTemp; // merge output; swapped with g_depthAccum after each merge
webgpu::TextureWithSampler g_dummyTexture;   // 1x1 source for passes that need none
bool g_depthAccumValid = false;
bool g_depthCarry = false;  // a world block continues in the next batch: keep the accumulation
uint32_t g_carryDraws = 0;  // world-camera draws of that open block so far
std::atomic<bool> g_depthCaptured{false};
webgpu::TextureWithSampler g_sceneColor;     // colour after the 3D world pass (bloom source)
bool g_sceneColorValid = false;

// Camera selection (recording side). Prime splits the depth range per camera: world [0.125, 1],
// sky layers [0.999, 1], arm cannon [0.002, 0.004], a 64x64 radar, weapon distortion near the
// camera. The world camera is the full-width perspective camera with the most draws.
std::mutex g_projMutex;
float g_curA = 0.0f, g_curB = 0.0f, g_curSx = 1.0f, g_curSy = 1.0f;
bool g_curPerspective = false;
float g_curZNear = 0.0f, g_curZFar = 1.0f, g_curVpWidth = 0.0f, g_curEfbWidth = 0.0f;
struct CameraCount { float a, b, zn, zf, sx, sy; uint32_t draws; };
std::array<CameraCount, 16> g_cameras{};
size_t g_cameraCount = 0;
float g_sealedA = 0.0f, g_sealedB = 0.0f, g_sealedNear = 0.0f, g_sealedFar = 1.0f, g_sealedSx = 1.0f, g_sealedSy = 1.0f;
bool g_sealedScene = false;
// World up in view space (SSR floors): noted per frame by the command processor, sealed with the camera.
float g_curUp[3] = {0.0f, 0.0f, 0.0f};
bool g_curUpValid = false;
float g_sealedUp[3] = {0.0f, 0.0f, 0.0f};
uint32_t g_sealedUpAge = 1000;
// Lights for the shafts (GX view space), noted per frame and sealed with the camera.
float g_curLights[4][4] = {};
uint32_t g_curLightCount = 0;
float g_sealedLights[4][4] = {};
uint32_t g_sealedLightCount = 0;
// Modern lighting (same lock): the frame's engine lights {x, y, z (GX view), radius, luma, directional}
// and the world view matrix, sealed with the camera; the previous sealed view feeds the reprojection.
float g_curEngine[4][6] = {};
uint32_t g_curEngineCount = 0;
float g_sealedEngine[4][6] = {};
uint32_t g_sealedEngineCount = 0;
float g_curView[12] = {};
bool g_curViewValid = false;
float g_sealedView[12] = {}, g_sealedPrevView[12] = {};
bool g_sealedViewValid = false, g_sealedPrevViewValid = false;

std::mutex g_modernMutex;
Modern g_modern;
std::atomic<bool> g_modernActive{false};  // contact || exposure, read without the lock
Pass g_contact, g_contactBlur1, g_contactBlur2, g_contactTemporal, g_lumMeasure, g_adapt, g_sceneLight;
wgpu::TextureFormat g_sceneLightFormat = wgpu::TextureFormat::Undefined;
webgpu::TextureWithSampler g_contactA, g_contactB;   // RGBA16Float light factor + distance at EFB size
webgpu::TextureWithSampler g_contactHist[2];         // temporal history, ping-pong
uint32_t g_contactHistIndex = 0;
bool g_contactHistValid = false;
uint32_t g_contactFrame = 0;                         // jitter offset for the temporal blend
webgpu::TextureWithSampler g_lightsTex;              // RGBA32Float 4 x 2: the contact lights
webgpu::TextureWithSampler g_lumTex;                 // RGBA16Float 1 x 1: measured log2 luminance
webgpu::TextureWithSampler g_adaptTex[2];            // RGBA16Float 1 x 1: adapted value, ping-pong
uint32_t g_adaptIndex = 0;
bool g_adaptValid = false;
webgpu::TextureWithSampler g_litScene;               // the world snapshot with contact shadows / exposure

wgpu::BindGroupLayout make_layout(Layout kind) {
  std::array<wgpu::BindGroupLayoutEntry, 7> entries{
      wgpu::BindGroupLayoutEntry{.binding = 0, .visibility = wgpu::ShaderStage::Fragment,
                                 .sampler = {.type = wgpu::SamplerBindingType::Filtering}},
      wgpu::BindGroupLayoutEntry{.binding = 1, .visibility = wgpu::ShaderStage::Fragment,
                                 .texture = {.sampleType = kind == Layout::Depth ? wgpu::TextureSampleType::UnfilterableFloat
                                                                                  : wgpu::TextureSampleType::Float,
                                             .viewDimension = wgpu::TextureViewDimension::e2D}},
      wgpu::BindGroupLayoutEntry{.binding = 2, .visibility = wgpu::ShaderStage::Fragment,
                                 .buffer = {.type = wgpu::BufferBindingType::Uniform, .minBindingSize = sizeof(Params)}},
      wgpu::BindGroupLayoutEntry{.binding = 3, .visibility = wgpu::ShaderStage::Fragment,
                                 .texture = {.sampleType = kind == Layout::Depth        ? wgpu::TextureSampleType::Depth
                                                           : kind == Layout::DepthValue || kind == Layout::Contact
                                                               ? wgpu::TextureSampleType::UnfilterableFloat
                                                               : wgpu::TextureSampleType::Float,
                                             .viewDimension = wgpu::TextureViewDimension::e2D}},
      wgpu::BindGroupLayoutEntry{.binding = 4, .visibility = wgpu::ShaderStage::Fragment,
                                 .texture = {.sampleType = kind == Layout::Contact ? wgpu::TextureSampleType::UnfilterableFloat
                                                                                   : wgpu::TextureSampleType::Float,
                                             .viewDimension = wgpu::TextureViewDimension::e2D}},
      wgpu::BindGroupLayoutEntry{.binding = 5, .visibility = wgpu::ShaderStage::Fragment,
                                 .texture = {.sampleType = wgpu::TextureSampleType::Float,
                                             .viewDimension = wgpu::TextureViewDimension::e2D}},
      wgpu::BindGroupLayoutEntry{.binding = 6, .visibility = wgpu::ShaderStage::Fragment,
                                 .texture = {.sampleType = wgpu::TextureSampleType::Float,
                                             .viewDimension = wgpu::TextureViewDimension::e2D}},
  };
  const wgpu::BindGroupLayoutDescriptor descriptor{
      .label = "Post-process bind group layout",
      .entryCount = kind == Layout::Basic           ? 3u
                    : kind == Layout::FiveTextures  ? 7u
                    : kind == Layout::FourTextures  ? 6u
                    : kind == Layout::Contact       ? 6u
                    : kind == Layout::ThreeTextures ? 5u
                                                    : 4u,
      .entries = entries.data(),
  };
  return webgpu::g_device.CreateBindGroupLayout(&descriptor);
}

void build_pass(Pass& pass, Layout kind, std::string_view fragment, const char* label,
                wgpu::TextureFormat targetFormat = wgpu::TextureFormat::Undefined, bool blendMax = false) {
  if (targetFormat == wgpu::TextureFormat::Undefined) targetFormat = g_builtFormat;
  const std::string source = std::string(kPrelude) + std::string(fragment);
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = source.c_str();
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = label};
  auto module = webgpu::g_device.CreateShaderModule(&moduleDescriptor);
  pass.kind = kind;
  pass.layout = make_layout(kind);
  const wgpu::PipelineLayoutDescriptor layoutDescriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &pass.layout};
  auto pipelineLayout = webgpu::g_device.CreatePipelineLayout(&layoutDescriptor);
  const wgpu::BlendState maxBlend{
      .color = {.operation = wgpu::BlendOperation::Max, .srcFactor = wgpu::BlendFactor::One, .dstFactor = wgpu::BlendFactor::One},
      .alpha = {.operation = wgpu::BlendOperation::Max, .srcFactor = wgpu::BlendFactor::One, .dstFactor = wgpu::BlendFactor::One},
  };
  const std::array colorTargets{wgpu::ColorTargetState{
      .format = targetFormat, .blend = blendMax ? &maxBlend : nullptr, .writeMask = wgpu::ColorWriteMask::All}};
  const wgpu::FragmentState fragmentState{.module = module, .entryPoint = "fs_main",
                                          .targetCount = colorTargets.size(), .targets = colorTargets.data()};
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = label,
      .layout = pipelineLayout,
      .vertex = {.module = module, .entryPoint = "vs_main"},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
      .multisample = {.count = 1, .mask = UINT32_MAX},
      .fragment = &fragmentState,
  };
  pass.pipeline = webgpu::g_device.CreateRenderPipeline(&descriptor);
  const wgpu::BufferDescriptor bufferDescriptor{
      .label = label, .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, .size = sizeof(Params)};
  pass.uniforms = webgpu::g_device.CreateBuffer(&bufferDescriptor);
}

void ensure_pipelines(wgpu::TextureFormat format) {
  if (g_builtFormat == format && g_sharpen.pipeline) return;
  g_builtFormat = format;
  const wgpu::SamplerDescriptor samplerDescriptor{
      .label = "Post-process sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
      .mipmapFilter = wgpu::MipmapFilterMode::Nearest,
  };
  g_linearClamp = webgpu::g_device.CreateSampler(&samplerDescriptor);
  build_pass(g_sharpen, Layout::Basic, kSharpen, "Post sharpen (CAS)");
  build_pass(g_fxaa, Layout::Basic, kFxaa, "Post FXAA");
  build_pass(g_bright, Layout::Basic, kBright, "Post bloom bright");
  build_pass(g_blurH, Layout::Basic, kBlur, "Post bloom blur H");
  build_pass(g_blurV, Layout::Basic, kBlur, "Post bloom blur V");
  build_pass(g_composite, Layout::TwoTextures, kComposite, "Post bloom composite");
  build_pass(g_ssao, Layout::DepthValue, kSsao, "Post SSAO", wgpu::TextureFormat::RGBA16Float);
  build_pass(g_ssaoBlur, Layout::Basic, kSsaoBlur, "Post SSAO blur", wgpu::TextureFormat::RGBA16Float);
  build_pass(g_ssaoBlur2, Layout::Basic, kSsaoBlur, "Post SSAO blur 2", wgpu::TextureFormat::RGBA16Float);
  build_pass(g_ssr, Layout::DepthValue, kSsr, "Post floor reflections", wgpu::TextureFormat::RGBA16Float);
  build_pass(g_areaProbe, Layout::Basic, kAreaProbe, "Post area colours", wgpu::TextureFormat::RGBA8Unorm);
  build_pass(g_shafts, Layout::Basic, kShafts, "Post light shafts", wgpu::TextureFormat::RGBA16Float);
    build_pass(g_color, Layout::Basic, kColor, "Post colour");
  build_pass(g_depthMerge, Layout::Depth, kDepthMerge, "Post depth merge", wgpu::TextureFormat::R32Float);
  build_pass(g_copySoftH, Layout::Basic, kCopySoften, "Glow copy soften H");
  build_pass(g_copySoftV, Layout::Basic, kCopySoften, "Glow copy soften V");
  build_pass(g_faceSnapshot, Layout::Depth, kFaceSnapshot, "Post SSAO exclusion snapshot", wgpu::TextureFormat::R32Float);
  build_pass(g_faceDiff, Layout::Depth, kFaceDiff, "Post SSAO exclusion diff", wgpu::TextureFormat::R32Float);
  build_pass(g_faceFinalize, Layout::Depth, kFaceFinalize, "Post SSAO exclusion mask", wgpu::TextureFormat::R8Unorm);
  // Modern lighting: separate Pass objects (the contact blur reuses the SSAO blur's shader text only).
  build_pass(g_contact, Layout::Contact, kContact, "Post contact shadows", wgpu::TextureFormat::RGBA16Float);
  build_pass(g_contactBlur1, Layout::Basic, kSsaoBlur, "Post contact shadows blur 1", wgpu::TextureFormat::RGBA16Float);
  build_pass(g_contactBlur2, Layout::Basic, kSsaoBlur, "Post contact shadows blur 2", wgpu::TextureFormat::RGBA16Float);
  build_pass(g_contactTemporal, Layout::TwoTextures, kContactTemporal, "Post contact shadows temporal",
             wgpu::TextureFormat::RGBA16Float);
  build_pass(g_lumMeasure, Layout::Basic, kLumMeasure, "Post exposure metering", wgpu::TextureFormat::RGBA16Float);
  build_pass(g_adapt, Layout::TwoTextures, kAdapt, "Post exposure adaptation", wgpu::TextureFormat::RGBA16Float);
  Log.info("post-process pipelines built (format {})", static_cast<uint32_t>(format));
}

webgpu::TextureWithSampler make_target(uint32_t width, uint32_t height, wgpu::TextureFormat format, const char* label,
                                       wgpu::TextureUsage extraUsage = wgpu::TextureUsage::None) {
  webgpu::TextureWithSampler t;
  const wgpu::TextureDescriptor descriptor{
      .label = label,
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | extraUsage,
      .size = {width, height, 1},
      .format = format,
  };
  t.texture = webgpu::g_device.CreateTexture(&descriptor);
  t.view = t.texture.CreateView();
  t.size = {width, height, 1};
  t.format = format;
  return t;
}

void ensure_scratch(uint32_t width, uint32_t height) {
  if (g_scratch.width == width && g_scratch.height == height && g_scratch.full.texture) return;
  g_scratch.full = webgpu::create_render_texture(width, height, false);
  const uint32_t qw = std::max<uint32_t>(width / 4, 1), qh = std::max<uint32_t>(height / 4, 1);
  g_scratch.quarterA = webgpu::create_render_texture(qw, qh, false);
  g_scratch.quarterB = webgpu::create_render_texture(qw, qh, false);
  g_scratch.aoA = make_target(width, height, wgpu::TextureFormat::RGBA16Float, "Post SSAO A");
  g_scratch.aoB = make_target(width, height, wgpu::TextureFormat::RGBA16Float, "Post SSAO B");
  g_scratch.width = width;
  g_scratch.height = height;
}

wgpu::BindGroup bind(const Pass& pass, const wgpu::TextureView& source, const wgpu::TextureView* extra,
                     const wgpu::TextureView* extra2, const wgpu::TextureView* extra3 = nullptr,
                     const wgpu::TextureView* extra4 = nullptr) {
  std::array<wgpu::BindGroupEntry, 7> entries{
      wgpu::BindGroupEntry{.binding = 0, .sampler = g_linearClamp},
      wgpu::BindGroupEntry{.binding = 1, .textureView = source},
      wgpu::BindGroupEntry{.binding = 2, .buffer = pass.uniforms, .size = sizeof(Params)},
      wgpu::BindGroupEntry{.binding = 3, .textureView = extra ? *extra : wgpu::TextureView{}},
      wgpu::BindGroupEntry{.binding = 4, .textureView = extra2 ? *extra2 : wgpu::TextureView{}},
      wgpu::BindGroupEntry{.binding = 5, .textureView = extra3 ? *extra3 : wgpu::TextureView{}},
      wgpu::BindGroupEntry{.binding = 6, .textureView = extra4 ? *extra4 : wgpu::TextureView{}},
  };
  const wgpu::BindGroupDescriptor descriptor{
      .label = "Post-process bind group",
      .layout = pass.layout,
      .entryCount = pass.kind == Layout::Basic           ? 3u
                    : pass.kind == Layout::FiveTextures  ? 7u
                    : pass.kind == Layout::FourTextures  ? 6u
                    : pass.kind == Layout::Contact       ? 6u
                    : pass.kind == Layout::ThreeTextures ? 5u
                                                         : 4u,
      .entries = entries.data(),
  };
  return webgpu::g_device.CreateBindGroup(&descriptor);
}

void run(const wgpu::CommandEncoder& encoder, const Pass& pass, const Params& params,
         const wgpu::TextureView& source, const wgpu::TextureView* extra, const wgpu::TextureView& target,
         const char* label, bool load = false, const wgpu::TextureView* extra2 = nullptr,
         const wgpu::TextureView* extra3 = nullptr, const wgpu::TextureView* extra4 = nullptr) {
  webgpu::g_queue.WriteBuffer(pass.uniforms, 0, &params, sizeof(params));
  const auto bindGroup = bind(pass, source, extra, extra2, extra3, extra4);
  const std::array attachments{wgpu::RenderPassColorAttachment{
      .view = target, .loadOp = load ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store}};
  const wgpu::RenderPassDescriptor descriptor{
      .label = label, .colorAttachmentCount = attachments.size(), .colorAttachments = attachments.data(),
      .timestampWrites = gpu_timing::pass_writes(gpu_timing::Category::Post)};
  const auto rp = encoder.BeginRenderPass(&descriptor);
  rp.SetPipeline(pass.pipeline);
  rp.SetBindGroup(0, bindGroup, 0, nullptr);
  rp.Draw(3);
  rp.End();
}

Params texel_params(const webgpu::TextureWithSampler& texture) {
  Params p;
  p.a[0] = 1.0f / static_cast<float>(std::max<uint32_t>(texture.size.width, 1));
  p.a[1] = 1.0f / static_cast<float>(std::max<uint32_t>(texture.size.height, 1));
  return p;
}

// Presented EFB rectangle normalized to a texture of `width` x `height` render pixels.
void display_rect(float out[4], float width, float height) {
  const float sw = g_displaySrc[2].load(std::memory_order_relaxed), sh = g_displaySrc[3].load(std::memory_order_relaxed);
  width = std::max(width, 1.0f);
  height = std::max(height, 1.0f);
  if (sw > 0.0f && sh > 0.0f) {
    out[0] = g_displaySrc[0].load(std::memory_order_relaxed) / width;
    out[1] = g_displaySrc[1].load(std::memory_order_relaxed) / height;
    out[2] = sw / width;
    out[3] = sh / height;
  } else {
    out[0] = 0.0f; out[1] = 0.0f; out[2] = 1.0f; out[3] = 1.0f;
  }
}
void clear_face_mask(const wgpu::CommandEncoder& encoder) {
  if (!g_faceMaskDirty || !g_faceMask.view) return;
  const std::array attachments{wgpu::RenderPassColorAttachment{
      .view = g_faceMask.view, .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store,
      .clearValue = {0.0, 0.0, 0.0, 0.0}}};
  const wgpu::RenderPassDescriptor descriptor{
      .label = "Post SSAO exclusion mask reset", .colorAttachmentCount = attachments.size(), .colorAttachments = attachments.data()};
  encoder.BeginRenderPass(&descriptor).End();
  g_faceMaskDirty = false;
}

bool ensure_face_targets(const wgpu::Extent3D& size) {
  if (size.width == 0 || size.height == 0) return false;
  if (!g_faceMask.texture || g_faceMask.size.width != size.width || g_faceMask.size.height != size.height) {
    g_faceBefore = make_target(size.width, size.height, wgpu::TextureFormat::R32Float, "Post SSAO exclusion before");
    g_faceDepth = make_target(size.width, size.height, wgpu::TextureFormat::R32Float, "Post SSAO exclusion depth");
    g_faceMask = make_target(size.width, size.height, wgpu::TextureFormat::R8Unorm, "Post SSAO exclusion mask");
    g_faceMaskNew = true;
    g_faceMaskDirty = false;
    g_faceDepthPending = false;
  }
  if (!g_dummyTexture.texture) g_dummyTexture = make_target(1, 1, wgpu::TextureFormat::R16Float, "Post dummy source");
  return true;
}

// Called by merge_depth for the pass whose depth is about to be merged.
void finalize_face(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& depthView,
                   const wgpu::Extent3D& depthSize) {
  if (!g_faceDepthPending) return;
  g_faceDepthPending = false;
  if (depthSize.width != g_faceMask.size.width || depthSize.height != g_faceMask.size.height) return;
  run(encoder, g_faceFinalize, Params{}, g_faceDepth.view, &depthView, g_faceMask.view, "Post SSAO exclusion mask", true);
  g_faceMaskDirty = true;
}
} // namespace

void set_ssao_exclude_textures(const uint64_t* hashes, size_t count) {
  texture_replacement::set_texture_tag_list(texture_replacement::TagNoSsao, hashes, count);
}

void set_ssao_cover_textures(const uint64_t* hashes, size_t count) {
  texture_replacement::set_texture_tag_list(texture_replacement::TagSsaoCover, hashes, count);
}

bool ssao_effects_active() {
  return g_effectsOption.load(std::memory_order_acquire) && webgpu::g_graphicsConfig.msaaSamples <= 1;
}

bool ssao_exclude_active() {
  return (texture_replacement::texture_tags_listed() &
          (texture_replacement::TagNoSsao | texture_replacement::TagSsaoCover)) != 0 &&
         g_excludeOption.load(std::memory_order_acquire) && webgpu::g_graphicsConfig.msaaSamples <= 1;
}

void face_before(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& depthView, const wgpu::Extent3D& size,
                 bool cover, bool graduated) {
  if (!depthView || !ensure_face_targets(size)) return;
  ensure_pipelines(webgpu::g_graphicsConfig.surfaceConfiguration.format);
  if (g_faceMaskNew) {  // a new mask texture is cleared once, before anything is marked in it
    g_faceMaskNew = false;
    g_faceMaskDirty = true;
    clear_face_mask(encoder);
  }
  if (cover) {
    const auto& efb = webgpu::g_frameBuffer;
    if (!efb.texture || efb.size.width != size.width || efb.size.height != size.height) return;
    if (!g_coverBefore.texture || g_coverBefore.size.width != size.width || g_coverBefore.size.height != size.height ||
        g_coverFormat != efb.format) {
      g_coverBefore = make_target(size.width, size.height, efb.format, "Post SSAO cover before", wgpu::TextureUsage::CopyDst);
      g_coverFormat = efb.format;
      build_pass(g_coverDiff, Layout::TwoTextures, kCoverDiff, "Post SSAO cover diff", wgpu::TextureFormat::R8Unorm);
    }
    if (graduated && !g_effectDiff.pipeline)
      build_pass(g_effectDiff, Layout::TwoTextures, kEffectDiff, "Post SSAO effect cover", wgpu::TextureFormat::R8Unorm,
                 true);
    const wgpu::TexelCopyTextureInfo from{.texture = efb.texture};
    const wgpu::TexelCopyTextureInfo to{.texture = g_coverBefore.texture};
    const wgpu::Extent3D extent{size.width, size.height, 1};
    encoder.CopyTextureToTexture(&from, &to, &extent);
    return;
  }
  run(encoder, g_faceSnapshot, Params{}, g_dummyTexture.view, &depthView, g_faceBefore.view, "Post SSAO exclusion snapshot");
}

void face_after(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& depthView, const wgpu::Extent3D& size,
                bool cover, bool graduated) {
  if (cover) {
    const auto& efb = webgpu::g_frameBuffer;
    const Pass& diff = graduated ? g_effectDiff : g_coverDiff;
    if (!g_coverBefore.texture || !diff.pipeline || !efb.view || g_coverBefore.size.width != size.width ||
        g_coverBefore.size.height != size.height || g_faceMask.size.width != size.width)
      return;
    run(encoder, diff, Params{}, g_coverBefore.view, &efb.view, g_faceMask.view,
        graduated ? "Post SSAO effect mask" : "Post SSAO cover mask", true);
    g_faceMaskDirty = true;
    ++g_faceRuns;
    return;
  }
  if (!depthView || g_faceBefore.size.width != size.width || g_faceBefore.size.height != size.height) return;
  // The first run of a pass starts from an empty face depth; later runs add to it.
  run(encoder, g_faceDiff, Params{}, g_faceBefore.view, &depthView, g_faceDepth.view, "Post SSAO exclusion diff",
      g_faceDepthPending);
  g_faceDepthPending = true;
  ++g_faceRuns;
}

void set_config(const Config& config) {
  std::lock_guard<std::mutex> lock(g_configMutex);
  g_config = config;
  // The mask also keeps floor and wall reflections off the listed textures (kSsaoApply).
  g_excludeOption.store((config.ssao || config.ssr || config.wallSsr) && config.ssaoSkipMarked,
                        std::memory_order_release);
  g_effectsOption.store((config.ssao || config.ssr || config.wallSsr) && config.ssaoUnderEffects,
                        std::memory_order_release);
  g_shaftsWanted.store(config.lightShafts, std::memory_order_release);
}

Config config() {
  std::lock_guard<std::mutex> lock(g_configMutex);
  return g_config;
}

void set_modern(const Modern& modern) {
  std::lock_guard<std::mutex> lock(g_modernMutex);
  g_modern = modern;
  g_modernActive.store(modern.contact || modern.exposure, std::memory_order_release);
}

Modern modern() {
  std::lock_guard<std::mutex> lock(g_modernMutex);
  return g_modern;
}

void note_engine_lights(uint32_t count, const float (*positions)[3], const float* radius, const float* luma,
                        const uint8_t* directional) {
  std::lock_guard<std::mutex> lock(g_projMutex);
  g_curEngineCount = positions != nullptr ? std::min<uint32_t>(count, 4) : 0;
  for (uint32_t i = 0; i < g_curEngineCount; ++i) {
    for (int c = 0; c < 3; ++c) g_curEngine[i][c] = positions[i][c];
    g_curEngine[i][3] = radius[i];
    g_curEngine[i][4] = luma[i];
    g_curEngine[i][5] = directional[i] != 0 ? 1.0f : 0.0f;
  }
}

void note_world_view(const float* rows) {
  if (rows == nullptr) return;
  std::lock_guard<std::mutex> lock(g_projMutex);
  std::memcpy(g_curView, rows, sizeof(g_curView));
  g_curViewValid = true;
}

bool any_enabled() {
  const Config c = config();
  return c.sharpen || c.fxaa || c.bloom || c.ssao || c.color;
}

const char* status_note() { return g_statusNote; }

void note_display_source(float x, float y, float width, float height) {
  g_displaySrc[0].store(x, std::memory_order_relaxed);
  g_displaySrc[1].store(y, std::memory_order_relaxed);
  g_displaySrc[2].store(width, std::memory_order_relaxed);
  g_displaySrc[3].store(height, std::memory_order_relaxed);
}

void note_projection(const float* xf6, bool perspective) {
  if (xf6 == nullptr) return;
  std::lock_guard<std::mutex> lock(g_projMutex);
  g_curPerspective = perspective;
  g_curA = xf6[4];
  g_curB = xf6[5];
  g_curSx = xf6[0];
  g_curSy = xf6[2];
}

void note_viewport(float left, float top, float width, float height, float znear, float zfar, float efbWidth) {
  (void)left; (void)top; (void)height;
  std::lock_guard<std::mutex> lock(g_projMutex);
  g_curVpWidth = width;
  g_curEfbWidth = efbWidth;
  g_curZNear = znear;
  g_curZFar = zfar;
}

void note_draw() {
  float a = 0.0f, b = 0.0f, zn = 0.0f, zf = 0.0f;
  {
    std::lock_guard<std::mutex> lock(g_projMutex);
    if (!g_curPerspective || std::abs(g_curA) < 1.0e-7f) return;
    if (g_curEfbWidth > 0.0f && g_curVpWidth < g_curEfbWidth * 0.9f) return;  // radar / small views
    if (g_curZFar - g_curZNear < 0.01f) return;                               // cannon, sky layers
    a = g_curA;
    b = g_curB;
    zn = g_curZNear;
    zf = g_curZFar;
    bool found = false;
    for (size_t i = 0; i < g_cameraCount; ++i) {
      auto& c = g_cameras[i];
      if (c.a == g_curA && c.b == g_curB && c.zn == g_curZNear && c.zf == g_curZFar) { ++c.draws; found = true; break; }
    }
    if (!found && g_cameraCount < g_cameras.size())
      g_cameras[g_cameraCount++] = {g_curA, g_curB, g_curZNear, g_curZFar, g_curSx, g_curSy, 1};
  }
  gfx::note_pass_camera(a, b, zn, zf);
}

// A camera whose depth band is only a thin slice at the far end is a distant backdrop composited
// behind the world (RM3E01: the fleet and Norion seen from the gunship, band 0.956-1, B = -10.01).
// It can have more draws than the world itself (81 against 70), and as ships come and go the pick
// flipped every few frames: the cockpit panels lost and regained their occlusion (F5 31/10/2026
// 17:19-17:20). The world band starts at 0.125 in every saved dump (627 F7/F5 files).
constexpr float kFarLayerNear = 0.9f;

void note_world_up(float x, float y, float z) {
  std::lock_guard<std::mutex> lock(g_projMutex);
  // GX view space looks down -z; the SSR works with z = distance in front of the camera.
  g_curUp[0] = x; g_curUp[1] = y; g_curUp[2] = -z;
  g_curUpValid = true;
}

void note_lights(uint32_t count, const float (*positions)[3], const float* luminance) {
  std::lock_guard<std::mutex> lock(g_projMutex);
  g_curLightCount = std::min<uint32_t>(count, 4);
  for (uint32_t i = 0; i < g_curLightCount; ++i) {
    g_curLights[i][0] = positions[i][0]; g_curLights[i][1] = positions[i][1]; g_curLights[i][2] = positions[i][2];
    g_curLights[i][3] = luminance[i];
  }
}

bool light_shafts_wanted() { return g_shaftsWanted.load(std::memory_order_acquire); }

void set_area_probe(bool enabled) {
  g_probeWanted.store(enabled, std::memory_order_release);
  if (!enabled) {
    std::lock_guard<std::mutex> lock(g_probeMutex);
    g_areaValid = false;
  }
}

bool area_colours(float up[3], float down[3]) {
  std::lock_guard<std::mutex> lock(g_probeMutex);
  if (!g_areaValid) return false;
  for (int i = 0; i < 3; ++i) { up[i] = g_areaUp[i]; down[i] = g_areaDown[i]; }
  return true;
}

void after_submit() {
  std::array<size_t, 3> pending{};
  size_t count = 0;
  {
    std::lock_guard<std::mutex> lock(g_probeMutex);
    for (size_t i = 0; i < g_probeSlots.size(); ++i) {
      if (g_probeSlots[i].state != ProbeState::CopySubmitted) continue;
      g_probeSlots[i].state = ProbeState::MapPending;
      pending[count++] = i;
    }
  }
  for (size_t k = 0; k < count; ++k) {
    const size_t idx = pending[k];
    g_probeSlots[idx].buffer.MapAsync(
        wgpu::MapMode::Read, 0, 8, wgpu::CallbackMode::AllowSpontaneous,
        [idx](wgpu::MapAsyncStatus status, wgpu::StringView) {
          std::lock_guard<std::mutex> lock(g_probeMutex);
          auto& slot = g_probeSlots[idx];
          if (status == wgpu::MapAsyncStatus::Success) {
            const auto* px = static_cast<const uint8_t*>(slot.buffer.GetConstMappedRange(0, 8));
            if (px != nullptr) {
              // Smoothed over a few readings: the colours change with the area, not per frame.
              const float a = g_areaValid ? 0.25f : 1.0f;
              for (int c = 0; c < 3; ++c) {
                g_areaUp[c] += (static_cast<float>(px[c]) / 255.0f - g_areaUp[c]) * a;
                g_areaDown[c] += (static_cast<float>(px[4 + c]) / 255.0f - g_areaDown[c]) * a;
              }
              g_areaValid = g_probeWanted.load(std::memory_order_acquire);
            }
            slot.buffer.Unmap();
          }
          slot.state = ProbeState::Available;
        });
  }
}

void latch_frame() {
  std::lock_guard<std::mutex> lock(g_projMutex);
  // Light shafts: a frame whose lit draws gave no lights keeps the last ones for a few frames instead
  // of switching the shafts off for one frame (on/off flicker over bright areas such as lava, which
  // the game's own glow then amplified; F5 of 2026-11-01).
  static uint32_t lightlessFrames = 0;
  if (g_curLightCount > 0 || ++lightlessFrames > 8) {
    if (g_curLightCount > 0) lightlessFrames = 0;
    g_sealedLightCount = g_curLightCount;
    for (uint32_t i = 0; i < 4; ++i)
      for (int c = 0; c < 4; ++c) g_sealedLights[i][c] = g_curLights[i][c];
  }
  g_curLightCount = 0;
  // Modern lighting: the engine lights (held a few frames like the shafts' ones) and the world view,
  // with the previous one for the temporal reprojection.
  static uint32_t engineLightless = 0;
  if (g_curEngineCount > 0 || ++engineLightless > 8) {
    if (g_curEngineCount > 0) engineLightless = 0;
    g_sealedEngineCount = g_curEngineCount;
    std::memcpy(g_sealedEngine, g_curEngine, sizeof(g_sealedEngine));
  }
  g_curEngineCount = 0;
  if (g_curViewValid) {
    std::memcpy(g_sealedPrevView, g_sealedView, sizeof(g_sealedView));
    g_sealedPrevViewValid = g_sealedViewValid;
    std::memcpy(g_sealedView, g_curView, sizeof(g_sealedView));
    g_sealedViewValid = true;
  } else {
    g_sealedPrevViewValid = false;
    g_sealedViewValid = false;
  }
  g_curViewValid = false;
  if (g_curUpValid) {
    g_sealedUp[0] = g_curUp[0]; g_sealedUp[1] = g_curUp[1]; g_sealedUp[2] = g_curUp[2];
    g_sealedUpAge = 0;
  } else if (g_sealedUpAge < 1000) {
    ++g_sealedUpAge;
  }
  g_curUpValid = false;
  // The camera with the most draws, preferring any camera that is not a far backdrop layer.
  const CameraCount* best = nullptr;
  for (size_t i = 0; i < g_cameraCount; ++i) {
    const CameraCount& c = g_cameras[i];
    if (best == nullptr) { best = &c; continue; }
    const bool cFar = c.zn >= kFarLayerNear, bestFar = best->zn >= kFarLayerNear;
    if (cFar != bestFar ? !cFar : c.draws > best->draws) best = &c;
  }
  static const bool debugLog = debug_depth_requested();
  if (debugLog && g_config.ssao) {
    std::string line = "[post-ssao-cam] frame=" + std::to_string(gfx::current_frame()) + " cams=" + std::to_string(g_cameraCount);
    if (best) line += " pick=" + std::to_string(best->a) + "/" + std::to_string(best->b) + " band=" + std::to_string(best->zn) + "-" + std::to_string(best->zf) + " draws=" + std::to_string(best->draws);
    for (size_t i = 0; i < g_cameraCount; ++i)
      line += " | " + std::to_string(g_cameras[i].a) + "/" + std::to_string(g_cameras[i].b) + " " + std::to_string(g_cameras[i].zn) + "-" + std::to_string(g_cameras[i].zf) + " x" + std::to_string(g_cameras[i].draws);
    std::fprintf(stderr, "%s\n", line.c_str());
  }
  // A repeated presentation (no new draws) keeps the previous camera; only half a second without
  // any 3D scene means a menu, the map or a video.
  static uint32_t emptyLatches = 0;
  if (best) {
    g_sealedA = best->a; g_sealedB = best->b; g_sealedNear = best->zn; g_sealedFar = best->zf;
    g_sealedSx = std::abs(best->sx) > 1.0e-4f ? best->sx : 1.0f;
    g_sealedSy = std::abs(best->sy) > 1.0e-4f ? best->sy : 1.0f;
    g_sealedScene = true;
    emptyLatches = 0;
  } else if (++emptyLatches > 30) {
    g_sealedScene = false;
  }
  g_cameraCount = 0;
}

bool scene_active() {
  std::lock_guard<std::mutex> lock(g_projMutex);
  return g_sealedScene;
}

bool world_camera(std::array<float, 4>& camera) {
  std::lock_guard<std::mutex> lock(g_projMutex);
  camera = {g_sealedA, g_sealedB, g_sealedNear, g_sealedFar};
  return g_sealedScene;
}

bool wants_scene_color() {
  std::lock_guard<std::mutex> lock(g_configMutex);
  return (g_config.bloom && g_config.bloomWorldOnly) || g_config.ssao || g_config.ssr || g_config.wallSsr ||
         g_config.lightShafts ||
         g_probeWanted.load(std::memory_order_acquire) || g_modernActive.load(std::memory_order_acquire);
}

void capture_scene_color(const wgpu::CommandEncoder& encoder) {
  const auto& source = webgpu::present_source();
  if (!source.texture || source.size.width == 0) return;
  if (!g_sceneColor.texture || g_sceneColor.size.width != source.size.width ||
      g_sceneColor.size.height != source.size.height) {
    g_sceneColor = webgpu::create_render_texture(source.size.width, source.size.height, false);
  }
  // Exact texel copy (same size and format): keeps the alpha channel, which the game uses as its
  // glow mask and which the in-frame SSAO writes back into the EFB.
  const wgpu::TexelCopyTextureInfo from{.texture = source.texture};
  const wgpu::TexelCopyTextureInfo to{.texture = g_sceneColor.texture};
  const wgpu::Extent3D size{source.size.width, source.size.height, 1};
  encoder.CopyTextureToTexture(&from, &to, &size);
  g_sceneColorValid = true;
}

bool wants_depth() {
  if (webgpu::g_graphicsConfig.msaaSamples > 1) return false;
  std::lock_guard<std::mutex> lock(g_configMutex);
  return g_config.ssao || g_config.ssr || g_config.wallSsr || g_config.lightShafts ||
         g_probeWanted.load(std::memory_order_acquire) || g_modernActive.load(std::memory_order_acquire);
}

void begin_depth(const wgpu::CommandEncoder& encoder, const webgpu::TextureWithSampler& depth) {
  if (!depth.texture || depth.size.width == 0) { g_depthAccumValid = false; return; }
  // The game's world often straddles two batches: while a world block is open (set_depth_carry),
  // keep accumulating instead of clearing. The accumulation is cleared after every SSAO application.
  if (g_depthCarry && g_depthAccumValid && g_depthAccum.size.width == depth.size.width &&
      g_depthAccum.size.height == depth.size.height) {
    return;
  }
  if (!g_depthAccum.texture || g_depthAccum.size.width != depth.size.width ||
      g_depthAccum.size.height != depth.size.height) {
    // Two identical targets used in turn (ping-pong): no copy back after each merge.
    g_depthAccum = make_target(depth.size.width, depth.size.height, wgpu::TextureFormat::R32Float,
                               "Post depth accumulation A");
    g_depthMergeTemp = make_target(depth.size.width, depth.size.height, wgpu::TextureFormat::R32Float,
                                   "Post depth accumulation B");
  }
  if (!g_dummyTexture.texture) g_dummyTexture = make_target(1, 1, wgpu::TextureFormat::R16Float, "Post dummy source");
  clear_face_mask(encoder);
  g_faceDepthPending = false;
  // Reset to "far" (0 in reversed Z).
  const std::array attachments{wgpu::RenderPassColorAttachment{
      .view = g_depthAccum.view, .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store,
      .clearValue = {0.0, 0.0, 0.0, 1.0}}};
  const wgpu::RenderPassDescriptor descriptor{
      .label = "Post depth accumulation reset", .colorAttachmentCount = attachments.size(), .colorAttachments = attachments.data()};
  encoder.BeginRenderPass(&descriptor).End();
  g_depthAccumValid = true;
}

void merge_depth(const wgpu::CommandEncoder& encoder, const wgpu::Texture& depth, const wgpu::TextureView& depthView,
                 const wgpu::Extent3D& depthSize) {
  if (depthView) finalize_face(encoder, depthView, depthSize);
  if (!g_depthAccumValid || !depth || !depthView || depthSize.width == 0 || depthSize.height == 0) return;
  // Only full-scene targets: the EFB itself or an offscreen target at least half its size.
  const auto& accum = g_depthAccum.size;
  if (depthSize.width * 2 < accum.width || depthSize.height * 2 < accum.height) return;
  if (depth.GetSampleCount() > 1) return;
  ensure_pipelines(webgpu::g_graphicsConfig.surfaceConfiguration.format);
  Params p;
  p.a[0] = static_cast<float>(depthSize.width) / static_cast<float>(accum.width);
  p.a[1] = static_cast<float>(depthSize.height) / static_cast<float>(accum.height);
  {
    std::lock_guard<std::mutex> lock(g_projMutex);
    if (!g_sealedScene) return;  // no world camera this frame: nothing to accumulate
    p.a[2] = g_sealedNear;
    p.a[3] = g_sealedFar;
    p.b[2] = g_sealedA;
    p.b[3] = g_sealedB;
  }
  // The merge writes every texel (max of the previous value and this pass' depth), so the output
  // simply becomes the accumulation: a full-screen R32Float copy less per merge.
  run(encoder, g_depthMerge, p, g_depthAccum.view, &depthView, g_depthMergeTemp.view, "Post depth merge");
  std::swap(g_depthAccum, g_depthMergeTemp);
  g_depthCaptured.store(true, std::memory_order_release);
}

static const char* g_lastApplySkip = "";
const char* last_apply_skip() { return g_lastApplySkip; }

// Modern lighting: contact shadows and automatic exposure applied to the world snapshot. Returns the
// view the rest of the application reads instead of the snapshot (the snapshot itself when neither
// ran, so the SSAO, reflections and the write-back are then exactly as before). History and
// adaptation only advance on the native image (`allowProbe`); inserted images reuse them.
static wgpu::TextureView modern_scene(const wgpu::CommandEncoder& encoder, const webgpu::TextureWithSampler& efb,
                                      float scaleX, float scaleY, bool firstInGameFrame, bool allowProbe) {
  const Modern m = modern();
  if (!(m.contact || m.exposure)) {
    g_contactHistValid = false;
    g_adaptValid = false;
    return g_sceneColor.view;
  }
  float engine[4][6] = {};
  uint32_t engineCount = 0;
  float shaftLights[4][4] = {};
  uint32_t shaftCount = 0;
  float view[12] = {}, prevView[12] = {};
  bool viewOk = false, prevOk = false;
  {
    std::lock_guard<std::mutex> lock(g_projMutex);
    engineCount = g_sealedEngineCount;
    std::memcpy(engine, g_sealedEngine, sizeof(engine));
    shaftCount = g_sealedLightCount;
    std::memcpy(shaftLights, g_sealedLights, sizeof(shaftLights));
    viewOk = g_sealedViewValid;
    prevOk = g_sealedPrevViewValid;
    std::memcpy(view, g_sealedView, sizeof(view));
    std::memcpy(prevView, g_sealedPrevView, sizeof(prevView));
  }
  if (!g_dummyTexture.texture) g_dummyTexture = make_target(1, 1, wgpu::TextureFormat::R16Float, "Post dummy source");
  const uint32_t w = efb.size.width, h = efb.size.height;
  wgpu::TextureView contactView;
  bool contactRan = false;
  if (m.contact && m.contactStrength > 0.0f) {
    // Lights in the contact pass's view space (x right, y up, z = distance): the engine's, or without
    // them the lights the game loaded for its lit draws (the light shafts' ones) with a default range.
    float rows[2][4][4] = {};
    uint32_t n = 0;
    if (engineCount > 0) {
      for (uint32_t i = 0; i < engineCount && i < 4; ++i, ++n) {
        const bool directional = engine[i][5] > 0.5f;
        float x = engine[i][0], y = engine[i][1], z = -engine[i][2];
        if (directional) {
          const float len = std::max(std::sqrt(x * x + y * y + z * z), 1.0e-6f);
          x /= len; y /= len; z /= len;
        }
        rows[0][n][0] = x; rows[0][n][1] = y; rows[0][n][2] = z;
        rows[0][n][3] = directional ? 1.0e6f : std::max(engine[i][3], 0.05f);
        rows[1][n][0] = std::clamp(engine[i][4], 0.0f, 4.0f);
        rows[1][n][1] = directional ? 1.0f : 0.0f;
      }
    } else {
      for (uint32_t i = 0; i < shaftCount && i < 4; ++i, ++n) {
        rows[0][n][0] = shaftLights[i][0]; rows[0][n][1] = shaftLights[i][1]; rows[0][n][2] = -shaftLights[i][2];
        rows[0][n][3] = 10.0f;
        rows[1][n][0] = std::clamp(shaftLights[i][3], 0.0f, 1.0f);
      }
    }
    if (n > 0) {
      if (!g_contactA.texture || g_contactA.size.width != w || g_contactA.size.height != h) {
        g_contactA = make_target(w, h, wgpu::TextureFormat::RGBA16Float, "Post contact shadows A");
        g_contactB = make_target(w, h, wgpu::TextureFormat::RGBA16Float, "Post contact shadows B");
        g_contactHist[0] = make_target(w, h, wgpu::TextureFormat::RGBA16Float, "Post contact shadows history A");
        g_contactHist[1] = make_target(w, h, wgpu::TextureFormat::RGBA16Float, "Post contact shadows history B");
        g_contactHistValid = false;
      }
      if (!g_lightsTex.texture)
        g_lightsTex = make_target(4, 2, wgpu::TextureFormat::RGBA32Float, "Post contact lights", wgpu::TextureUsage::CopyDst);
      const wgpu::TexelCopyTextureInfo dst{.texture = g_lightsTex.texture};
      const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = 4 * 16, .rowsPerImage = 2};
      const wgpu::Extent3D extent{4, 2, 1};
      webgpu::g_queue.WriteTexture(&dst, rows, sizeof(rows), &layout, &extent);
      const bool temporal = m.temporal && !gx::frame_interpolation_active();
      if (allowProbe && temporal) ++g_contactFrame;
      Params p = texel_params(efb);
      p.a[2] = std::clamp(m.contactStrength, 0.0f, 1.0f);
      p.a[3] = std::clamp(m.contactLength, 0.05f, 3.0f);
      p.b[0] = static_cast<float>(g_depthAccum.size.width);
      p.b[1] = static_cast<float>(g_depthAccum.size.height);
      p.b[2] = static_cast<float>(n);
      p.b[3] = 0.3f;  // ambient weight: the share of a pixel's light no single light gives
      p.c[0] = temporal ? static_cast<float>(g_contactFrame % 4u) * 0.25f : 0.0f;
      p.d[2] = scaleX;
      p.d[3] = scaleY;
      const auto mc = config();
      const bool masked = (mc.ssaoSkipMarked || mc.ssaoUnderEffects) && g_faceMaskDirty && g_faceMask.view;
      p.e[0] = masked ? 1.0f : 0.0f;
      run(encoder, g_contact, p, g_sceneColor.view, &g_depthAccum.view, g_contactA.view, "Post contact shadows", false,
          &g_lightsTex.view, masked ? &g_faceMask.view : &g_dummyTexture.view);
      Params blur = p;
      const float resScale = static_cast<float>(h) / 1080.0f;
      blur.a[2] = std::max(1.0f, 1.5f * resScale);
      blur.a[3] = 0.6f;
      run(encoder, g_contactBlur1, blur, g_contactA.view, nullptr, g_contactB.view, "Post contact shadows blur 1");
      blur.a[2] = std::max(1.0f, 0.75f * resScale);
      run(encoder, g_contactBlur2, blur, g_contactB.view, nullptr, g_contactA.view, "Post contact shadows blur 2");
      contactView = g_contactA.view;
      if (temporal && allowProbe && viewOk && prevOk) {
        // previous view * inverse(current view); both are rigid 3x4 (rotation rows + translation).
        float inv[12];
        for (int r = 0; r < 3; ++r)
          for (int c = 0; c < 3; ++c) inv[r * 4 + c] = view[c * 4 + r];
        for (int r = 0; r < 3; ++r)
          inv[r * 4 + 3] = -(inv[r * 4 + 0] * view[3] + inv[r * 4 + 1] * view[7] + inv[r * 4 + 2] * view[11]);
        float rel[12];
        for (int r = 0; r < 3; ++r) {
          for (int c = 0; c < 4; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 3; ++k) s += prevView[r * 4 + k] * inv[k * 4 + c];
            rel[r * 4 + c] = s + (c == 3 ? prevView[r * 4 + 3] : 0.0f);
          }
        }
        Params tp = texel_params(efb);
        tp.a[2] = g_contactHistValid ? 0.85f : 0.0f;
        tp.b[0] = scaleX;
        tp.b[1] = scaleY;
        std::memcpy(tp.c, rel, 4 * sizeof(float));
        std::memcpy(tp.d, rel + 4, 4 * sizeof(float));
        std::memcpy(tp.e, rel + 8, 4 * sizeof(float));
        const uint32_t from = g_contactHistIndex, to = g_contactHistIndex ^ 1u;
        run(encoder, g_contactTemporal, tp, g_contactA.view, &g_contactHist[from].view, g_contactHist[to].view,
            "Post contact shadows temporal");
        g_contactHistIndex = to;
        g_contactHistValid = true;
        contactView = g_contactHist[to].view;
      } else if (allowProbe) {
        g_contactHistValid = false;  // no continuous history (option off, interpolation on, or a cut)
      }
      contactRan = true;
    }
  } else {
    g_contactHistValid = false;
  }
  bool exposureRan = false;
  if (m.exposure) {
    if (!g_lumTex.texture) {
      g_lumTex = make_target(1, 1, wgpu::TextureFormat::RGBA16Float, "Post exposure metering");
      g_adaptTex[0] = make_target(1, 1, wgpu::TextureFormat::RGBA16Float, "Post exposure A");
      g_adaptTex[1] = make_target(1, 1, wgpu::TextureFormat::RGBA16Float, "Post exposure B");
      g_adaptValid = false;
    }
    if (allowProbe && firstInGameFrame) {
      run(encoder, g_lumMeasure, Params{}, g_sceneColor.view, nullptr, g_lumTex.view, "Post exposure metering");
      Params ap;
      ap.a[0] = 1.0f - std::exp(-(1.0f / 60.0f) / std::max(m.exposureSpeed, 0.05f));
      ap.a[1] = g_adaptValid ? 1.0f : 0.0f;
      const uint32_t from = g_adaptIndex, to = g_adaptIndex ^ 1u;
      run(encoder, g_adapt, ap, g_lumTex.view, &g_adaptTex[from].view, g_adaptTex[to].view, "Post exposure adaptation");
      g_adaptIndex = to;
      g_adaptValid = true;
    }
    exposureRan = g_adaptValid;
  } else {
    g_adaptValid = false;
  }
  if (!contactRan && !exposureRan) return g_sceneColor.view;
  if (!g_sceneLight.pipeline || g_sceneLightFormat != g_sceneColor.format) {
    build_pass(g_sceneLight, Layout::ThreeTextures, kSceneLight, "Post modern lighting (scene)", g_sceneColor.format);
    g_sceneLightFormat = g_sceneColor.format;
  }
  if (!g_litScene.texture || g_litScene.size.width != g_sceneColor.size.width ||
      g_litScene.size.height != g_sceneColor.size.height || g_litScene.format != g_sceneColor.format) {
    g_litScene = webgpu::create_render_texture(g_sceneColor.size.width, g_sceneColor.size.height, false);
  }
  Params sp;
  sp.a[0] = contactRan ? 1.0f : 0.0f;
  sp.a[1] = exposureRan ? 1.0f : 0.0f;
  sp.a[2] = std::clamp(m.exposureRange, 0.0f, 2.0f);
  sp.a[3] = -3.3f;  // reference: log2 of a linear luminance of 0.1 (about 35 % grey), the usual room
  sp.b[0] = 0.5f;   // half of the difference is corrected, like the eye
  const wgpu::TextureView exposureView = exposureRan ? g_adaptTex[g_adaptIndex].view : g_dummyTexture.view;
  run(encoder, g_sceneLight, sp, g_sceneColor.view, contactRan ? &contactView : &g_dummyTexture.view, g_litScene.view,
      "Post modern lighting (scene)", false, &exposureView);
  return g_litScene.view;
}

bool apply_ssao_in_frame(const wgpu::CommandEncoder& encoder, bool firstInGameFrame, bool allowProbe) {
  const Config c = config();
  const bool probeWanted = g_probeWanted.load(std::memory_order_acquire);
  const bool probe = probeWanted && firstInGameFrame && allowProbe;
  g_lastApplySkip = "off";
  const bool modernActive = g_modernActive.load(std::memory_order_acquire);
  if ((!c.ssao && !c.ssr && !c.wallSsr && !c.lightShafts && !probeWanted && !modernActive) ||
      webgpu::g_graphicsConfig.msaaSamples > 1)
    return false;
  static const bool debugLog = debug_depth_requested();
  const uint32_t frame = gfx::current_frame();
  if (debugLog && frame % 30 == 0) {
    std::fprintf(stderr, "[post-ssao-efb] frame=%u depth=%d scene=%d\n", frame, g_depthAccumValid ? 1 : 0,
                 g_sceneColorValid ? 1 : 0);
  }
  if (!g_depthAccumValid || !g_depthAccum.view || !g_sceneColorValid || !g_sceneColor.view) {
    g_lastApplySkip = !g_depthAccumValid || !g_depthAccum.view ? "no-depth" : "no-scene-color";
    return false;
  }
  float projA = 0.0f, projB = 0.0f, scaleX = 1.0f, scaleY = 1.0f;
  float up[3] = {0.0f, 0.0f, 0.0f};
  bool upKnown = false;
  float lights[4][4] = {};
  uint32_t lightCount = 0;
  {
    std::lock_guard<std::mutex> lock(g_projMutex);
    if (!g_sealedScene) {
      g_lastApplySkip = "no-sealed-scene";
      return false;
    }
    projA = g_sealedA; projB = g_sealedB; scaleX = g_sealedSx; scaleY = g_sealedSy;
    lightCount = g_sealedLightCount;
    for (uint32_t i = 0; i < 4; ++i)
      for (int k = 0; k < 4; ++k) lights[i][k] = g_sealedLights[i][k];
    // The last 30 frames' static world geometry gave the up vector.
    upKnown = g_sealedUpAge <= 30;
    up[0] = g_sealedUp[0]; up[1] = g_sealedUp[1]; up[2] = g_sealedUp[2];
  }
  const auto& efb = webgpu::g_frameBuffer;
  if (!efb.texture || efb.size.width == 0 || efb.size.height == 0) {
    g_lastApplySkip = "no-efb";
    return false;
  }
  g_lastApplySkip = upKnown ? "" : "applied-without-up";
  ensure_pipelines(webgpu::g_graphicsConfig.surfaceConfiguration.format);
  if (!g_ssaoApply.pipeline || g_ssaoApplyFormat != efb.format) {
    build_pass(g_ssaoApply, Layout::FiveTextures, kSsaoApply, "Post SSAO apply (EFB)", efb.format);
    g_ssaoApplyFormat = efb.format;
  }
  if (!g_aoEfbA.texture || g_aoEfbA.size.width != efb.size.width || g_aoEfbA.size.height != efb.size.height) {
    g_aoEfbA = make_target(efb.size.width, efb.size.height, wgpu::TextureFormat::RGBA16Float, "Post SSAO EFB A");
    g_aoEfbB = make_target(efb.size.width, efb.size.height, wgpu::TextureFormat::RGBA16Float, "Post SSAO EFB B");
  }
  static const bool debugInset = debug_depth_requested();
  Params p = texel_params(efb);
  p.a[2] = c.ssaoRadius;
  p.a[3] = c.ssaoIntensity;
  p.b[0] = static_cast<float>(g_depthAccum.size.width);
  p.b[1] = static_cast<float>(g_depthAccum.size.height);
  p.b[2] = projA;
  p.b[3] = projB;
  p.c[0] = 0.0f; p.c[1] = 0.0f; p.c[2] = 1.0f; p.c[3] = 1.0f;  // the "picture" is the whole EFB
  // WII_COMPILED_POST_DEBUG_VIEW: ao = occlusion only, ny / nx = reconstructed normal components.
  static const float debugView = [] {
#if !defined(MKW_PRIME_QA_TOOLS)
    return 1.0f;
#else
    const char* v = std::getenv("WII_COMPILED_POST_DEBUG_VIEW");
    if (v == nullptr) return 1.0f;
    const std::string s(v);
    return s == "ao" ? 2.0f : s == "ny" ? 3.0f : s == "nx" ? 4.0f : s == "glow" ? 5.0f : s == "mask" ? 6.0f : 1.0f;
#endif
  }();
  p.d[0] = debugView;
  p.d[1] = debugInset ? 1.0f : 0.0f;
  p.d[2] = scaleX;
  p.d[3] = scaleY;
  p.e[0] = 0.0f; p.e[1] = 0.0f; p.e[2] = 1.0f; p.e[3] = 1.0f;  // depth covers the same EFB area
  p.f[0] = c.ssaoSamples;
  p.f[1] = c.ssaoBias * 0.01f;
  p.f[2] = c.ssaoFadeDistance;
  p.f[3] = c.ssaoPower;
  if (c.ssao) {
    run(encoder, g_ssao, p, g_sceneColor.view, &g_depthAccum.view, g_aoEfbA.view, "Post SSAO (EFB)");
    // Two depth-aware blur passes (step = softness, then half of it); separate uniform buffers so each
    // pass keeps its own step.
    Params blur = p;
    const float resScale = static_cast<float>(efb.size.height) / 1080.0f;  // softness tuned at 1080 rows
    blur.a[2] = std::max(1.0f, c.ssaoSoftness * resScale);
    blur.a[3] = c.ssaoEdgeSharpness;
    run(encoder, g_ssaoBlur, blur, g_aoEfbA.view, nullptr, g_aoEfbB.view, "Post SSAO blur 1 (EFB)");
    blur.a[2] = std::max(1.0f, c.ssaoSoftness * 0.5f * resScale);
    run(encoder, g_ssaoBlur2, blur, g_aoEfbB.view, nullptr, g_aoEfbA.view, "Post SSAO blur 2 (EFB)");
  }
  // Modern lighting: contact shadows and exposure change the snapshot the reflections and the
  // write-back read. Without them this is the snapshot itself.
  const wgpu::TextureView sceneView =
      modernActive ? modern_scene(encoder, efb, scaleX, scaleY, firstInGameFrame, allowProbe) : g_sceneColor.view;
  // Floor reflections (experimental): rays marched through the accumulated depth of this block.
  const bool floorSsr = c.ssr && c.ssrStrength > 0.0f;
  const bool wallSsr = c.wallSsr && c.wallSsrStrength > 0.0f;
  const bool ssr = (floorSsr || wallSsr) && upKnown;
  if (ssr) {
    if (!g_ssrEfb.texture || g_ssrEfb.size.width != efb.size.width || g_ssrEfb.size.height != efb.size.height)
      g_ssrEfb = make_target(efb.size.width, efb.size.height, wgpu::TextureFormat::RGBA16Float, "Post floor reflections");
    Params rp = p;
    rp.a[2] = floorSsr ? std::clamp(c.ssrStrength, 0.0f, 1.0f) : 0.0f;
    rp.e[0] = wallSsr ? std::clamp(c.wallSsrStrength, 0.0f, 1.0f) : 0.0f;
    rp.b[2] = std::clamp(c.ssrRelief, 0.0f, 2.0f);
    rp.b[3] = std::clamp(c.ssrRoughness, 0.0f, 1.0f);
    rp.a[3] = 40.0f;  // longest reflected ray (Prime units are metres)
    rp.f[0] = up[0]; rp.f[1] = up[1]; rp.f[2] = up[2]; rp.f[3] = 1.0f;
    run(encoder, g_ssr, rp, sceneView, &g_depthAccum.view, g_ssrEfb.view, "Post floor reflections (EFB)");
  }
  // Area colours for the material reflections: two pixels, copied to a free readback slot (mapped
  // after the submission; the colours arrive a few frames late and are smoothed).
  if (probe) {
    if (!g_probeTex.texture)
      g_probeTex = make_target(2, 1, wgpu::TextureFormat::RGBA8Unorm, "Post area colours", wgpu::TextureUsage::CopySrc);
    ProbeSlot* slot = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_probeMutex);
      for (auto& s : g_probeSlots)
        if (s.state == ProbeState::Available) { slot = &s; break; }
      if (slot != nullptr && !slot->buffer) {
        const wgpu::BufferDescriptor bd{.label = "Post area colours readback",
                                        .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst, .size = 256};
        slot->buffer = webgpu::g_device.CreateBuffer(&bd);
      }
    }
    if (slot != nullptr) {
      run(encoder, g_areaProbe, Params{}, g_sceneColor.view, nullptr, g_probeTex.view, "Post area colours");
      const wgpu::TexelCopyTextureInfo from{.texture = g_probeTex.texture};
      const wgpu::TexelCopyBufferInfo to{.layout = {.offset = 0, .bytesPerRow = 256, .rowsPerImage = 1}, .buffer = slot->buffer};
      const wgpu::Extent3D size{2, 1, 1};
      encoder.CopyTextureToBuffer(&from, &to, &size);
      std::lock_guard<std::mutex> lock(g_probeMutex);
      slot->state = ProbeState::CopySubmitted;
    }
  }
  // Light shafts from the lights of the frame that are in front of the camera and near the screen.
  bool shafts = false;
  if (c.lightShafts && firstInGameFrame && lightCount > 0 && c.lightShaftsIntensity > 0.0f) {
    Params sp{};
    float uv[4][2] = {};
    float weight[4] = {};
    uint32_t used = 0;
    for (uint32_t i = 0; i < lightCount && i < 4; ++i) {
      const float z = -lights[i][2];  // GX view space looks down -z
      if (z <= 0.1f) continue;
      const float nx = lights[i][0] * scaleX / z, ny = lights[i][1] * scaleY / z;
      if (std::abs(nx) > 1.6f || std::abs(ny) > 1.6f) continue;
      uv[used][0] = nx * 0.5f + 0.5f;
      uv[used][1] = 0.5f - ny * 0.5f;
      weight[used] = std::clamp(lights[i][3], 0.0f, 1.0f);
      ++used;
    }
    if (used > 0) {
      const uint32_t qw = std::max<uint32_t>(efb.size.width / 4, 1), qh = std::max<uint32_t>(efb.size.height / 4, 1);
      if (!g_shaftsTex.texture || g_shaftsTex.size.width != qw || g_shaftsTex.size.height != qh)
        g_shaftsTex = make_target(qw, qh, wgpu::TextureFormat::RGBA16Float, "Post light shafts");
      sp.a[2] = std::clamp(c.lightShaftsIntensity, 0.0f, 2.0f) * 0.35f;
      sp.a[3] = static_cast<float>(used);
      sp.b[0] = uv[0][0]; sp.b[1] = uv[0][1]; sp.b[2] = uv[1][0]; sp.b[3] = uv[1][1];
      sp.c[0] = uv[2][0]; sp.c[1] = uv[2][1]; sp.c[2] = uv[3][0]; sp.c[3] = uv[3][1];
      sp.d[0] = static_cast<float>(efb.size.width) / static_cast<float>(std::max<uint32_t>(efb.size.height, 1));
      sp.e[0] = weight[0]; sp.e[1] = weight[1]; sp.e[2] = weight[2]; sp.e[3] = weight[3];
      sp.f[0] = 0.94f;  // decay per step
      sp.f[1] = 0.85f;  // density: fraction of the way to the light covered by the 24 steps
      sp.f[2] = 0.72f;  // brightness threshold
      sp.f[3] = 0.6f;   // weight of glowing surfaces (EFB alpha)
      run(encoder, g_shafts, sp, g_sceneColor.view, nullptr, g_shaftsTex.view, "Post light shafts");
      shafts = true;
    }
  }
  // The snapshot holds the world colour; the result overwrites the whole EFB colour.
  Params applyParams = p;
  applyParams.e[0] = c.ssaoGlowProtect;
  const bool masked = (c.ssaoSkipMarked || c.ssaoUnderEffects) && g_faceMaskDirty && g_faceMask.view;
  applyParams.e[1] = masked ? 1.0f : 0.0f;
  applyParams.f[0] = c.ssao ? 1.0f : 0.0f;
  applyParams.f[1] = ssr ? 1.0f : 0.0f;
  applyParams.f[2] = c.specOcclusion ? 1.0f : 0.0f;
  applyParams.f[3] = shafts ? 1.0f : 0.0f;
  if (!g_dummyTexture.texture) g_dummyTexture = make_target(1, 1, wgpu::TextureFormat::R16Float, "Post dummy source");
  if (debugLog && frame % 30 == 0)
    std::fprintf(stderr, "[post-ssao-exclude] frame=%u runs=%u masked=%d\n", frame, g_faceRuns, masked ? 1 : 0);
  g_faceRuns = 0;
  run(encoder, g_ssaoApply, applyParams, sceneView, c.ssao ? &g_aoEfbA.view : &g_dummyTexture.view, efb.view,
      "Post SSAO apply (EFB)", false, masked ? &g_faceMask.view : &g_dummyTexture.view,
      ssr ? &g_ssrEfb.view : &g_dummyTexture.view, shafts ? &g_shaftsTex.view : &g_dummyTexture.view);
  return true;
}

bool face_depth_pending() { return g_faceDepthPending; }

void soften_copy(const wgpu::CommandEncoder& encoder, const TextureRef& target, float scaleX, float scaleY) {
  if (!config().glowFix) return;
  if (scaleX <= 1.05f && scaleY <= 1.05f) return;
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  static uint32_t logged = 0;
  if (target.format != format || !target.attachmentTextureView || !target.sampleTextureView ||
      target.size.width == 0 || target.size.height == 0) {
#if defined(MKW_PRIME_QA_TOOLS)
    if (logged < 4) {
      ++logged;
      Log.info("[glow-soften] skipped copy {}x{} format {} (render format {})", target.size.width, target.size.height,
               static_cast<uint32_t>(target.format), static_cast<uint32_t>(format));
    }
#endif
    return;
  }
#if defined(MKW_PRIME_QA_TOOLS)
  if (logged < 4) {
    ++logged;
    Log.info("[glow-soften] glow source copy {}x{} softened by {:.2f} x {:.2f} texels", target.size.width,
             target.size.height, scaleX, scaleY);
  }
#endif
  ensure_pipelines(format);
  static webgpu::TextureWithSampler temp;
  if (!temp.texture || temp.size.width != target.size.width || temp.size.height != target.size.height ||
      temp.format != format) {
    temp = make_target(target.size.width, target.size.height, format, "Glow copy soften temp");
  }
  Params h;
  h.a[2] = std::max(scaleX - 1.0f, 0.0f) + 1.0f;  // tent over one Wii texel (+ its bilinear neighbour)
  h.b[0] = 1.0f / static_cast<float>(target.size.width);
  run(encoder, g_copySoftH, h, target.sampleTextureView, nullptr, temp.view, "Glow copy soften H");
  Params v;
  v.a[2] = std::max(scaleY - 1.0f, 0.0f) + 1.0f;
  v.b[1] = 1.0f / static_cast<float>(target.size.height);
  run(encoder, g_copySoftV, v, temp.view, nullptr, target.attachmentTextureView, "Glow copy soften V");
}

void set_depth_carry(bool carry, uint32_t draws) {
  g_depthCarry = carry;
  g_carryDraws = carry ? draws : 0;
}

uint32_t carried_world_draws() { return g_depthCarry ? g_carryDraws : 0; }

void reset_depth(const wgpu::CommandEncoder& encoder) {
  clear_face_mask(encoder);
  if (!g_depthAccumValid || !g_depthAccum.view) return;
  const std::array attachments{wgpu::RenderPassColorAttachment{
      .view = g_depthAccum.view, .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store,
      .clearValue = {0.0, 0.0, 0.0, 1.0}}};
  const wgpu::RenderPassDescriptor descriptor{
      .label = "Post depth accumulation reset (next block)", .colorAttachmentCount = attachments.size(),
      .colorAttachments = attachments.data()};
  encoder.BeginRenderPass(&descriptor).End();
}

void apply(const wgpu::CommandEncoder& encoder, const webgpu::TextureWithSampler& image,
           float viewportLeft, float viewportTop, float viewportWidth, float viewportHeight) {
  const Config c = config();
  if (!(c.sharpen || c.fxaa || c.bloom || c.ssao || c.color)) { g_statusNote = ""; return; }
  if (!image.texture || image.size.width == 0 || image.size.height == 0) return;
  static const bool debugLog = debug_depth_requested();
  const char* previousNote = g_statusNote;
  ensure_pipelines(image.format);
  ensure_scratch(image.size.width, image.size.height);

  const float w = static_cast<float>(image.size.width), h = static_cast<float>(image.size.height);
  const float vp[4] = {viewportLeft / w, viewportTop / h, std::max(viewportWidth, 1.0f) / w,
                       std::max(viewportHeight, 1.0f) / h};

  // Ping-pong between the presentation image and the full-size scratch texture.
  const webgpu::TextureWithSampler* current = &image;
  const webgpu::TextureWithSampler* other = &g_scratch.full;
  const auto swap = [&] { std::swap(current, other); };

  g_statusNote = "";
  if (!scene_active()) g_sceneColorValid = false;  // menu, map or video: the snapshot is stale
  if (c.ssao) {
    // SSAO itself runs inside the frame (apply_ssao_in_frame); only the status note is set here.
    bool sceneThisFrame = false;
    {
      std::lock_guard<std::mutex> lock(g_projMutex);
      sceneThisFrame = g_sealedScene;
    }
    if (webgpu::g_graphicsConfig.msaaSamples > 1) {
      g_statusNote = "SSAO needs antialiasing (MSAA) off; restart after changing it.";
    } else if (!g_depthAccum.view || !g_depthCaptured.load(std::memory_order_acquire)) {
      g_statusNote = "SSAO: waiting for a frame with depth.";
    } else if (!sceneThisFrame) {
      g_statusNote = "SSAO: no 3D scene in this frame (menu or video), skipped.";
    }
  }
  if (c.bloom) {
    Params bright = texel_params(*current);
    bright.a[2] = c.bloomThreshold;
    const bool fromScene = c.bloomWorldOnly && g_sceneColorValid && g_sceneColor.view;
    if (fromScene) {
      bright = texel_params(g_sceneColor);
      bright.a[2] = c.bloomThreshold;
      bright.b[0] = 1.0f;
      std::memcpy(bright.c, vp, sizeof(vp));
      display_rect(bright.e, static_cast<float>(g_sceneColor.size.width), static_cast<float>(g_sceneColor.size.height));
    }
    run(encoder, g_bright, bright, fromScene ? g_sceneColor.view : current->view, nullptr, g_scratch.quarterA.view,
        "Post bloom bright");
    Params blur = texel_params(g_scratch.quarterA);
    blur.a[2] = c.bloomRadius; blur.a[3] = 0.0f;
    run(encoder, g_blurH, blur, g_scratch.quarterA.view, nullptr, g_scratch.quarterB.view, "Post bloom blur H");
    blur.a[2] = 0.0f; blur.a[3] = c.bloomRadius;
    run(encoder, g_blurV, blur, g_scratch.quarterB.view, nullptr, g_scratch.quarterA.view, "Post bloom blur V");
    Params composite = texel_params(*current);
    composite.a[2] = c.bloomIntensity;
    run(encoder, g_composite, composite, current->view, &g_scratch.quarterA.view, other->view, "Post bloom composite");
    swap();
  }
  if (c.sharpen) {
    Params p = texel_params(*current);
    p.a[2] = c.sharpenAmount;
    run(encoder, g_sharpen, p, current->view, nullptr, other->view, "Post sharpen");
    swap();
  }
  if (c.fxaa) {
    Params p = texel_params(*current);
    run(encoder, g_fxaa, p, current->view, nullptr, other->view, "Post FXAA");
    swap();
  }
  if (c.color) {
    Params p = texel_params(*current);
    p.a[2] = c.brightness; p.a[3] = c.contrast;
    p.b[0] = c.saturation; p.b[1] = c.gamma; p.b[2] = c.temperature; p.b[3] = c.vignette;
    std::memcpy(p.c, vp, sizeof(vp));
    run(encoder, g_color, p, current->view, nullptr, other->view, "Post colour");
    swap();
  }
  if (debugLog && g_statusNote != previousNote && *g_statusNote) {
    std::fprintf(stderr, "[post-ssao-skip] frame=%u %s\n", gfx::current_frame(), g_statusNote);
  }
  if (current != &image) {
    // Odd number of passes: copy the result back into the presentation image.
    const auto bindGroup = webgpu::create_copy_bind_group(current->view, g_linearClamp);
    const std::array attachments{wgpu::RenderPassColorAttachment{
        .view = image.view, .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store}};
    const wgpu::RenderPassDescriptor descriptor{
        .label = "Post copy back", .colorAttachmentCount = attachments.size(), .colorAttachments = attachments.data()};
    const auto pass = encoder.BeginRenderPass(&descriptor);
    pass.SetPipeline(webgpu::g_CopyPipeline);
    pass.SetBindGroup(0, bindGroup, 0, nullptr);
    pass.Draw(3);
    pass.End();
  }
}

void shutdown() {
  g_sharpen = g_fxaa = g_bright = g_blurH = g_blurV = g_composite = g_ssao = g_ssaoBlur = g_ssaoBlur2 = g_ssaoApply =
      g_color = g_depthMerge = g_faceSnapshot = g_faceDiff = g_faceFinalize = g_coverDiff = g_effectDiff = Pass{};
  g_coverBefore = {};
  g_coverFormat = wgpu::TextureFormat::Undefined;
  g_faceBefore = {};
  g_faceDepth = {};
  g_faceMask = {};
  g_faceDepthPending = false;
  g_faceMaskDirty = false;
  g_ssaoApplyFormat = wgpu::TextureFormat::Undefined;
  g_aoEfbA = {};
  g_aoEfbB = {};
  g_linearClamp = {};
  g_scratch = Scratch{};
  g_depthAccum = {};
  g_depthMergeTemp = {};
  g_dummyTexture = {};
  g_depthAccumValid = false;
  g_depthCaptured.store(false);
  g_sceneColor = {};
  g_sceneColorValid = false;
  g_builtFormat = wgpu::TextureFormat::Undefined;
  g_contact = g_contactBlur1 = g_contactBlur2 = g_contactTemporal = g_lumMeasure = g_adapt = g_sceneLight = Pass{};
  g_sceneLightFormat = wgpu::TextureFormat::Undefined;
  g_contactA = {};
  g_contactB = {};
  g_contactHist[0] = {};
  g_contactHist[1] = {};
  g_contactHistValid = false;
  g_lightsTex = {};
  g_lumTex = {};
  g_adaptTex[0] = {};
  g_adaptTex[1] = {};
  g_adaptValid = false;
  g_litScene = {};
}

} // namespace aurora::gfx::post

#pragma once
namespace aurora::gx {
// GX viewport origins include the raster coordinate bias, like BP scissor.
inline constexpr float XfViewportOriginBias=342.0f;
inline constexpr float xf_viewport_edge(float origin,float extent) {
  return origin-XfViewportOriginBias-extent*0.5f;
}
inline constexpr float xf_viewport_origin(float edge,float extent) {
  return edge+extent*0.5f+XfViewportOriginBias;
}
}

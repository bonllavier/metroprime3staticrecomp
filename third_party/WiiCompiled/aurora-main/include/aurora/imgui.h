#ifndef AURORA_IMGUI_H
#define AURORA_IMGUI_H

#include <imgui.h>

#ifdef __cplusplus
#include <cstdint>

extern "C" {
#else
#include "stdint.h"
#endif

ImTextureID aurora_imgui_add_texture(uint32_t width, uint32_t height, const void* rgba8);
// Rebuild the ImGui font atlas before the next ImGui frame: the atlas is cleared, the config's
// imGuiInitCallback runs again to add the fonts (and may change the style), and the backend uploads
// the new texture. Callable from any thread; the work happens where ImGui frames start.
void aurora_imgui_request_font_rebuild(void);

#ifdef __cplusplus
}
#endif

#endif

#pragma once

#include "dolphin/gx/GXEnum.h"
#include "../internal.hpp"

namespace aurora::gx::fifo {

// Reset the CP write-deduplication state whenever the GX shadow state is reinitialized.
void reset_cp_register_cache();

// Process a buffer of GX FIFO commands
uint32_t process(const uint8_t* data, uint32_t size, bool bigEndian);

// GX_LOAD_AURORA_HOST_CALLBACK support (see aurora_stream_host_callback).
void set_host_callback(void (*callback)(uint32_t, const uint8_t*, uint32_t));
void invoke_host_callback(uint32_t id, const uint8_t* data, uint32_t size);
// Runs the callback process() stopped at, outside the renderer lock. False when none.
bool dispatch_pending_host_callback();
// GX_LOAD_AURORA_DEFERRED_DRAW support (see aurora_stream_deferred_draw).
using DeferredDrawExpander = const uint8_t* (*)(const uint8_t* payload, uint32_t size, uint32_t* wireBytes);
void set_deferred_draw_expander(DeferredDrawExpander expander);
bool has_deferred_draw_expander();
// Submit already-packed direct vertex bytes against the current GX state.
bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes);

} // namespace aurora::gx::fifo

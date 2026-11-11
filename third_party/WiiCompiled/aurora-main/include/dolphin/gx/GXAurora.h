#ifndef DOLPHIN_GXAURORA_H
#define DOLPHIN_GXAURORA_H

#include <dolphin/types.h>

#if __cplusplus
extern "C" {
#endif

//
// Subcommands for GX_LOAD_AURORA.
//

/**
 * Sets the actual render viewport in native framebuffer coordinates.
 * Must be followed by six f32 values: left, top, width, height, nearz, farz.
 */
#define GX_LOAD_AURORA_VIEWPORT_RENDER 0x0001

/**
 * Sets the actual render scissor in native framebuffer coordinates.
 * Must be followed by four u32 values: left, top, width, height.
 */
#define GX_LOAD_AURORA_SCISSOR_RENDER 0x0002

/**
 * Aurora equivalent of CP_REG_ARRAYBASE_ID: sets the base address and size of a vertex array.
 * This command must be followed by a 64-bit memory address, 32-bit size, and 1-byte little-endian flag.
 * The index of the vertex array is given by the lowest 4 bits of the command ID,
 * e.g. writing GX_LOAD_AURORA_ARRAYBASE + 5 will set the vertex array for the sixth vertex attribute.
 * To set strides, use the normal CP_REG_ARRAYSTRIDE_ID register.
 */
#define GX_LOAD_AURORA_ARRAYBASE 0x0010

/**
 * Pushes a debug group to the backend graphics API. These may show in debugging tools such as RenderDoc.
 * Must be followed by a u16 string length and that many UTF-8 characters (no null terminator required).
 * It is considered an error to have unpopped debug groups at the end of the frame. They will be automatically cleared.
 */
#define GX_LOAD_AURORA_DEBUG_GROUP_PUSH 0x0020

/**
 * Pops a previously pushed debug group.
 * Followed by nothing.
 */
#define GX_LOAD_AURORA_DEBUG_GROUP_POP 0x0021

/**
 * Sends a debug marker to the backend graphics API.
 * Must be followed by a u16 string length and that many UTF-8 characters (no null terminator required).
 */
#define GX_LOAD_AURORA_DEBUG_MARKER_INSERT 0x0022

#define GX_LOAD_AURORA_TEXOBJ 0x0030

#define GX_LOAD_AURORA_TLUT 0x0031

#define GX_LOAD_AURORA_DESTROY_TEXOBJ 0x0032

#define GX_LOAD_AURORA_DESTROY_TLUT 0x0033

#define GX_LOAD_AURORA_DESTROY_COPY_TEX 0x0034

#define GX_LOAD_AURORA_INVALIDATE_TEX_ALL 0x0035

/**
 * Host callback in GX stream order (aurora_stream_host_callback): u32 id,
 * u32 size, then `size` payload bytes. The decoder runs the registered
 * AuroraStreamCallback outside the renderer lock, after every earlier command.
 */
#define GX_LOAD_AURORA_HOST_CALLBACK 0x0040

/**
 * Deferred draw (aurora_stream_deferred_draw): u32 size, then `size` payload
 * bytes the host's expander (aurora_set_deferred_draw_expander) turns into one
 * draw command (opcode|fmt, u16 count, vertices) on the decoder thread, inline,
 * so the draw merges and batches like any other.
 */
#define GX_LOAD_AURORA_DEFERRED_DRAW 0x0041

/**
 * Vertex/XF array by value (GXSetArrayCopy): like GX_LOAD_AURORA_ARRAYBASE + n, but followed by a u32
 * size, a 1-byte little-endian flag and `size` bytes of array data. The decoder keeps its own copy, so
 * the draws see the array as it was when the command was written, whatever the game writes later.
 */
#define GX_LOAD_AURORA_ARRAYDATA 0x0050


/*
 * Debug marker stuff
 */

/**
 * Pushes a debug group to the backend graphics API. These may show in debugging tools such as RenderDoc.
 * It is considered an error to have unpopped debug groups at the end of the frame. They will be automatically cleared.
 */
void GXPushDebugGroup(const char* label);

/**
 * Pop a debug group previously pushed via GXPushDebugGroup().
 */
void GXPopDebugGroup();

/**
 * Sends a debug marker to the backend graphics API. These may show in debugging tools such as RenderDoc.
 */
void GXInsertDebugMarker(const char* label);

typedef enum _AuroraViewportPolicy {
  AURORA_VIEWPORT_FIT = 0,     // Preserve logical aspect in the content framebuffer
  AURORA_VIEWPORT_STRETCH = 1, // Match content framebuffer aspect to the native surface
  AURORA_VIEWPORT_NATIVE = 2,  // Use active framebuffer pixels directly
  AURORA_VIEWPORT_16_9 = 3,    // Fixed 16:9 content with bars on other surfaces
} AuroraViewportPolicy;

/**
 * Configures content framebuffer sizing and how GXSetViewport/GXSetScissor parameters are applied to rendering.
 * When AURORA_VIEWPORT_NATIVE is used, GXSetTexCopySrc/GXSetTexCopyDst will use native framebuffer resolution.
 */
void AuroraSetViewportPolicy(AuroraViewportPolicy policy);
/**
 * Shape of the AURORA_VIEWPORT_16_9 policy's content (16:9 by default). A wider value renders and
 * presents a wider image, pillarboxed on wider surfaces (used for ultrawide).
 */
void AuroraSetFixedAspect(float aspect);

/**
 * Retrieves the current content framebuffer size.
 */
void AuroraGetRenderSize(u32* width, u32* height);

/** Retrieves the current native presentation-surface size, ignoring the GX sizing policy. */
void AuroraGetSurfaceSize(u32* width, u32* height);

/**
 * Sets the actual render viewport in native framebuffer coordinates.
 * Overrides the automatically scaled values set by the logical GXSetViewport.
 */
void GXSetViewportRender(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz);

/**
 * Sets the actual render scissor in native framebuffer coordinates.
 * Overrides the automatically scaled values set by the logical GXSetScissor.
 */
void GXSetScissorRender(u32 left, u32 top, u32 wd, u32 ht);

/** Maps the logical GX viewport and scissor into a centered safe area, for fixed-aspect UI bridges. */
void GXSetViewportScissorRenderSafeArea(f32 aspect);

/** Restores the viewport and scissor from logical GX state after GXSetViewportScissorRenderSafeArea. */
void GXRestoreViewportScissorRender(void);

/** Sets the texture-copy source in active render-target coordinates, for bridges that already
 *  resolved VI/window scaling. */
void GXSetTexCopySrcRender(u16 left, u16 top, u16 wd, u16 ht);

/**
 * Create an offscreen framebuffer and switch rendering to it.
 * All subsequent GX rendering will target this framebuffer until GXRestoreFrameBuffer() is called.
 * Use GXCopyTex to resolve the offscreen content into a texture.
 */
void GXCreateFrameBuffer(u32 width, u32 height);

/**
 * Restore rendering to the main EFB framebuffer.
 * Must be called after GXCreateFrameBuffer() to resume normal rendering.
 */
void GXRestoreFrameBuffer(void);

void GXApplyBPReg(u8 reg, u32 value);

/**
 * Counter advanced whenever the SDK vertex descriptor/attribute-format shadow
 * or the decoder's vertex state may have changed. Callers that republish
 * vertex state can skip an identical republish while it is unchanged.
 */
u64 GXAuroraVtxStateEpoch(void);

/**
 * Writer: flush the SDK shadow's dirty state (vertex descriptors, attribute
 * formats, BP masks) into the stream now. Required before a host callback
 * whose decoder-side handler reads vertex state (aurora_stream_host_callback).
 */
void GXAuroraPublishDirtyState(void);

#if __cplusplus
}
#endif

#endif

#include "gx.hpp"
#include "__gx.h"

#include "../../gx/command_processor.hpp"
#include "../../gx/fifo.hpp"

#include <cstring>

static __GXData_struct sSavedGXData;
// Defined in GXGeometry.cpp; see GXAuroraVtxStateEpoch.
extern "C" u64 g_gxVtxStateEpoch;

extern "C" {
void GXBeginDisplayList(void* list, u32 size) {
  CHECK(!aurora::gx::fifo::in_display_list(), "Display list began twice!");

  // Flush any pending dirty state before recording
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // Save current shadow register state if requested
  if (__gx->dlSaveContext != 0) {
    std::memcpy(&sSavedGXData, __gx, sizeof(sSavedGXData));
  }

  __gx->inDispList = 1;

  // Redirect FIFO writes to the user-provided buffer
  aurora::gx::fifo::begin_display_list(static_cast<u8*>(list), size);
}

u32 GXEndDisplayList() {
  // Flush any pending dirty state into the display list
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // End FIFO redirection and get the byte count (ROUNDUP32)
  u32 bytesWritten = aurora::gx::fifo::end_display_list();

  // Restore saved shadow register state
  if (__gx->dlSaveContext != 0) {
    std::memcpy(__gx, &sSavedGXData, sizeof(*__gx));
    ++g_gxVtxStateEpoch;
    // The decoder keeps what the recording emitted; the shadow went back.
    aurora::gx::fifo::mark_vertex_cp_unknown();
  }

  __gx->inDispList = 0;

  return bytesWritten;
}

void GXCallDisplayList(const void* data, u32 nbytes) {
  // Flush any pending dirty state before calling
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // Flush pending primitives
  if (*reinterpret_cast<u32*>(&__gx->vNum) != 0) {
    __GXSendFlushPrim();
  }

  // Nested display-list calls add bytes instead of changing live renderer state.
  if (aurora::gx::fifo::in_display_list()) {
    aurora::gx::fifo::write_data(data, nbytes);
    return;
  }

  // CP loads inside the list change the decoder's vertex registers behind the
  // shadow: GXSetVtxDesc/GXSetVtxAttrFmt then re-emit, and the vertex-state
  // epoch moves here, where the synchronous decoder would have moved it.
  if (aurora::gx::fifo::note_display_list_called(static_cast<const u8*>(data), nbytes)) {
    ++g_gxVtxStateEpoch;
  }

  // With the asynchronous decoder the bytes are copied into the stream, in
  // order behind everything already written, and decoded by that thread.
  if (aurora::gx::fifo::async_enabled() && !aurora::gx::fifo::on_decoder_thread()) {
    aurora::gx::fifo::write_data(data, nbytes);
    aurora::gx::fifo::commit();
    return;
  }
  // Decode the display list immediately while its borrowed resources are valid.
  aurora::gx::fifo::drain();
  aurora::gx::fifo::process_all(static_cast<const u8*>(data), nbytes, true);
}

void GXCallDisplayListLE(const void* data, u32 nbytes) {
  // Flush any pending dirty state before calling
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // Flush pending primitives
  if (*reinterpret_cast<u32*>(&__gx->vNum) != 0) {
    __GXSendFlushPrim();
  }

  // Decode little-endian lists separately after finishing the normal FIFO work.
  aurora::gx::fifo::mark_vertex_cp_unknown();
  ++g_gxVtxStateEpoch;
  aurora::gx::fifo::drain();

  // Process the display list through the command processor (little-endian)
  aurora::gx::fifo::process_all(static_cast<const u8*>(data), nbytes, false);
}
}

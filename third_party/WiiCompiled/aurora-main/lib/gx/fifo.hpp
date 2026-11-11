#pragma once

#include "../internal.hpp"

#include <cstring>

namespace aurora::gx::fifo {

namespace detail {
extern uint8_t* sBufferData;
extern uint32_t sBufferSize;
extern uint32_t sBufferCapacity;
extern bool sInDisplayList;
extern uint8_t* sDlBuffer;
extern uint32_t sDlSize;
extern uint32_t sDlWritePos;
} // namespace detail

void init();

// Out-of-line slow path: grows internal buffer then appends data
void write_data_grow(const void* data, uint32_t length);
// Bulk SDK vertex payload; retains the same capacity and DL-recording checks
// as the inline scalar writers rather than forcing a growth on every packet.
void write_data_bulk(const void* data,uint32_t length);

inline void write_data(const void* data, const uint32_t length) {
  if (!detail::sInDisplayList)
    LIKELY {
      if (detail::sBufferSize + length <= detail::sBufferCapacity)
        LIKELY {
          std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
          detail::sBufferSize += length;
          return;
        }
      write_data_grow(data, length);
    }
  else if (detail::sDlWritePos + length <= detail::sDlSize) {
    std::memcpy(detail::sDlBuffer + detail::sDlWritePos, data, length);
    detail::sDlWritePos += length;
  }
}

inline void write_u8(const uint8_t val) {
  if (!detail::sInDisplayList)
    LIKELY {
      if (detail::sBufferSize < detail::sBufferCapacity)
        LIKELY {
          detail::sBufferData[detail::sBufferSize++] = val;
          return;
        }
      write_data_grow(&val, 1);
    }
  else if (detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = val;
  }
}

inline void write_u16(const uint16_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u32(const uint32_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u64(const uint64_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_f32(const float val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

// Display list recording
void begin_display_list(uint8_t* buf, uint32_t size);
uint32_t end_display_list();
bool in_display_list();

// Drain the internal FIFO buffer through the command processor. With the
// asynchronous decoder this is sync(): callers that then read or mutate
// decoder state keep their barrier (slower but safe); hot paths use commit().
void drain();

// Asynchronous decoder (aurora_set_async_decoder): the writer thread only
// appends GX wire commands; a decoder thread runs process_all over chunks in
// order. Everything that reads or mutates decoder state must either run on the
// decoder thread (stream commands, host callbacks) or sync() first.
bool async_enabled();
void set_async_enabled(bool enabled);
bool on_decoder_thread();
// Writer, at a command boundary: hands the buffer to the decoder when it is
// large enough, or as soon as the decoder is idle and it holds a few KiB.
void commit();
// Writer, at a command boundary: hands the buffer to the decoder now.
void hand_off();
// SDK immediate-mode writer is between GXBegin and GXEnd (GXVert.cpp).
bool in_begin();
// Writer: lock-free count of chunks the decoder has queued or in progress.
uint32_t async_backlog_hint();

// Writer-side knowledge of the decoder's vertex CP state (VCD 0x50/0x60, VAT
// 0x70..0x97). The SDK emits those registers from its shadow, so decoder and
// shadow agree unless raw display-list bytes loaded CP registers behind the
// shadow's back; these flags remember that until the next emission. They
// replace reading g_gxState from the writer, which only the synchronous
// decoder made valid.
bool vertex_cp_unknown_vcd();
bool vertex_cp_unknown_vat(uint8_t fmt);
void note_vcd_emitted();
void note_vat_emitted(uint8_t fmt);
void mark_vertex_cp_unknown();
// Writer, before raw display-list bytes enter the stream: marks the vertex CP
// state unknown when the list loads CP registers 0x50..0x9F, or when it cannot
// be walked to its end (draws need vertex sizes). Returns whether it marked.
bool note_display_list_called(const uint8_t* data, uint32_t nbytes);
// Writer: hand_off() then wait until the decoder has consumed every chunk
// and the frame worker has sealed the previous frame.
void sync();
// Nothing queued for the decoder and nothing being decoded.
bool async_idle();
uint32_t decoder_native_thread_id();
void shutdown_async();

// Decode a whole command stream, submitting the staging batch whenever a draw does not fit.
// process() alone stops at that draw, so a caller that ignores its count drops the rest.
void process_all(const uint8_t* data, uint32_t size, bool bigEndian);

// Internal buffer inspection
const uint8_t* get_buffer_data();
uint32_t get_buffer_size();
void clear_buffer();
// PI FIFO reset: discard the immediate-mode writer without validating or
// submitting its incomplete GXBegin packet. Does not signal draw completion.
void abort_vertex_writer();

} // namespace aurora::gx::fifo

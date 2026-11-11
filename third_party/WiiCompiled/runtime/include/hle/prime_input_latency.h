#pragma once
#include <cstdint>

// Input-to-display latency diagnostics for the Prime remote pointer. Logs the
// average and worst time from the KPAD read whose state a frame used to (1) the
// GX consumer decoding that frame's XFB copy, (2) VI handing it to Aurora and
// (3) the surface Present. Observation only; nothing is paced by it.
namespace PrimeInputLatency {
int64_t NowNanos();
// Guest thread, at every published KPAD sample.
void NoteInputRead();
// GX consumer: submit time of the burst being decoded (0 when decoded inline).
void SetDecodingSubmitTime(int64_t nanos);
int64_t DecodingSubmitTime();
// Whoever decodes the XFB copy command.
void NoteXfbCopy();
// Guest thread, when VI presents a new XFB.
void NoteHandoff();
}

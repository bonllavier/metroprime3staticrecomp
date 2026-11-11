#pragma once

#include <cstdint>

struct CpuContext;

namespace AxDspHle {

void Init();
void InitForAXOut(CpuContext* ctx);
// Raw hardware-mailbox entry: no MKW task globals or synthetic callbacks.
// Currently accepts only the microcode fingerprint observed in RM3E01_00.
void InitPrimeMicrocode(uint32_t crc);
void Stop();

uint32_t CheckInit();
uint32_t AddTask(uint32_t taskPtr);
void SendMailToDSP(uint32_t mail);
uint32_t CheckMailToDSP();
uint32_t CheckMailFromDSP();
uint32_t ReadMailFromDSP();
uint32_t AssertTask(uint32_t taskPtr);
void ServiceDeferredCallbacks();

// INVARIANT: no guest code may observe mix output before this returns. Called from
// Audio_HLE_Tick before the AI DMA block reaches the backend and before the AI callback
// runs __AXOutNewFrame, the two places guest code reads the mix's PB/output/aux data.
// Cheap when the mix already finished (the normal case).
void JoinMixWorker();
// Joins only a pending or running mix that writes PCM into [addr, addr + bytes):
// the audio DMA reads guest PCM and must never see a frame the worker is writing.
// Guest code reads it too: the movie player's AI callback mixes THP audio into the
// output buffer __AXOutNewFrame has just started, after DCInvalidateRange, whose
// runtime overlay (PrimeBeforeGuestDcInvalidate) calls this.
void JoinMixWorkerIfWriting(uint32_t addr, uint32_t bytes);
// Prime (raw mailbox): called on the mix worker once a command list has finished
// and its DSP->CPU mails are available, so the caller can raise the mailbox IRQ.
void SetMixCompletionHook(void (*hook)());

// Joins and tears down the mix worker thread. Call before the guest memory map
// is destroyed or re-created.
void ShutdownMixWorker();

void SetMixWorkerEnabled(bool enabled);

void InitAram();

}

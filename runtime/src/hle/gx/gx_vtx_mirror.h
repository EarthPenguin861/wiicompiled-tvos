#pragma once

#include "gx_internal.h"

#include <cstdint>

// Game-thread mirror of the vertex descriptor (VCD) and vertex attribute format (VAT) state.
//
// With the GX worker active, the game thread records GX calls and the worker applies them later,
// so g_hleGxState describes the past from the game thread's point of view. GXGetVtxDesc and
// GXGetVtxAttrFmt therefore used to Sync() with the worker before answering, and
// nw4r::ef::DrawStrategyImpl::SetupGP asks each of them three times for every particle emitter it
// draws: hundreds of full game-thread/worker round trips per race frame, and the largest part of
// the game thread's waiting on a 2-core Apple TV.
//
// The mirror applies every recorded change of the vertex layout when it is recorded, so those
// queries are answered without waiting:
//   - GXSetVtxDesc, GXClearVtxDesc, GXSetVtxDescv, GXSetVtxAttrFmt and GXSetVtxAttrFmtv, with the
//     worker's own rules;
//   - gather-pipe bytes, parsed packet by packet exactly like HleFifoWrite: CP VCD/VAT loads go
//     through the same decoder (gx_cp_decode.h), every other packet and all vertex data are skipped
//     by their exact length;
//   - GXBegin/GXEnd, whose vertex data is skipped by its exact length.
// Anything it cannot follow exactly (a display list call, the debug shape helpers, GX work run
// inline on the game thread, an incomplete packet at a sync point) makes it invalid. The next query
// then syncs as before and reloads the mirror from the drained worker's state.
//
// Every answer is also verified: the query records a probe that the worker evaluates in stream
// order against its real state. A mismatch is logged once and turns the mirror off for the rest of
// the run. MKW_GX_VTX_MIRROR=0 in the environment turns it off from the start.
//
// All functions run on the game thread, and only on the recording path (GxWorker::ShouldDefer()).
namespace GxVtxMirror {

// True when a query may be answered from the mirror.
bool Usable() noexcept;
GXAttrType Desc(uint32_t attr) noexcept;
VtxAttrFmt Fmt(uint32_t vtxFmt, uint32_t attr) noexcept;

void Invalidate() noexcept;
// Copies the worker's state. Only valid right after a Sync, inside an inline scope.
void ReloadFromWorkerState() noexcept;

void OnSetVtxDesc(uint32_t attr, uint32_t type) noexcept;
void OnClearVtxDesc() noexcept;
void OnSetVtxAttrFmt(uint32_t vtxFmt, uint32_t attr, uint32_t cnt, uint32_t type, uint32_t frac) noexcept;
void OnBegin(uint32_t vtxFmt, uint32_t vertexCount) noexcept;
void OnEnd() noexcept;
void OnFifoWrite(uint32_t value, uint32_t sizeBytes) noexcept;
void OnFifoBurst(const uint8_t* data, uint32_t sizeBytes) noexcept;

// Records a worker-side check of an answer given from the mirror.
void VerifyDesc(uint32_t attr, GXAttrType answered);
void VerifyFmt(uint32_t vtxFmt, uint32_t attr, VtxAttrFmt answered);

} // namespace GxVtxMirror

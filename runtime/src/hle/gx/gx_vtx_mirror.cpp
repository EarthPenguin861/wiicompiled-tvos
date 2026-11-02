// gx_vtx_mirror.cpp - game-thread mirror of the vertex layout state (see gx_vtx_mirror.h).
#include "gx_vtx_mirror.h"

#include "gx_cp_decode.h"
#include "gx_stream_common.h"
#include "gx_worker.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace GxVtxMirror {
namespace {

struct MirrorState {
    bool valid = false;
    GXAttrType desc[26]{};
    VtxAttrFmt fmt[8][26]{};
    // Gather-pipe parse position, as HleFifoWrite would have it after the recorded bytes.
    uint64_t skip = 0;              // bytes still to pass: an XF payload or a draw's vertex data
    bool skipIsVertexData = false;  // the worker is inside a begin while `skip` runs
    uint8_t packet[9]{};            // header of the packet being assembled
    uint32_t packetLength = 0;      // header bytes received so far; 0 = between packets
    uint32_t packetNeeded = 0;      // header bytes this packet needs
};

MirrorState g_mirror;
std::atomic<bool> g_broken{false};
uint64_t g_answered = 0;

// HleFifoWrite parses packets out of a 4096-byte buffer and drops what does not fit; an XF load
// that large is never seen in practice, and the mirror stops following the stream if one is.
constexpr uint64_t kWorkerFifoBufferBytes = 4096;

bool EnabledByEnvironment() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("MKW_GX_VTX_MIRROR");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void ResetParser() noexcept {
    g_mirror.skip = 0;
    g_mirror.skipIsVertexData = false;
    g_mirror.packetLength = 0;
    g_mirror.packetNeeded = 0;
}

uint32_t CanonicalAttr(uint32_t attr) noexcept {
    return attr == GX_VA_NBT ? GX_VA_NRM : attr;
}

// Bytes one vertex occupies in the stream, walking the enabled attributes exactly like
// HleFifoWrite's in-begin loop. 0 when that loop would not consume a whole vertex (no attribute
// enabled, or a descriptor type it does not parse), which the mirror cannot follow.
uint32_t VertexBytes(uint32_t vtxFmt) noexcept {
    if (vtxFmt >= 8) {
        return 0;
    }
    uint32_t bytes = 0;
    for (int i = 0; i < 26; ++i) {
        const GXAttrType type = g_mirror.desc[i];
        if (type == GX_NONE) {
            continue;
        }
        const GXAttr attr = static_cast<GXAttr>(i);
        const VtxAttrFmt& fmt = g_mirror.fmt[vtxFmt][i];
        if (IsMatrixIndexAttr(attr)) {
            bytes += 1;
        } else if (type == GX_DIRECT) {
            if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
                bytes += GxStream::ColorByteSize(fmt.type, fmt.cnt);
            } else {
                bytes += static_cast<uint32_t>(GetCompSizeBytes(fmt.type)) *
                         GxStream::AttrCompCount(attr, fmt, 1u);
            }
        } else if (type == GX_INDEX8 || type == GX_INDEX16) {
            const uint32_t indexBytes = type == GX_INDEX8 ? 1u : 2u;
            bytes += indexBytes * (attr == GX_VA_NRM ? GxStream::NormalIndexCount(fmt) : 1u);
        } else {
            return 0;
        }
    }
    return bytes;
}

void StartVertexData(uint32_t vtxFmt, uint32_t vertexCount) noexcept {
    const uint32_t vertexBytes = VertexBytes(vtxFmt);
    if (vertexBytes == 0) {
        Invalidate();
        return;
    }
    g_mirror.skip = static_cast<uint64_t>(vertexCount) * vertexBytes;
    g_mirror.skipIsVertexData = true;
}

// Packet lengths and the order of the tests follow HleFifoWrite outside a begin.
void StartPacket(uint8_t cmd) noexcept {
    using namespace GxCmd;
    const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;
    uint32_t needed = 1;
    if (cmd == GX_NOP_CMD || opcode == GX_CMD_INVL_VC_CMD) {
        needed = 1;
    } else if (cmd == GX_LOAD_BP_REG_CMD) {
        needed = 5;
    } else if (opcode == GX_LOAD_CP_REG_CMD) {
        needed = 6;
    } else if (opcode == GX_LOAD_XF_REG_CMD) {
        needed = 5;
    } else if (opcode >= GX_LOAD_INDX_A_CMD && opcode <= GX_LOAD_INDX_D_CMD) {
        needed = 5;
    } else if (opcode == GX_CMD_CALL_DL_CMD) {
        needed = 9;
    } else if (GxStream::IsDrawOpcode(opcode)) {
        needed = 3;
    }
    // One-byte packets, and unknown bytes, which the worker discards one at a time.
    if (needed == 1) {
        return;
    }
    g_mirror.packet[0] = cmd;
    g_mirror.packetLength = 1;
    g_mirror.packetNeeded = needed;
}

void FinishPacket() noexcept {
    using namespace GxCmd;
    const uint8_t cmd = g_mirror.packet[0];
    const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;
    g_mirror.packetLength = 0;
    if (opcode == GX_LOAD_CP_REG_CMD) {
        bool changed = false;
        GxCpDecode::ApplyVtxLayoutCpRegWrite(g_mirror.desc, g_mirror.fmt, g_mirror.packet[1],
                                             GxStream::ReadBE32(g_mirror.packet + 2), changed);
        return;
    }
    if (opcode == GX_LOAD_XF_REG_CMD) {
        const uint64_t payload = (static_cast<uint64_t>(GxStream::ReadBE16(g_mirror.packet + 1)) + 1u) * 4u;
        if (payload + 5u > kWorkerFifoBufferBytes) {
            Invalidate();
            return;
        }
        g_mirror.skip = payload;
        g_mirror.skipIsVertexData = false;
        return;
    }
    if (opcode == GX_CMD_CALL_DL_CMD) {
        // The worker runs the list at this point; its CP writes are not followed here.
        Invalidate();
        return;
    }
    if (GxStream::IsDrawOpcode(opcode)) {
        const uint16_t vertexCount = GxStream::ReadBE16(g_mirror.packet + 1);
        if (vertexCount != 0) {
            StartVertexData(cmd & GX_VAT_MASK_CMD, vertexCount);
        }
        return;
    }
    // BP loads and indexed XF loads do not touch the vertex layout.
}

void Consume(const uint8_t* data, uint32_t size) noexcept {
    uint32_t i = 0;
    while (i < size && g_mirror.valid) {
        if (g_mirror.skip != 0) {
            const uint64_t take = std::min<uint64_t>(g_mirror.skip, size - i);
            g_mirror.skip -= take;
            i += static_cast<uint32_t>(take);
            if (g_mirror.skip == 0) {
                g_mirror.skipIsVertexData = false;
            }
            continue;
        }
        const uint8_t byte = data[i++];
        if (g_mirror.packetLength == 0) {
            StartPacket(byte);
            continue;
        }
        g_mirror.packet[g_mirror.packetLength++] = byte;
        if (g_mirror.packetLength == g_mirror.packetNeeded) {
            FinishPacket();
        }
    }
}

void ReportMismatch(const char* query, uint32_t vtxFmt, uint32_t attr) {
    if (!g_broken.exchange(true, std::memory_order_relaxed)) {
        RT_LOGF(RT_TAG_GX,
                "GX vertex-state mirror disagreed with the worker (%s fmt %u attr %u); "
                "answering from the worker from now on\n",
                query, vtxFmt, attr);
    }
}

struct InlineListenerRegistration {
    InlineListenerRegistration() { GxWorker::SetInlineListener(&Invalidate); }
};
InlineListenerRegistration g_inlineListenerRegistration;

} // namespace

bool Usable() noexcept {
    return g_mirror.valid && !g_broken.load(std::memory_order_relaxed);
}

GXAttrType Desc(uint32_t attr) noexcept {
    ++g_answered;
    if (g_answered == 1 || g_answered == 1000 || g_answered == 1000000) {
        RT_LOGF(RT_TAG_GX, "GX vertex-state mirror: %llu queries answered without a worker sync\n",
                static_cast<unsigned long long>(g_answered));
    }
    const uint32_t canonical = CanonicalAttr(attr);
    return canonical < 26 ? g_mirror.desc[canonical] : GX_NONE;
}

VtxAttrFmt Fmt(uint32_t vtxFmt, uint32_t attr) noexcept {
    const uint32_t canonical = CanonicalAttr(attr);
    if (vtxFmt >= 8 || canonical >= GX_VA_MAX_ATTR || canonical == GX_VA_NULL) {
        return VtxAttrFmt{};
    }
    return g_mirror.fmt[vtxFmt][canonical];
}

void Invalidate() noexcept {
    g_mirror.valid = false;
    ResetParser();
}

void ReloadFromWorkerState() noexcept {
    // A begin in progress or a partial packet in the worker's FIFO buffer is parse state the
    // mirror does not take over; it stays invalid until a later sync point.
    if (!EnabledByEnvironment() || g_broken.load(std::memory_order_relaxed) || g_hleGxState.inBegin ||
        g_hleGxState.fifoByteCount != 0) {
        Invalidate();
        return;
    }
    std::memcpy(g_mirror.desc, g_hleGxState.vtxDesc, sizeof(g_mirror.desc));
    std::memcpy(g_mirror.fmt, g_hleGxState.vtxAttrFmt, sizeof(g_mirror.fmt));
    ResetParser();
    g_mirror.valid = true;
}

void OnSetVtxDesc(uint32_t attr, uint32_t type) noexcept {
    if (!g_mirror.valid) {
        return;
    }
    const uint32_t canonical = CanonicalAttr(attr);
    if (canonical >= 26 || canonical == GX_VA_NULL) {
        return;
    }
    g_mirror.desc[canonical] = static_cast<GXAttrType>(type);
    if (attr == GX_VA_NBT) {
        g_mirror.desc[GX_VA_NBT] = GX_NONE;
    }
}

void OnClearVtxDesc() noexcept {
    if (!g_mirror.valid) {
        return;
    }
    for (auto& type : g_mirror.desc) {
        type = GX_NONE;
    }
}

void OnSetVtxAttrFmt(uint32_t vtxFmt, uint32_t attr, uint32_t cnt, uint32_t type, uint32_t frac) noexcept {
    if (!g_mirror.valid) {
        return;
    }
    const uint32_t canonical = CanonicalAttr(attr);
    if (vtxFmt < 8 && canonical < 26) {
        auto& fmt = g_mirror.fmt[vtxFmt][canonical];
        fmt.cnt = static_cast<GXCompCnt>(cnt);
        fmt.type = static_cast<GXCompType>(type);
        fmt.frac = static_cast<u8>(frac);
        if (attr == GX_VA_NBT) {
            g_mirror.fmt[vtxFmt][GX_VA_NBT] = {};
        }
    }
}

void OnBegin(uint32_t vtxFmt, uint32_t vertexCount) noexcept {
    if (!g_mirror.valid) {
        return;
    }
    // GXBegin discards any partial packet in the worker's FIFO buffer.
    ResetParser();
    if (vertexCount != 0) {
        StartVertexData(vtxFmt, vertexCount);
    }
}

void OnEnd() noexcept {
    // GXEnd leaves the worker's begin early; vertex bytes still owed would then be parsed as
    // packets, which the mirror does not follow.
    if (g_mirror.valid && g_mirror.skipIsVertexData && g_mirror.skip != 0) {
        Invalidate();
    }
}

void OnFifoWrite(uint32_t value, uint32_t sizeBytes) noexcept {
    if (!g_mirror.valid) {
        return;
    }
    // HleFifoWrite treats size 0 as 4 and never pushes more than 4 bytes for one write.
    const uint32_t count = sizeBytes == 0 ? 4u : std::min<uint32_t>(sizeBytes, 4u);
    if (count == 3) {
        // pushBytes appends three bytes of which only one is the written value; the gather pipe
        // never sees such a write, and the mirror does not model it.
        Invalidate();
        return;
    }
    uint8_t bytes[4];
    if (count == 4) {
        bytes[0] = static_cast<uint8_t>(value >> 24);
        bytes[1] = static_cast<uint8_t>(value >> 16);
        bytes[2] = static_cast<uint8_t>(value >> 8);
        bytes[3] = static_cast<uint8_t>(value);
    } else if (count == 2) {
        bytes[0] = static_cast<uint8_t>(value >> 8);
        bytes[1] = static_cast<uint8_t>(value);
    } else {
        bytes[0] = static_cast<uint8_t>(value);
    }
    Consume(bytes, count);
}

void OnFifoBurst(const uint8_t* data, uint32_t sizeBytes) noexcept {
    if (g_mirror.valid && data != nullptr) {
        Consume(data, sizeBytes);
    }
}

void VerifyDesc(uint32_t attr, GXAttrType answered) {
    GxWorker::Submit([attr, answered] {
        const uint32_t canonical = CanonicalAttr(attr);
        const GXAttrType real = canonical < 26 ? g_hleGxState.vtxDesc[canonical] : GX_NONE;
        if (real != answered) {
            ReportMismatch("GXGetVtxDesc", 0, attr);
        }
    });
}

void VerifyFmt(uint32_t vtxFmt, uint32_t attr, VtxAttrFmt answered) {
    GxWorker::Submit([vtxFmt, attr, answered] {
        const uint32_t canonical = CanonicalAttr(attr);
        VtxAttrFmt real{};
        if (vtxFmt < 8 && canonical < GX_VA_MAX_ATTR && canonical != GX_VA_NULL) {
            real = g_hleGxState.vtxAttrFmt[vtxFmt][canonical];
        }
        if (real.cnt != answered.cnt || real.type != answered.type || real.frac != answered.frac) {
            ReportMismatch("GXGetVtxAttrFmt", vtxFmt, attr);
        }
    });
}

} // namespace GxVtxMirror

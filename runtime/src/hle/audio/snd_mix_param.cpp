// nw4r::snd::detail::Voice::CalcMixParam (PAL 800AB760), in native code: the per-voice pan and send
// math behind every sound's AX mix volumes, recomputed for each playing voice every audio frame.
// It is a literal transcription of the PAL machine code (not of the open-source nw4r reference,
// which differs), so the volumes it produces are bit-identical: every operation is the same single
// precision IEEE operation in the same order, and floating-point contraction is off so no multiply
// and add are fused. The pan tables are read from the game's own read-only data.
//
// Voices the transcription does not cover (Dolby Pro Logic II output, a balance-panned voice beyond
// its second channel, any non-finite volume) run the original translation instead.
// MKW_SND_NATIVE_MIX=0 always runs the original; MKW_SND_MIX_VERIFY=1 runs both and logs mismatches.

#include "abi_bridge.h"
#include "guest_abi/guest_region.h"
#include "guest_abi/memory_access.h"
#include "guest_function_imports.h"
#include "isa/big_endian.h"
#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace {

constexpr uint32_t kAxManagerInstance = 0x802D5730; // AxManager::GetInstance()'s static instance
constexpr uint32_t kPanTablePointers = 0x80274B70; // per pan curve: sqrt, sin/cos, linear
constexpr uint32_t kVolumeTable = 0x802497B8;      // Util::CalcVolumeRatio's dB table
constexpr uint32_t kVoiceBytes = 0x114;            // fields of Voice the function reads
constexpr uint32_t kVoiceOutParamStride = 24;

enum : uint32_t { kOutputStereo = 0, kOutputSurround = 1, kOutputDpl2 = 2, kOutputMono = 3 };

bool EnvFlag(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    return value == nullptr || value[0] == '\0' ? fallback : value[0] != '0';
}

const bool g_native = EnvFlag("MKW_SND_NATIVE_MIX", true);
const bool g_verify = EnvFlag("MKW_SND_MIX_VERIFY", false);

struct Tables {
    std::array<std::array<float, 257>, 3> pan{};
    float surroundVolume = 0.0f; // Util::CalcVolumeRatio(-3.0f)
};

const Tables* GetTables() {
    static const Tables* tables = [] () -> const Tables* {
        static Tables loaded;
        for (uint32_t curve = 0; curve < 3; ++curve) {
            const uint32_t table = Memory::Read32(kPanTablePointers + curve * 4);
            if (table == 0) {
                return nullptr;
            }
            for (uint32_t i = 0; i < 257; ++i) {
                loaded.pan[curve][i] = std::bit_cast<float>(Memory::Read32(table + i * 4));
            }
        }
        // CalcVolumeRatio: clamp(-3, -90.4, 6) * 10 truncated is -30, index -30 + 904.
        loaded.surroundVolume = std::bit_cast<float>(Memory::Read32(kVolumeTable + (904 - 30) * 4));
        return &loaded;
    }();
    return tables;
}

struct PanInfo {
    uint32_t curve = 0;
    bool centerZero = false;
    bool zeroClamp = false;
};

// Util::CalcPanRatio (800ACE00)
float CalcPanRatio(const Tables& tables, float pan, const PanInfo& info) {
    float x = 1.0f;
    if (pan <= 1.0f) {
        x = pan >= -1.0f ? pan : -1.0f;
    }
    const float position = (1.0f + x) * 0.5f;
    const float scaled = 256.0f * position;
    const int index = static_cast<int>(0.5f + scaled);
    const auto& table = tables.pan[info.curve];
    float ratio = table[index];
    if (info.centerZero) {
        ratio = ratio / table[128];
    }
    const float high = info.zeroClamp ? 1.0f : 2.0f;
    if (ratio <= high) {
        return ratio >= 0.0f ? ratio : 0.0f;
    }
    return high;
}

// Util::CalcSurroundPanRatio (800ACEE0)
float CalcSurroundPanRatio(const Tables& tables, float pan, const PanInfo& info) {
    float x = 2.0f;
    if (pan <= 2.0f) {
        x = pan >= 0.0f ? pan : 0.0f;
    }
    const float position = x * 0.5f;
    const float scaled = 256.0f * position;
    const int index = static_cast<int>(0.5f + scaled);
    const float ratio = tables.pan[info.curve][index];
    if (ratio <= 2.0f) {
        return ratio >= 0.0f ? ratio : 0.0f;
    }
    return 2.0f;
}

// Voice::CalcMixVolume: 0 for silence, else 32768 * volume truncated and capped at 0xFFFF. Only
// called with finite volumes (others take the original path).
uint16_t CalcMixVolume(float volume) {
    if (volume <= 0.0f) {
        return 0;
    }
    const float scaled = 32768.0f * volume;
    if (!(scaled < 65535.0f)) {
        return 0xFFFF;
    }
    return static_cast<uint16_t>(static_cast<uint32_t>(scaled));
}

struct MixResult {
    std::array<uint16_t, 12> mix{};   // AxVoice::MixParam: L R S, AuxA L R S, AuxB L R S, AuxC L R S
    std::array<uint16_t, 8> remote{}; // AxVoice::RemoteMixParam: main, aux for each Wii Remote
};

// False when this voice must take the original path.
bool Compute(uint32_t self, int32_t channel, uint32_t voice, MixResult& out) {
    const Tables* tables = GetTables();
    if (tables == nullptr || voice > 64) {
        return false;
    }
    const uint32_t span = std::max(kVoiceBytes, 0x40 + voice * kVoiceOutParamStride);
    const uint8_t* v = MemoryInline::GetPointerFast(self, span);
    const uint8_t* axm = MemoryInline::GetPointerFast(kAxManagerInstance, 4);
    if (v == nullptr || axm == nullptr) {
        return false;
    }
    const auto u32 = [v](uint32_t offset) { return BigEndian::Read32(v + offset); };
    const auto f32 = [v](uint32_t offset) { return std::bit_cast<float>(BigEndian::Read32(v + offset)); };
    const uint32_t vop = voice * kVoiceOutParamStride;
    const uint32_t mode = BigEndian::Read32(axm);
    if (mode != kOutputStereo && mode != kOutputSurround && mode != kOutputMono) {
        return false;
    }

    const uint32_t lines = u32(0xB4);
    float mainVolume = 0.0f;
    float mainSend = 0.0f;
    float fxSendA = 0.0f;
    float fxSendB = 0.0f;
    float fxSendC = 0.0f;
    if (lines & 1) {
        mainVolume = f32(0xB8);
        mainSend = f32(0xBC);
        const float send = f32(0xC0) + f32(vop + 0x3C);
        if (!(send <= 1.0f)) {
            fxSendA = 1.0f;
        } else {
            fxSendA = send >= 0.0f ? send : 0.0f;
        }
        fxSendB = f32(0xC4);
        fxSendC = f32(0xC8);
    }
    const float main = mainVolume * mainSend;
    const float fxA = mainVolume * fxSendA;
    const float fxB = mainVolume * fxSendB;
    const float fxC = mainVolume * fxSendC;

    std::array<float, 4> remote{};
    for (uint32_t i = 0; i < 4; ++i) {
        if (lines & (2u << i)) {
            remote[i] = f32(0xCC + i * 4) * f32(0xDC + i * 4);
        } else {
            remote[i] = 0.0f;
        }
    }

    PanInfo info;
    const uint32_t curve = u32(0x110);
    if (curve <= 8) {
        info.curve = curve / 3;
        info.centerZero = curve % 3 >= 1;
        info.zeroClamp = curve % 3 == 2;
    }

    const int32_t channels = static_cast<int32_t>(u32(0x8C));
    float left = 0.0f;
    float right = 0.0f;
    float front = 0.0f;
    float rear = 0.0f;
    if (channels > 1 && u32(0x10C) == 1) {
        const float pan = f32(0xA8) + f32(vop + 0x34);
        const float surroundPan = f32(0xAC) + f32(vop + 0x38);
        if (channel == 0) {
            left = CalcPanRatio(*tables, pan, info);
        } else if (channel == 1) {
            right = CalcPanRatio(*tables, -pan, info);
        } else {
            return false;
        }
        front = CalcSurroundPanRatio(*tables, surroundPan, info);
        rear = CalcSurroundPanRatio(*tables, 2.0f - surroundPan, info);
    } else {
        float voicePan = 0.0f;
        if (channels == 2) {
            if (channel == 0) {
                voicePan = -1.0f;
            }
            if (channel == 1) {
                voicePan = 1.0f;
            }
        }
        const float pan = (f32(0xA8) + voicePan) + f32(vop + 0x34);
        const float surroundPan = f32(0xAC) + f32(vop + 0x38);
        left = CalcPanRatio(*tables, pan, info);
        right = CalcPanRatio(*tables, -pan, info);
        front = CalcSurroundPanRatio(*tables, surroundPan, info);
        rear = CalcSurroundPanRatio(*tables, 2.0f - surroundPan, info);
    }

    const float surround = tables->surroundVolume;
    const float lrMixed = 0.5f * (left + right);

    // m, a, b, c: main, AuxA, AuxB, AuxC; each L, R, S.
    std::array<float, 12> mix{};
    switch (mode) {
    case kOutputStereo:
        mix = {main * left, main * right, 0.0f, fxA * left, fxA * right, 0.0f,
               fxB * left, fxB * right, 0.0f, fxC * left, fxC * right, 0.0f};
        break;
    case kOutputMono: {
        const float m = main * lrMixed;
        const float a = fxA * lrMixed;
        const float b = fxB * lrMixed;
        const float c = fxC * lrMixed;
        mix = {m, m, 0.0f, a, a, 0.0f, b, b, 0.0f, c, c, 0.0f};
        break;
    }
    case kOutputSurround: {
        const float fl = left * front;
        const float fr = right * front;
        const float rs = surround * rear;
        mix = {main * fl, main * fr, main * rs, fxA * fl, fxA * fr, fxA * rs,
               fxB * fl, fxB * fr, fxB * rs, fxC * fl, fxC * fr, fxC * rs};
        break;
    }
    default:
        return false;
    }

    std::array<float, 4> rmt{};
    for (uint32_t i = 0; i < 4; ++i) {
        rmt[i] = lrMixed * remote[i];
    }
    for (float value : mix) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    for (float value : rmt) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    for (size_t i = 0; i < mix.size(); ++i) {
        out.mix[i] = CalcMixVolume(mix[i]);
    }
    for (uint32_t i = 0; i < 4; ++i) {
        out.remote[i * 2] = CalcMixVolume(rmt[i]);
        out.remote[i * 2 + 1] = 0;
    }
    return true;
}

void Store(uint32_t address, const uint16_t* values, size_t count) {
    uint8_t* dst = MemoryInline::GetPointerFast(address, count * 2);
    if (dst != nullptr) {
        for (size_t i = 0; i < count; ++i) {
            BigEndian::Write16(dst + i * 2, values[i]);
        }
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        Memory::Write16(address + static_cast<uint32_t>(i * 2), values[i]);
    }
}

void Verify(uint32_t mixAddress, uint32_t remoteAddress, const MixResult& native) {
    static std::atomic<uint64_t> calls{0};
    static std::atomic<uint64_t> mismatches{0};
    bool same = true;
    for (size_t i = 0; i < native.mix.size(); ++i) {
        same &= Memory::Read16(mixAddress + static_cast<uint32_t>(i * 2)) == native.mix[i];
    }
    for (size_t i = 0; i < native.remote.size(); ++i) {
        same &= Memory::Read16(remoteAddress + static_cast<uint32_t>(i * 2)) == native.remote[i];
    }
    const uint64_t n = calls.fetch_add(1) + 1;
    if (!same) {
        const uint64_t bad = mismatches.fetch_add(1) + 1;
        if (bad <= 8) {
            RT_LOGF("snd-mix", "mismatch: L %u/%u R %u/%u AuxA L %u/%u\n",
                    Memory::Read16(mixAddress), native.mix[0], Memory::Read16(mixAddress + 2),
                    native.mix[1], Memory::Read16(mixAddress + 6), native.mix[3]);
        }
    }
    if ((n & (n - 1)) == 0 && n >= 1024) {
        RT_LOGF("snd-mix", "verify: %llu calls, %llu mismatches\n", static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(mismatches.load()));
    }
}

} // namespace

extern "C" void Voice_CalcMixParam_800ab760(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    const int32_t channel = static_cast<int32_t>(ctx->gpr[4]);
    const uint32_t voice = ctx->gpr[5];
    const uint32_t mixAddress = ctx->gpr[6];
    const uint32_t remoteAddress = ctx->gpr[7];
    if (!g_native) {
        MKW_GUEST_NEXT(800AB760)(ctx);
        return;
    }
    MixResult result;
    const bool computed = Compute(self, channel, voice, result);
    if (!computed || g_verify) {
        MKW_GUEST_NEXT(800AB760)(ctx);
        if (computed) {
            Verify(mixAddress, remoteAddress, result);
        }
        return;
    }
    Store(mixAddress, result.mix.data(), result.mix.size());
    Store(remoteAddress, result.remote.data(), result.remote.size());
}
REGISTER_NATIVE_WRAPPER_AS(0x800AB760, Voice_CalcMixParam_800ab760, "Voice_CalcMixParam_800ab760");

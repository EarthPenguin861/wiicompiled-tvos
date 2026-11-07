// Optional cuts to the race renderer for slow hosts: the kart and object shadow models, and the
// drawing of particle effects (they are still simulated, so emitters keep their normal lifetimes).
// Chosen in the settings bar ([video] shadows / particles); off by default on the Apple TV, where
// they cost about 1.5 fps. MKW_NO_SHADOWS / MKW_NO_PARTICLES (1 or 0) override the setting.

#include "abi_bridge.h"
#include "guest_abi/guest_region.h"
#include "guest_function_imports.h"
#include "platform/apple_target.h"
#include "render_toggles.h"
#include "runtime_config.h"

#include <atomic>
#include <cstdlib>

namespace {

constexpr bool kDefaultEnabled = !MKW_HOST_TVOS;

bool Initial(const char* disableEnv, bool configured) {
    if (const char* value = std::getenv(disableEnv); value != nullptr && value[0] != '\0') {
        return value[0] == '0';
    }
    return configured;
}

std::atomic<bool>& Shadows() {
    static std::atomic<bool> enabled{
        Initial("MKW_NO_SHADOWS", RuntimeConfigFile::ShadowsEnabled(kDefaultEnabled))};
    return enabled;
}

std::atomic<bool>& Particles() {
    static std::atomic<bool> enabled{
        Initial("MKW_NO_PARTICLES", RuntimeConfigFile::ParticlesEnabled(kDefaultEnabled))};
    return enabled;
}

} // namespace

namespace RenderToggles {

bool ShadowsEnabled() {
    return Shadows().load(std::memory_order_relaxed);
}

void SetShadowsEnabled(bool enabled) {
    Shadows().store(enabled, std::memory_order_relaxed);
    RuntimeConfigFile::SetShadowsEnabled(enabled);
}

bool ParticlesEnabled() {
    return Particles().load(std::memory_order_relaxed);
}

void SetParticlesEnabled(bool enabled) {
    Particles().store(enabled, std::memory_order_relaxed);
    RuntimeConfigFile::SetParticlesEnabled(enabled);
}

} // namespace RenderToggles

// ShadowModelsMgr::Draw
extern "C" void ShadowModelsMgr_Draw_805b8e70(CpuContext* ctx) {
    if (RenderToggles::ShadowsEnabled()) {
        MKW_GUEST_NEXT(805B8E70)(ctx);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x805B8E70, ShadowModelsMgr_Draw_805b8e70, "ShadowModelsMgr_Draw_805b8e70");

// ShadowModelsMgr::Draw2
extern "C" void ShadowModelsMgr_Draw2_805b8f40(CpuContext* ctx) {
    if (RenderToggles::ShadowsEnabled()) {
        MKW_GUEST_NEXT(805B8F40)(ctx);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x805B8F40, ShadowModelsMgr_Draw2_805b8f40, "ShadowModelsMgr_Draw2_805b8f40");

// nw4r::ef::EffectSystem::Draw
extern "C" void EffectSystem_Draw_800276c0(CpuContext* ctx) {
    if (RenderToggles::ParticlesEnabled()) {
        MKW_GUEST_NEXT(800276C0)(ctx);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x800276C0, EffectSystem_Draw_800276c0, "EffectSystem_Draw_800276c0");

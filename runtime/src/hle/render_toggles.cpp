// Optional cuts to the race renderer for slow hosts, chosen from the environment:
//   MKW_NO_SHADOWS=1    skips the kart and object shadow models.
//   MKW_NO_PARTICLES=1  skips drawing particle effects (they are still simulated, so
//                       emitters keep their normal lifetimes).

#include "abi_bridge.h"
#include "guest_abi/guest_region.h"
#include "guest_function_imports.h"
#include "runtime_log.h"

#include <cstdlib>

namespace {

bool EnvFlag(const char* name) {
    const char* value = std::getenv(name);
    const bool enabled = value != nullptr && value[0] != '\0' && value[0] != '0';
    if (enabled) {
        RT_LOGF("render", "%s set\n", name);
    }
    return enabled;
}

bool NoShadows() {
    static const bool enabled = EnvFlag("MKW_NO_SHADOWS");
    return enabled;
}

bool NoParticles() {
    static const bool enabled = EnvFlag("MKW_NO_PARTICLES");
    return enabled;
}

} // namespace

// ShadowModelsMgr::Draw
extern "C" void ShadowModelsMgr_Draw_805b8e70(CpuContext* ctx) {
    if (!NoShadows()) {
        MKW_GUEST_NEXT(805B8E70)(ctx);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x805B8E70, ShadowModelsMgr_Draw_805b8e70, "ShadowModelsMgr_Draw_805b8e70");

// ShadowModelsMgr::Draw2
extern "C" void ShadowModelsMgr_Draw2_805b8f40(CpuContext* ctx) {
    if (!NoShadows()) {
        MKW_GUEST_NEXT(805B8F40)(ctx);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x805B8F40, ShadowModelsMgr_Draw2_805b8f40, "ShadowModelsMgr_Draw2_805b8f40");

// nw4r::ef::EffectSystem::Draw
extern "C" void EffectSystem_Draw_800276c0(CpuContext* ctx) {
    if (!NoParticles()) {
        MKW_GUEST_NEXT(800276C0)(ctx);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x800276C0, EffectSystem_Draw_800276c0, "EffectSystem_Draw_800276c0");

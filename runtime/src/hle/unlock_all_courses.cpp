// Every cup and course is selectable from the start. Only the checks are answered; the save
// file's unlock flags are left untouched. MKW_LOCKED_COURSES=1 keeps the game's own checks.

#include "abi_bridge.h"
#include "guest_abi/guest_region.h"
#include "guest_function_imports.h"

#include <cstdlib>

namespace {

bool UnlockAll() {
    static const bool enabled = [] {
        const char* value = std::getenv("MKW_LOCKED_COURSES");
        return value == nullptr || value[0] == '\0' || value[0] == '0';
    }();
    return enabled;
}

} // namespace

// IsCupUnlocked
extern "C" void IsCupUnlocked_8054fc44(CpuContext* ctx) {
    MKW_GUEST_NEXT(8054FC44)(ctx);
    if (UnlockAll()) {
        ctx->gpr[3] = 1;
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8054FC44, IsCupUnlocked_8054fc44, "IsCupUnlocked_8054fc44");

// SectionParams::IsCupUnlockedInOneCC
extern "C" void SectionParams_IsCupUnlockedInOneCC_805e37b0(CpuContext* ctx) {
    MKW_GUEST_NEXT(805E37B0)(ctx);
    if (UnlockAll()) {
        ctx->gpr[3] = 1;
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x805E37B0, SectionParams_IsCupUnlockedInOneCC_805e37b0,
                           "SectionParams_IsCupUnlockedInOneCC_805e37b0");

// SectionParams::IsCourseUnlockedInOneCC
extern "C" void SectionParams_IsCourseUnlockedInOneCC_805e36a0(CpuContext* ctx) {
    MKW_GUEST_NEXT(805E36A0)(ctx);
    if (UnlockAll()) {
        ctx->gpr[3] = 1;
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x805E36A0, SectionParams_IsCourseUnlockedInOneCC_805e36a0,
                           "SectionParams_IsCourseUnlockedInOneCC_805e36a0");

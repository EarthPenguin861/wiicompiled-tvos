// MKW_RACE_30FPS=1: races with one or two local players use the game's own 30 fps mode.
//
// RKSystem::Run draws one frame per game update unless RKSystem's 30 fps flag is set, in which case
// it runs two updates per drawn frame and the scene code skips its draw preparation on the first of
// the two. The game sets the flag for three and four local players (RaceScene::CreateAndInitInstances
// -> GameScene::SetFramerate). A host that cannot draw 60 frames per second in the 60 fps mode runs
// the race in slow motion; the 30 fps mode needs only half as many drawn frames for the same game
// speed.

#include "abi_bridge.h"
#include "guest_abi/guest_region.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

bool ForceRace30Fps() {
    static const bool enabled = [] {
        const char* value = std::getenv("MKW_RACE_30FPS");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

// Non-zero while RaceScene::CreateAndInitInstances runs. Game thread only.
int g_raceInitDepth = 0;

} // namespace

// RaceScene::CreateAndInitInstances
extern "C" void RaceScene_CreateAndInitInstances_80554208(CpuContext* ctx) {
    struct Scope {
        Scope() { ++g_raceInitDepth; }
        ~Scope() { --g_raceInitDepth; }
    } scope;
    MKW_GUEST_NEXT(80554208)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80554208, RaceScene_CreateAndInitInstances_80554208,
                           "RaceScene_CreateAndInitInstances_80554208");

// GameScene::SetFramerate(bool is30Fps)
extern "C" void GameScene_SetFramerate_8051af84(CpuContext* ctx) {
    MKW_GUEST_NEXT(8051AF84)(ctx);
    if (g_raceInitDepth == 0 || !ForceRace30Fps()) {
        return;
    }
    // What the function itself stores for is30Fps = true: the flag in the RKSystem instance and
    // two retraces per frame in the display.
    const uint32_t system = MKW_GADDR(802A4080);
    if (Memory::Read8(system + 0x69u) != 0) {
        return;
    }
    Memory::Write8(system + 0x69u, 1);
    const uint32_t display = Memory::Read32(Memory::Read32(MKW_GADDR(80385FC8)) + 0x4Cu);
    Memory::Write8(display + 0x8u, 2);
    RT_LOG(RT_TAG_CONFIG) << "race 30 fps mode forced (two game updates per drawn frame)" << std::endl;
}
REGISTER_NATIVE_WRAPPER_AS(0x8051AF84, GameScene_SetFramerate_8051af84, "GameScene_SetFramerate_8051af84");

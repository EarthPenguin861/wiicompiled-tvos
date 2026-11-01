// Apple TV process entry: UIKit start-up through SDL plus a full-size stack for
// the game.
//
// Why the dedicated stack: the guest's main thread runs on the host main
// thread's own stack (fiber_manager.cpp registers the scheduler fiber as the
// guest main thread; only guest threads created later get their own 1 MiB
// coroutine stacks). macOS and Linux give the main thread 8 MiB. tvOS gives it
// 1 MiB, part of which UIKit and SDL have already used by the time the runtime
// starts, and the main-thread stack size cannot be raised on tvOS. So the
// runtime is started on an 8 MiB stack switched to with the same Darwin AArch64
// context primitive host_context.cpp uses for every guest fiber
// (platform/macos/co_switch.S). It is still the main thread: SDL event pumping,
// UIKit and Metal presentation behave exactly as before; only the stack memory
// differs, as it already does whenever a guest fiber drives a frame.

#include "platform/apple_target.h"

#if MKW_HOST_TVOS

#include "platform/tvos_app.h"

#include <SDL3/SDL_hints.h>
// main() lives in main.cpp; SDL_main.h must only declare SDL_RunApp here, not
// inject SDL's own main() (the runtime targets also define this globally).
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL3/SDL_main.h>

#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" void mkw_co_switch(void** targetSp, void** sourceSp);
extern "C" void* mkw_co_init(void* stackTop, void (*entry)(void*), void* argument);

namespace RuntimeTvos {
namespace {

// Matches the default macOS/Linux main-thread stack the runtime is tested with.
constexpr std::size_t kGameStackSize = 8u * 1024u * 1024u;

struct LaunchState {
    EntryPoint entry = nullptr;
    int argc = 0;
    char** argv = nullptr;
    int result = EXIT_FAILURE;
    void* hostStackPointer = nullptr;
    void* gameStackPointer = nullptr;
};

LaunchState g_launch;
std::vector<char*> g_runtimeArguments;

// RuntimeMain rejects unknown options on purpose, but UIKit and Xcode may append
// Cocoa user-defaults arguments ("-Key value") to an app launch. The runtime's
// own options are all spelled "--...", so only those are forwarded.
void SelectRuntimeArguments(int argc, char* argv[]) {
    g_runtimeArguments.clear();
    if (argc > 0 && argv != nullptr && argv[0] != nullptr) {
        g_runtimeArguments.push_back(argv[0]);
    }
    for (int i = 1; i < argc && argv != nullptr; ++i) {
        if (argv[i] != nullptr && std::strncmp(argv[i], "--", 2) == 0) {
            g_runtimeArguments.push_back(argv[i]);
        } else if (argv[i] != nullptr) {
            std::fprintf(stderr, "[tvos] ignoring launch argument '%s'\n", argv[i]);
        }
    }
    g_launch.argc = static_cast<int>(g_runtimeArguments.size());
    g_runtimeArguments.push_back(nullptr);
    g_launch.argv = g_runtimeArguments.data();
}

void GameStackMain(void* argument) {
    auto* launch = static_cast<LaunchState*>(argument);
    launch->result = launch->entry(launch->argc, launch->argv);
    // Back to the UIKit stack. Nothing ever switches to this frame again, and
    // the coroutine trampoline that called us has no caller to return to.
    mkw_co_switch(&launch->hostStackPointer, &launch->gameStackPointer);
    std::abort();
}

int SDLCALL RunOnGameStack(int argc, char* argv[]) {
    SelectRuntimeArguments(argc, argv);

    const std::size_t guardSize = static_cast<std::size_t>(getpagesize());
    const std::size_t totalSize = kGameStackSize + guardSize;
    void* stack = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (stack == MAP_FAILED) {
        std::fprintf(stderr, "[tvos] could not allocate the %zu-byte game stack; "
                             "starting on the 1 MiB main-thread stack instead\n",
                     kGameStackSize);
        return g_launch.entry(g_launch.argc, g_launch.argv);
    }
    // Lowest page faults on overflow instead of corrupting the mapping below it.
    if (mprotect(stack, guardSize, PROT_NONE) != 0) {
        munmap(stack, totalSize);
        std::fprintf(stderr, "[tvos] could not guard the game stack; "
                             "starting on the 1 MiB main-thread stack instead\n");
        return g_launch.entry(g_launch.argc, g_launch.argv);
    }

    g_launch.gameStackPointer =
        mkw_co_init(static_cast<char*>(stack) + totalSize, &GameStackMain, &g_launch);
    mkw_co_switch(&g_launch.gameStackPointer, &g_launch.hostStackPointer);

    munmap(stack, totalSize);
    return g_launch.result;
}

} // namespace

int RunApplication(int argc, char** argv, EntryPoint entry) {
    g_launch.entry = entry;

    // Hints read when SDL brings up its subsystems inside aurora_initialize.
    //
    // The Siri Remote must not occupy a controller slot: as a joystick it would
    // be enumerated first and take GameCube port 1 from the player's real
    // controller. Without it SDL delivers the remote's presses as keys instead
    // (arrows, Select = Return, Menu = Escape, Play/Pause = Pause), which the
    // keyboard bindings and the settings bar already understand.
    SDL_SetHint(SDL_HINT_TV_REMOTE_AS_JOYSTICK, "0");
    // Controller buttons must reach the game, never UIKit navigation (which
    // would send the app to the background on B / Menu).
    SDL_SetHint(SDL_HINT_APPLE_TV_CONTROLLER_UI_EVENTS, "0");
    // A mixable session so music started in another app keeps playing; music
    // ducking (external_audio_tvos.mm) then silences the game's soundtrack.
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "ambient");

    return SDL_RunApp(argc, argv, &RunOnGameStack, nullptr);
}

} // namespace RuntimeTvos

#endif // MKW_HOST_TVOS

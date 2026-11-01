// tvOS replacement for src/steam_input_layout.cpp.
//
// The desktop implementation asks a running Steam client to swap controller
// layouts by launching a steam:// URL through posix_spawn, which tvOS prohibits
// (and there is no Steam client on Apple TV). The interface stays callable so
// main.cpp and aurora_events.h need no platform branches.

#include "platform/apple_target.h"

#if MKW_HOST_TVOS

#include "steam_input_layout.h"

namespace SteamInputLayout {

void Initialize(bool enabled, uint32_t appId) {
    static_cast<void>(enabled);
    static_cast<void>(appId);
}

void HandleEvents(const AuroraEvent* events) {
    static_cast<void>(events);
}

void Release() noexcept {}

} // namespace SteamInputLayout

#endif // MKW_HOST_TVOS

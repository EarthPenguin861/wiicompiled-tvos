#pragma once

#include <string_view>

// `--race[=<course id>]`: boot straight into a 1P VS race instead of the title screen.
// `--race-cpu[=<local players>]`: the same, with the local racers driven by the CPU; 2 to 4 local
// players boot the matching split-screen VS race (for benchmarking multiplayer).
namespace BootRace {
// False when `arg` is not a boot-race option; throws on a malformed one.
bool ParseArgument(std::string_view arg);
bool Requested();
} // namespace BootRace

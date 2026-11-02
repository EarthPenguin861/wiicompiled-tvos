// The Siri Remote as a steering wheel during races (see platform/siri_remote.h).

#include "platform/apple_target.h"

#if MKW_HOST_TVOS

#include "platform/siri_remote.h"

#include "guest_abi/guest_region.h"
#include "memory.h"
#include "runtime_log.h"

#include <dolphin/pad.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace SiriRemote {
namespace {

constexpr float kRadiansToDegrees = 57.2957795f;
// Held sideways: gravity mostly along the remote's x axis. Entering needs the remote within about
// 37 degrees of sideways, leaving needs it past 60, so steering never drops out of the mode.
constexpr float kEnterSideways = 0.8f;
constexpr float kLeaveSideways = 0.5f;
// Tilting the face this far (about 27 degrees) aims the item backwards or forwards.
constexpr float kItemAim = 0.45f;
constexpr auto kTrickPress = std::chrono::milliseconds(120);
constexpr auto kTrickRearm = std::chrono::milliseconds(450);

struct Settings {
    bool enabled = true;
    float lockDegrees = 40.0f;
    float deadzoneDegrees = 4.0f;
    float shakeG = 1.3f;
    bool invert = false;
};

float EnvFloat(const char* name, float fallback, float low, float high) {
    if (const char* value = std::getenv(name)) {
        char* end = nullptr;
        const float parsed = std::strtof(value, &end);
        if (end != value && parsed >= low && parsed <= high) {
            return parsed;
        }
    }
    return fallback;
}

bool EnvFlag(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    return value == nullptr ? fallback : std::strcmp(value, "0") != 0;
}

const Settings& GetSettings() {
    static const Settings settings = [] {
        Settings s;
        s.enabled = EnvFlag("MKW_SIRI_REMOTE", true);
        s.lockDegrees = EnvFloat("MKW_SIRI_REMOTE_LOCK", 40.0f, 10.0f, 90.0f);
        s.deadzoneDegrees = EnvFloat("MKW_SIRI_REMOTE_DEADZONE", 4.0f, 0.0f, 30.0f);
        s.shakeG = EnvFloat("MKW_SIRI_REMOTE_SHAKE", 1.3f, 0.3f, 5.0f);
        s.invert = EnvFlag("MKW_SIRI_REMOTE_INVERT", false);
        return s;
    }();
    return settings;
}

using Clock = std::chrono::steady_clock;

bool g_sideways = false;   // the remote is held sideways (with hysteresis)
bool g_driving = false;    // the remote drives port 1 this frame
bool g_suspended = false;  // Menu paused the race; released when the remote is held upright
bool g_inRace = false;
bool g_previousMenu = false;
bool g_loggedDriving = false;
Clock::time_point g_trickUntil{};
Clock::time_point g_trickRearm{};

// Raceinfo::sInstance is set by Raceinfo::CreateInstance when a race scene starts and cleared by
// DestroyInstance when it ends. Its stage (offset 0x28, what Raceinfo::IsAtLeastStage compares):
// 0 intro, 1 countdown, 2 racing, 3 and up finished.
enum class RaceStage { None, Starting, Racing, Finished };

RaceStage CurrentRaceStage() noexcept {
    try {
        const uint32_t raceinfo = Memory::Read32(MKW_GADDR(809BD730));
        if (raceinfo == 0) {
            return RaceStage::None;
        }
        const auto stage = static_cast<int32_t>(Memory::Read32(raceinfo + 0x28));
        return stage < 2 ? RaceStage::Starting : stage == 2 ? RaceStage::Racing : RaceStage::Finished;
    } catch (...) {
        return RaceStage::None;
    }
}

// Steering angle in degrees, positive when the remote is turned clockwise (seen from the player),
// with the remote held like a wheel: face toward the player, touch surface at either end.
float SteeringDegrees(const Sample& sample) noexcept {
    const float side = sample.gravity[0] >= 0.0f ? 1.0f : -1.0f;
    return std::atan2(side * sample.gravity[1], side * sample.gravity[0]) * kRadiansToDegrees;
}

int8_t StickFromDegrees(float degrees, const Settings& settings) noexcept {
    const float magnitude = std::max(0.0f, std::fabs(degrees) - settings.deadzoneDegrees);
    const float span = std::max(1.0f, settings.lockDegrees - settings.deadzoneDegrees);
    const float amount = std::min(1.0f, magnitude / span);
    return static_cast<int8_t>(std::lround((degrees < 0.0f ? -amount : amount) * 127.0f));
}

} // namespace

bool Driving() noexcept {
    return g_driving;
}

bool OwnsMenuButton() noexcept {
    return g_inRace && (g_driving || g_suspended);
}

void Apply(PADStatus* statuses) noexcept {
    const Settings& settings = GetSettings();
    Sample sample;
    if (statuses == nullptr || !settings.enabled) {
        g_driving = false;
        return;
    }
    Poll(sample);
    const RaceStage stage = CurrentRaceStage();
    // After the finish the results and menus are driven by the remote's keys again; holding A
    // there would click straight through them.
    g_inRace = stage == RaceStage::Starting || stage == RaceStage::Racing;
    if (!sample.remote || sample.gameController || !g_inRace) {
        g_driving = false;
        g_suspended = false;
        g_previousMenu = sample.menu;
        return;
    }

    const float sideways = std::fabs(sample.gravity[0]);
    g_sideways = g_sideways ? sideways > kLeaveSideways : sideways > kEnterSideways;
    if (!g_sideways) {
        g_suspended = false;
    }
    const bool menuPressed = sample.menu && !g_previousMenu;
    g_previousMenu = sample.menu;
    g_driving = g_sideways && !g_suspended;
    if (!g_driving) {
        return;
    }
    if (!g_loggedDriving) {
        g_loggedDriving = true;
        RT_LOGF("siri-remote", "the Siri Remote is driving (held sideways during a race)\n");
    }

    // While held as a wheel the remote is port 1, whatever its keys or a keyboard mapped there.
    PADStatus& pad = statuses[0];
    std::memset(&pad, 0, sizeof(pad));
    pad.err = PAD_ERR_NONE;

    float degrees = SteeringDegrees(sample);
    if (settings.invert) {
        degrees = -degrees;
    }
    pad.stickX = StickFromDegrees(degrees, settings);
    if (sample.gravity[2] < -kItemAim) {
        pad.stickY = -100;  // face tilted up: aim back
    } else if (sample.gravity[2] > kItemAim) {
        pad.stickY = 100;   // face tilted down: aim forward
    }

    if (sample.click) {
        pad.button |= PAD_BUTTON_B;
        pad.analogB = 255;
    } else if (stage == RaceStage::Racing) {
        // Not during the countdown: accelerating too early there makes the kart burn out.
        pad.button |= PAD_BUTTON_A;
        pad.analogA = 255;
    }
    if (sample.playPause) {
        pad.button |= PAD_TRIGGER_L;
        pad.triggerL = 255;
    }
    if (sample.touching && !sample.click) {
        pad.button |= PAD_TRIGGER_R;
        pad.triggerR = 255;
    }

    // Shake: a short D-pad Up press, which the game takes as a trick (a wheelie on a bike), like
    // the shake-to-trick path for game controllers with motion sensors.
    const auto now = Clock::now();
    const float* user = sample.userAcceleration;
    const float shake = std::sqrt(user[0] * user[0] + user[1] * user[1] + user[2] * user[2]);
    if (shake > settings.shakeG && now >= g_trickRearm) {
        g_trickUntil = now + kTrickPress;
        g_trickRearm = now + kTrickRearm;
    }
    if (now < g_trickUntil) {
        pad.button |= PAD_BUTTON_UP;
    }

    if (menuPressed) {
        // Pause, then let go of port 1 so the pause menu works with the remote's keys.
        pad.button = PAD_BUTTON_START;
        pad.stickX = 0;
        pad.stickY = 0;
        pad.analogA = pad.analogB = pad.triggerL = pad.triggerR = 0;
        g_suspended = true;
        g_driving = false;
    }
}

} // namespace SiriRemote

#endif // MKW_HOST_TVOS

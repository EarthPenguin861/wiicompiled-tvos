// The Siri Remote in the game: menu buttons, and a steering wheel during races
// (see platform/siri_remote.h).

#include "platform/apple_target.h"

#if MKW_HOST_TVOS

#include "platform/siri_remote.h"

#include "guest_abi/guest_region.h"
#include "input_bindings.h"
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
// Held sideways: the long axis close to level (gravity along it under 0.5 g, about 30 degrees), and
// not lying flat (some gravity along the short axis), however far the face is tilted back. It
// stays sideways until the long axis passes about 58 degrees, so hard steering never drops out of
// the mode, or until the remote is laid flat.
constexpr float kEnterLongAxis = 0.5f;
constexpr float kLeaveLongAxis = 0.85f;
constexpr float kEnterShortAxis = 0.3f;
constexpr float kLeaveShortAxis = 0.15f;
// Tilting the face this far (about 20 degrees) from where it was when the wheel was taken aims the
// item backwards or forwards.
constexpr float kItemAim = 0.35f;
constexpr auto kTrickPress = std::chrono::milliseconds(120);
constexpr auto kTrickRearm = std::chrono::milliseconds(450);
// A swipe is the finger moving this far (the surface spans -1..1) from where it last stepped.
constexpr float kSwipeStep = 0.45f;
// A swipe holds its D-pad direction for this many PADReads, then releases it for at least one, so
// every swipe reaches the game as its own press.
constexpr int kSwipePressReads = 2;
constexpr int kSwipeGapReads = 1;

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

struct State {
    bool remote = false;          // a remote with motion sensors was seen on the last read
    bool sideways = false;        // held sideways (with hysteresis)
    bool grabbed = false;         // a click while sideways in a race handed it the wheel
    bool grabClickHeld = false;   // the grabbing click has not been released yet
    bool driving = false;         // it drives port 1 on this read
    bool suspended = false;       // Menu paused the race; released when it is held upright
    bool previousMenu = false;
    bool previousClick = false;
    bool menuConsumed = false;    // the Menu press that paused is held; it must not also go back
    bool loggedGrab = false;
    float aimBase = 0.0f;         // gravity along the face normal when the wheel was taken
    int diagnosticsLeft = 200;
    Clock::time_point nextDiagnostic{};
    Clock::time_point trickUntil{};
    Clock::time_point trickRearm{};
    // Swipes (menu mode).
    bool touchActive = false;
    float anchorX = 0.0f;
    float anchorY = 0.0f;
    uint16_t swipeButton = 0;
    int swipeReadsLeft = 0;
    int swipeGapLeft = 0;
};

State g_state;

// Which way round it is held sideways: +1 or -1 by the sign of gravity along the short axis.
float Side(const Sample& sample) noexcept {
    return sample.gravity[0] >= 0.0f ? 1.0f : -1.0f;
}

// Steering angle in degrees, positive when the remote is turned clockwise (seen from the player),
// with the remote held like a wheel, touch surface at either end: how far the long axis is tilted
// from level. That does not depend on how far the face is tilted back toward the ceiling.
float SteeringDegrees(const Sample& sample) noexcept {
    const float along = std::clamp(Side(sample) * sample.gravity[1], -1.0f, 1.0f);
    return std::asin(along) * kRadiansToDegrees;
}

int8_t StickFromDegrees(float degrees, const Settings& settings) noexcept {
    const float magnitude = std::max(0.0f, std::fabs(degrees) - settings.deadzoneDegrees);
    const float span = std::max(1.0f, settings.lockDegrees - settings.deadzoneDegrees);
    const float amount = std::min(1.0f, magnitude / span);
    return static_cast<int8_t>(std::lround((degrees < 0.0f ? -amount : amount) * 127.0f));
}

void ResetSwipe() noexcept {
    g_state.touchActive = false;
    g_state.swipeButton = 0;
    g_state.swipeReadsLeft = 0;
    g_state.swipeGapLeft = 0;
}

// One D-pad press per swipe step, from the finger's movement since the last step. The touch
// surface reports positions in the upright remote's frame; held sideways they are turned into the
// player's, so a swipe toward the TV's right is Right whichever way round the remote is.
uint16_t SwipeButtons(Sample sample) noexcept {
    if (g_state.sideways) {
        const float side = Side(sample);
        const float x = sample.touchX;
        const float y = sample.touchY;
        sample.touchX = side * y;
        sample.touchY = -side * x;
    }
    if (!sample.touching) {
        g_state.touchActive = false;
    } else if (!g_state.touchActive) {
        g_state.touchActive = true;
        g_state.anchorX = sample.touchX;
        g_state.anchorY = sample.touchY;
    } else if (g_state.swipeReadsLeft == 0 && g_state.swipeGapLeft == 0) {
        const float dx = sample.touchX - g_state.anchorX;
        const float dy = sample.touchY - g_state.anchorY;
        if (std::max(std::fabs(dx), std::fabs(dy)) >= kSwipeStep) {
            if (std::fabs(dx) > std::fabs(dy)) {
                g_state.swipeButton = dx > 0.0f ? PAD_BUTTON_RIGHT : PAD_BUTTON_LEFT;
            } else {
                g_state.swipeButton = dy > 0.0f ? PAD_BUTTON_UP : PAD_BUTTON_DOWN;
            }
            g_state.swipeReadsLeft = kSwipePressReads;
            g_state.anchorX = sample.touchX;
            g_state.anchorY = sample.touchY;
        }
    }
    if (g_state.swipeReadsLeft > 0) {
        if (--g_state.swipeReadsLeft == 0) {
            g_state.swipeGapLeft = kSwipeGapReads;
        }
        return g_state.swipeButton;
    }
    if (g_state.swipeGapLeft > 0) {
        --g_state.swipeGapLeft;
    }
    return 0;
}

// Menu mode: the remote's presses are added to port 1, beside whatever controller is there.
// `menuButton` is B (back) in menus and the pause menu, Start (pause) during a race.
void ApplyMenuButtons(PADStatus& pad, const Sample& sample, uint16_t menuButton) noexcept {
    uint16_t buttons = SwipeButtons(sample);
    if (sample.click) {
        buttons |= PAD_BUTTON_A;
        pad.analogA = 255;
    }
    if (sample.menu && !g_state.menuConsumed) {
        buttons |= menuButton;
        if (menuButton == PAD_BUTTON_B) {
            pad.analogB = 255;
        }
    }
    pad.button |= buttons;
}

// While a remote is connected, port 1 always reads as a connected controller. With nothing else
// there it would otherwise read as unplugged between presses, and the game stops for
// "communication interrupted" every time the port drops out.
void KeepConnected(PADStatus& pad) noexcept {
    if (pad.err != PAD_ERR_NONE) {
        std::memset(&pad, 0, sizeof(pad));
        pad.err = PAD_ERR_NONE;
    }
}

// Wheel mode: the remote replaces port 1.
void ApplyWheel(PADStatus& pad, const Sample& sample, RaceStage stage, const Settings& settings) noexcept {
    std::memset(&pad, 0, sizeof(pad));
    pad.err = PAD_ERR_NONE;

    float degrees = SteeringDegrees(sample);
    if (settings.invert) {
        degrees = -degrees;
    }
    pad.stickX = StickFromDegrees(degrees, settings);
    const float aim = sample.gravity[2] - g_state.aimBase;
    if (aim < -kItemAim) {
        pad.stickY = -100;  // face tilted further up: aim back
    } else if (aim > kItemAim) {
        pad.stickY = 100;   // face tilted down: aim forward
    }

    if (!sample.click) {
        g_state.grabClickHeld = false;
    }
    if (sample.click && !g_state.grabClickHeld) {
        pad.button |= PAD_BUTTON_B;
        pad.analogB = 255;
    } else if (!sample.click && stage == RaceStage::Racing) {
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
    if (shake > settings.shakeG && now >= g_state.trickRearm) {
        g_state.trickUntil = now + kTrickPress;
        g_state.trickRearm = now + kTrickRearm;
    }
    if (now < g_state.trickUntil) {
        pad.button |= PAD_BUTTON_UP;
    }
}

} // namespace

bool Driving() noexcept {
    return g_state.driving;
}

bool OwnsMenuButton() noexcept {
    return g_state.remote;
}

void Apply(PADStatus* statuses) noexcept {
    const Settings& settings = GetSettings();
    g_state.driving = false;
    if (statuses == nullptr || !settings.enabled) {
        g_state.remote = false;
        return;
    }
    Sample sample;
    Poll(sample);
    g_state.remote = sample.remote;
    if (sample.remote && g_state.diagnosticsLeft > 0 && Clock::now() >= g_state.nextDiagnostic) {
        // A short trace for tuning: what the motion sensors report and what the remote is doing.
        --g_state.diagnosticsLeft;
        g_state.nextDiagnostic = Clock::now() + std::chrono::seconds(3);
        RT_LOGF("siri-remote",
                "gravity %.2f %.2f %.2f accel %.2f %.2f %.2f sideways %d grabbed %d driving %d "
                "stage %d touch %.2f %.2f\n",
                sample.gravity[0], sample.gravity[1], sample.gravity[2], sample.userAcceleration[0],
                sample.userAcceleration[1], sample.userAcceleration[2], g_state.sideways ? 1 : 0,
                g_state.grabbed ? 1 : 0, g_state.driving ? 1 : 0,
                static_cast<int>(CurrentRaceStage()), sample.touchX, sample.touchY);
    }
    const bool clickPressed = sample.click && !g_state.previousClick;
    const bool menuPressed = sample.menu && !g_state.previousMenu;
    g_state.previousClick = sample.click;
    g_state.previousMenu = sample.menu;
    if (!sample.menu) {
        g_state.menuConsumed = false;
    }
    if (sample.remote) {
        KeepConnected(statuses[0]);
    }
    // The settings bar, a prompt or the startup screen own the input while they are open.
    if (!sample.remote || InputBindings::InputBlocked()) {
        g_state.grabbed = false;
        ResetSwipe();
        return;
    }

    const RaceStage stage = CurrentRaceStage();
    // After the finish the results and menus belong to menu mode again; holding A there would
    // click straight through them.
    const bool inRace = stage == RaceStage::Starting || stage == RaceStage::Racing;
    const float longAxis = std::fabs(sample.gravity[1]);
    const float shortAxis = std::fabs(sample.gravity[0]);
    g_state.sideways = g_state.sideways
                           ? longAxis < kLeaveLongAxis && shortAxis > kLeaveShortAxis
                           : longAxis < kEnterLongAxis && shortAxis > kEnterShortAxis;
    if (!g_state.sideways || !inRace) {
        g_state.grabbed = false;
        g_state.suspended = false;
    }

    PADStatus& pad = statuses[0];
    const auto pause = [&pad] {
        // Pause, then hand the remote to the pause menu (menu mode) until it is held upright.
        // The press stays consumed while held, or its next read would go back out of the menu.
        std::memset(&pad, 0, sizeof(pad));
        pad.err = PAD_ERR_NONE;
        pad.button = PAD_BUTTON_START;
        g_state.suspended = true;
        g_state.grabbed = false;
        g_state.driving = false;
        g_state.menuConsumed = true;
    };
    if (inRace && g_state.sideways && !g_state.suspended) {
        // Held like a wheel: a click takes over port 1 (a remote resting on its side never does),
        // and the remote keeps it until it is held upright or laid flat, or Menu pauses.
        if (!g_state.grabbed) {
            if (menuPressed) {
                pause();
                return;
            }
            if (!clickPressed) {
                return;
            }
            g_state.grabbed = true;
            g_state.grabClickHeld = true;
            g_state.aimBase = sample.gravity[2];
            ResetSwipe();
            if (!g_state.loggedGrab) {
                g_state.loggedGrab = true;
                RT_LOGF("siri-remote", "the Siri Remote took the wheel\n");
            }
        }
        g_state.driving = true;
        ApplyWheel(pad, sample, stage, settings);
        if (menuPressed) {
            pause();
        }
        return;
    }
    if (inRace && !g_state.suspended && menuPressed) {
        pause();
        return;
    }
    ApplyMenuButtons(pad, sample, PAD_BUTTON_B);
}

} // namespace SiriRemote

#endif // MKW_HOST_TVOS

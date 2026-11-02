#pragma once

#include <cstdint>

struct PADStatus;

namespace Bluetooth {
class RemoteHost;
}

// The first-generation Siri Remote (the one with motion sensors) in the game on Apple TV.
//
// Menu mode (everywhere the remote is not driving): its presses are added to GameCube port 1,
// beside any controller there: swipe = D-pad (one step per swipe), click = A, Menu = B (back), or
// Start (pause) during a race. Play/Pause keeps opening the settings bar.
//
// Held sideways (touch surface at the left or right end, face toward you or tilted back), swipes
// follow the TV: a swipe to the right is Right.
//
// Wheel mode, during a race: hold the remote sideways like a Wii Remote in a Wii Wheel and click
// once to take the wheel. It then replaces port 1, even with a controller
// connected, however far it is turned, until Menu pauses or the race ends:
//   tilt (rotate it like a wheel)   steer
//   nothing                         accelerate (held automatically once the race has started)
//   click the touch surface         brake / reverse
//   Play/Pause                      use item; tilt the face further up to throw back, down to
//                                   throw forward
//   rest a finger on the surface    hop / drift
//   shake                           trick off a ramp, or a wheelie on a bike
//   Menu                            pause; the remote then works the pause menu (Menu resumes,
//                                   a click chooses), and the next click while sideways takes
//                                   the wheel again
// A remote resting on its side never takes over: the wheel needs the click.
//
// MKW_SIRI_REMOTE=0 turns it off; MKW_SIRI_REMOTE_LOCK (degrees of tilt for full lock, default
// 60), MKW_SIRI_REMOTE_DEADZONE (degrees, default 6), MKW_SIRI_REMOTE_SHAKE (g, default 1.3) and
// MKW_SIRI_REMOTE_INVERT=1 tune it.
namespace SiriRemote {

// One poll of the remote and the controllers around it (src/platform/tvos/siri_remote_tvos.mm).
struct Sample {
    bool remote = false;               // a Siri Remote with motion sensors is connected
    bool gameController = false;       // a full game controller is connected too
    float gravity[3] = {};             // remote frame: x right, y toward the touch surface end,
                                       // z out of the face; g units
    float userAcceleration[3] = {};    // acceleration without gravity, g units
    bool hasRotationRate = false;      // the remote reports its gyroscope
    float rotationRate[3] = {};        // radians per second about the axes above
    bool click = false;                // touch surface pressed
    bool playPause = false;
    bool menu = false;
    bool touching = false;             // a finger rests on the touch surface
    float touchX = 0.0f;               // finger position, -1 (left) .. 1 (right), remote upright
    float touchY = 0.0f;               // -1 (bottom) .. 1 (top)
};
void Poll(Sample& sample) noexcept;

// Call once per guest PADRead, after the other GameCube input sources have been merged.
void Apply(PADStatus* statuses) noexcept;

// True while the remote drives port 1; its Play/Pause (the Pause key) is then an item, not the
// settings bar toggle.
bool Driving() noexcept;

// True while a remote with motion sensors is connected: its Menu button is the game's Back/Pause,
// so its Escape key press must not open the settings overlay's exit prompt.
bool OwnsMenuButton() noexcept;

// Wii Remote mode ([controller] siri_remote = "wiimote" in Config.toml, or MKW_SIRI_REMOTE=wiimote):
// the remote is a virtual Wii Remote on the game's own Bluetooth stack instead of GameCube port 1
// (src/platform/tvos/siri_wiimote_tvos.cpp). Held upright, tilting it moves the pointer (up and
// down by pointing, left and right by twisting, or by turning it when the remote has a gyroscope):
//   click = A, Menu = B, swipe = +Control Pad, Play/Pause = settings bar.
// Held sideways (touch surface on the left, like the Wii Remote's pointer end) it is a Wii Wheel:
// the game steers and does tricks from its own motion sensing; 2 (accelerate) is held once the
// race has started, click = 1 (brake), finger on the surface = B (drift), Play/Pause = +Control
// Pad (item), Menu = + (pause); in the pause menu swipe and click choose.
bool WiimoteMode() noexcept;

// The virtual Wii Remote behind slot 1 of the emulated Bluetooth controller; null outside Wii
// Remote mode (Wiimote::Host() hands it to the controller).
Bluetooth::RemoteHost* WiimoteHost() noexcept;
// True while the virtual Wii Remote is a Wii Wheel in a race (Play/Pause is then the item).
bool WiimoteDriving() noexcept;
// True while the virtual Wii Remote's Siri Remote is connected.
bool WiimotePresent() noexcept;

// The current race's stage (Raceinfo::sInstance->stage): -1 outside a race, 0 intro,
// 1 countdown, 2 racing, 3 and up finished.
int RaceStageNow() noexcept;

} // namespace SiriRemote

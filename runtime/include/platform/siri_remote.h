#pragma once

#include <cstdint>

struct PADStatus;

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
// connected, until it is held upright or laid flat, or Menu pauses:
//   tilt (rotate it like a wheel)   steer
//   nothing                         accelerate (held automatically once the race has started)
//   click the touch surface         brake / reverse
//   Play/Pause                      use item; tilt the face further up to throw back, down to
//                                   throw forward
//   rest a finger on the surface    hop / drift
//   shake                           trick off a ramp, or a wheelie on a bike
//   Menu                            pause; the remote then works the pause menu until it has been
//                                   held upright once
// A remote resting on its side never takes over: the wheel needs the click.
//
// MKW_SIRI_REMOTE=0 turns it off; MKW_SIRI_REMOTE_LOCK (degrees of tilt for full lock, default
// 40), MKW_SIRI_REMOTE_DEADZONE (degrees, default 4), MKW_SIRI_REMOTE_SHAKE (g, default 1.3) and
// MKW_SIRI_REMOTE_INVERT=1 tune it.
namespace SiriRemote {

// One poll of the remote and the controllers around it (src/platform/tvos/siri_remote_tvos.mm).
struct Sample {
    bool remote = false;               // a Siri Remote with motion sensors is connected
    bool gameController = false;       // a full game controller is connected too
    float gravity[3] = {};             // remote frame: x right, y toward the touch surface end,
                                       // z out of the face; g units
    float userAcceleration[3] = {};    // acceleration without gravity, g units
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

} // namespace SiriRemote

#pragma once

#include <cstdint>

struct PADStatus;

// The first-generation Siri Remote (the one with motion sensors) as a steering wheel on Apple TV.
//
// Held sideways during a race, like a Wii Remote in a Wii Wheel, it drives GameCube port 1:
//   tilt (rotate it like a wheel)   steer
//   nothing                         accelerate (held automatically once the race has started)
//   click the touch surface         brake / reverse
//   Play/Pause                      use item; tilt the face up to throw back, down to throw forward
//   rest a finger on the surface    hop / drift
//   shake                           trick off a ramp, or a wheelie on a bike
//   Menu                            pause, and hand the remote back to the menus
// Upright, outside a race, or once the race is finished, it keeps working as before (its presses
// arrive as arrow, Return, Escape and Pause keys). Pausing with Menu releases the wheel until the
// remote has been turned upright once, so the pause menu can be used with swipes and clicks. A
// connected game controller turns the wheel off.
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
};
void Poll(Sample& sample) noexcept;

// Call once per guest PADRead, after the other GameCube input sources have been merged.
void Apply(PADStatus* statuses) noexcept;

// True while the remote drives port 1; its Play/Pause (the Pause key) is then an item, not the
// settings bar toggle.
bool Driving() noexcept;

// True while the remote owns its Menu button (driving, or paused from driving); its Escape key
// press must then not reach the settings overlay.
bool OwnsMenuButton() noexcept;

} // namespace SiriRemote

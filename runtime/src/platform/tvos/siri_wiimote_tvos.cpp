// The Siri Remote as a virtual Wii Remote (see platform/siri_remote.h, WiimoteMode).
//
// This is the remote's side of the Wii Remote HID protocol for slot 1 of the emulated Bluetooth
// controller (bluetooth/bt_controller.h): data reporting modes, status, acknowledgements, EEPROM
// and register reads and writes, and the IR camera's view of a sensor bar placed from the remote's
// tilt. The game's own WPAD and KPAD do the rest: pointer, Wii Wheel steering and shake tricks.
// Everything runs on the game thread, which both drives the Bluetooth controller and polls the
// remote.

#include "platform/apple_target.h"

#if MKW_HOST_TVOS

#include "platform/siri_remote.h"

#include "bluetooth/bt_controller.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace SiriRemote {
namespace {

using Clock = std::chrono::steady_clock;
constexpr float kRadiansToDegrees = 57.2957795f;

// Core buttons as the two button bytes of an input report, first byte high.
constexpr uint16_t kLeft = 0x0100;
constexpr uint16_t kRight = 0x0200;
constexpr uint16_t kDown = 0x0400;
constexpr uint16_t kUp = 0x0800;
constexpr uint16_t kPlus = 0x1000;
constexpr uint16_t kTwo = 0x0001;
constexpr uint16_t kOne = 0x0002;
constexpr uint16_t kB = 0x0004;
constexpr uint16_t kA = 0x0008;

// Accelerometer: 10-bit counts, 0x200 at 0 g and 0x268 at 1 g, matching the calibration block
// in the EEPROM below.
constexpr int kAccelZero = 0x200;
constexpr int kAccelOneG = 0x68;

// The IR camera sees 1024x768; the sensor bar's two LEDs are drawn this far either side of the
// point the remote aims at.
constexpr int kCameraWidth = 1024;
constexpr int kCameraHeight = 768;
constexpr int kDotHalfSpacing = 100;
constexpr int kDotSize = 3;

// Pointer reach: this much tilt moves it from the centre to the edge of the screen.
constexpr float kPitchRangeDegrees = 20.0f;
constexpr float kRollRangeDegrees = 30.0f;
// People point at a TV with the remote's end raised; the centre then follows the hand whenever
// it tilts past an edge, so any comfortable hold works.
constexpr float kPitchCentreDegrees = 25.0f;
constexpr float kYawRangeDegrees = 25.0f;

// Same sideways test as the GameCube wheel (siri_remote_wheel_tvos.cpp).
constexpr float kEnterLongAxis = 0.5f;
constexpr float kLeaveLongAxis = 0.85f;
constexpr float kEnterShortAxis = 0.3f;
constexpr float kLeaveShortAxis = 0.15f;
constexpr float kItemAim = 0.35f;

constexpr float kSwipeStep = 0.45f;
constexpr auto kSwipePress = std::chrono::milliseconds(50);
constexpr auto kSwipeGap = std::chrono::milliseconds(30);
constexpr auto kReportInterval = std::chrono::milliseconds(10);
constexpr auto kPollInterval = std::chrono::milliseconds(4);

bool EnvFlag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "0") != 0;
}

struct Output {
    std::vector<uint8_t> bytes;
};

class VirtualWiimote final : public Bluetooth::RemoteHost {
public:
    VirtualWiimote() {
        // Accelerometer calibration, stored twice like on a real remote: zero-g X Y Z, their low
        // bits, one-g X Y Z, their low bits, motor/volume, checksum.
        const uint8_t calibration[9] = {0x80, 0x80, 0x80, 0x00, 0x9A, 0x9A, 0x9A, 0x00, 0x40};
        uint8_t checksum = 0x55;
        for (uint8_t byte : calibration) {
            checksum = static_cast<uint8_t>(checksum + byte);
        }
        for (size_t base : {size_t{0x16}, size_t{0x20}}) {
            std::memcpy(&m_eeprom[base], calibration, sizeof(calibration));
            m_eeprom[base + 9] = checksum;
        }
        m_invertX = EnvFlag("MKW_SIRI_POINTER_INVERT_X");
        m_invertY = EnvFlag("MKW_SIRI_POINTER_INVERT_Y");
        m_invertTilt = EnvFlag("MKW_SIRI_REMOTE_INVERT");
    }

    bool IsAttached(size_t slot) const override {
        return slot == 0 && m_seen;
    }

    bool ConsumeWake(size_t slot) override {
        if (slot != 0) return false;
        PollIfDue();
        const bool wake = m_wake;
        m_wake = false;
        return wake;
    }

    bool PopInputReport(size_t slot, std::vector<uint8_t>& report) override {
        if (slot != 0) return false;
        if (!m_pending.empty()) {
            report = std::move(m_pending.front().bytes);
            m_pending.pop_front();
            return true;
        }
        PollIfDue();
        const auto now = Clock::now();
        const bool changed = m_buttons != m_lastSentButtons;
        const bool streaming = m_continuous || m_mode != 0x30;
        if (!changed && (!streaming || now - m_lastReport < kReportInterval)) {
            return false;
        }
        m_lastReport = now;
        m_lastSentButtons = m_buttons;
        report = DataReport();
        return true;
    }

    void SendOutputReport(size_t slot, const uint8_t* report, size_t size) override {
        if (slot != 0 || size < 2) return;
        const uint8_t id = report[0];
        const uint8_t* p = report + 1;
        const size_t n = size - 1;
        const bool ack = (p[0] & 0x02) != 0;
        switch (id) {
        case 0x10: // rumble
            break;
        case 0x11: // player LEDs
            m_leds = static_cast<uint8_t>(p[0] >> 4);
            if (ack) Ack(id, 0);
            break;
        case 0x12: // data reporting mode
            if (n >= 2) {
                m_continuous = (p[0] & 0x04) != 0;
                m_mode = p[1];
                if (!m_loggedMode || m_mode != m_loggedModeValue) {
                    m_loggedMode = true;
                    m_loggedModeValue = m_mode;
                    RT_LOGF("siri-remote", "Wii Remote: the game set report mode 0x%02x%s\n", m_mode,
                            m_continuous ? " (continuous)" : "");
                }
            }
            if (ack) Ack(id, 0);
            break;
        case 0x13: // IR camera pixel clock
            m_irClock = (p[0] & 0x04) != 0;
            if (ack) Ack(id, 0);
            break;
        case 0x1A: // IR camera logic
            m_irLogic = (p[0] & 0x04) != 0;
            if (ack) Ack(id, 0);
            break;
        case 0x14: // speaker enable
            m_speaker = (p[0] & 0x04) != 0;
            if (ack) Ack(id, 0);
            break;
        case 0x19: // speaker mute
            if (ack) Ack(id, 0);
            break;
        case 0x15: // status request
            QueueStatus();
            break;
        case 0x16: // write memory or registers
            if (n >= 6) {
                const uint32_t address = (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
                const size_t length = std::min<size_t>(p[4], std::min<size_t>(16, n - 5));
                Write(p[0], address, p + 5, length);
            }
            Ack(id, 0);
            break;
        case 0x17: // read memory or registers
            if (n >= 6) {
                const uint32_t address = (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
                const uint32_t length = (uint32_t{p[4]} << 8) | p[5];
                QueueRead(p[0], address, length);
            }
            break;
        case 0x18: // speaker data
            break;
        default:
            break;
        }
    }

    void OnLinkStateChanged(size_t slot, Bluetooth::LinkState state) override {
        if (slot != 0) return;
        if (state == Bluetooth::LinkState::Down) {
            ResetProtocol();
        } else if (state == Bluetooth::LinkState::Ready && !m_loggedReady) {
            m_loggedReady = true;
            RT_LOGF("siri-remote", "Wii Remote: the game connected the Siri Remote as Wii Remote 1\n");
        }
    }

    void OnGuestDisconnect(size_t slot) override {
        if (slot != 0) return;
        // A real remote powers off here; the next press wakes it.
        m_asleep = true;
        RT_LOGF("siri-remote", "Wii Remote: the game disconnected it; press a button to reconnect\n");
    }

    void OnGuestInquiry(bool) override {}

    const char* DeviceName(size_t) const override {
        return "Nintendo RVL-CNT-01";
    }

    bool Driving() const noexcept { return m_driving; }
    bool Present() const noexcept { return m_present; }

private:
    void ResetProtocol() {
        m_mode = 0x30;
        m_continuous = false;
        m_irClock = false;
        m_irLogic = false;
        m_speaker = false;
        m_pending.clear();
    }

    void Ack(uint8_t reportId, uint8_t error) {
        m_pending.push_back({{0x22, ButtonsHigh(), ButtonsLow(), reportId, error}});
    }

    void QueueStatus() {
        uint8_t flags = static_cast<uint8_t>(m_leds << 4);
        if (m_speaker) flags |= 0x04;
        if (m_irClock && m_irLogic) flags |= 0x08;
        m_pending.push_back({{0x20, ButtonsHigh(), ButtonsLow(), flags, 0x00, 0x00, 0xC0}});
    }

    static bool IsRegisterSpace(uint8_t space) {
        return (space & 0x04) != 0;
    }

    void Write(uint8_t space, uint32_t address, const uint8_t* data, size_t length) {
        if (!IsRegisterSpace(space)) {
            for (size_t i = 0; i < length; ++i) {
                if (address + i < m_eeprom.size()) m_eeprom[address + i] = data[i];
            }
            return;
        }
        std::array<uint8_t, 0x100>* bank = Bank(address >> 16);
        if (bank == nullptr) return;
        const uint32_t offset = address & 0xFFFF;
        for (size_t i = 0; i < length; ++i) {
            if (offset + i < bank->size()) (*bank)[offset + i] = data[i];
        }
    }

    // Register banks the game uses: speaker (0xA2), extension (0xA4), IR camera (0xB0).
    std::array<uint8_t, 0x100>* Bank(uint32_t high) {
        switch (high & 0xFE) {
        case 0xA2: return &m_speakerRegs;
        case 0xA4: return &m_extensionRegs;
        case 0xB0: return &m_cameraRegs;
        default: return nullptr;
        }
    }

    void QueueRead(uint8_t space, uint32_t address, uint32_t length) {
        uint8_t error = 0;
        std::vector<uint8_t> data(length, 0);
        if (!IsRegisterSpace(space)) {
            if (address + length > m_eeprom.size()) {
                error = 8;
            } else {
                std::memcpy(data.data(), &m_eeprom[address], length);
            }
        } else if (((address >> 16) & 0xFE) == 0xA4) {
            error = 7; // no extension plugged in
        } else if (std::array<uint8_t, 0x100>* bank = Bank(address >> 16)) {
            const uint32_t offset = address & 0xFFFF;
            for (uint32_t i = 0; i < length; ++i) {
                if (offset + i < bank->size()) data[i] = (*bank)[offset + i];
            }
        }
        if (error != 0 || length == 0) {
            m_pending.push_back({{0x21, ButtonsHigh(), ButtonsLow(), static_cast<uint8_t>(0xF0 | error),
                                  static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address),
                                  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}});
            return;
        }
        for (uint32_t done = 0; done < length; done += 16) {
            const uint32_t chunk = std::min<uint32_t>(16, length - done);
            const uint32_t at = address + done;
            std::vector<uint8_t> report = {0x21, ButtonsHigh(), ButtonsLow(),
                                           static_cast<uint8_t>(((chunk - 1) << 4) | 0),
                                           static_cast<uint8_t>(at >> 8), static_cast<uint8_t>(at)};
            report.insert(report.end(), data.begin() + done, data.begin() + done + chunk);
            report.resize(22, 0);
            m_pending.push_back({std::move(report)});
        }
    }

    uint8_t ButtonsHigh() const { return static_cast<uint8_t>(m_buttons >> 8); }
    uint8_t ButtonsLow() const { return static_cast<uint8_t>(m_buttons & 0xFF); }

    std::vector<uint8_t> DataReport() const {
        if (m_mode == 0x3D) {
            std::vector<uint8_t> extensionOnly(22, 0);
            extensionOnly[0] = 0x3D;
            return extensionOnly;
        }
        std::vector<uint8_t> report = {m_mode, ButtonsHigh(), ButtonsLow()};
        const auto accel = [&] {
            report[1] = static_cast<uint8_t>(report[1] | ((m_accel[0] & 3) << 5));
            report[2] = static_cast<uint8_t>(report[2] | (((m_accel[1] >> 1) & 1) << 5) |
                                             (((m_accel[2] >> 1) & 1) << 6));
            for (int axis = 0; axis < 3; ++axis) {
                report.push_back(static_cast<uint8_t>(m_accel[axis] >> 2));
            }
        };
        const auto extension = [&](size_t count) { report.insert(report.end(), count, 0); };
        switch (m_mode) {
        case 0x31: accel(); break;
        case 0x32: extension(8); break;
        case 0x33: accel(); IrExtended(report); break;
        case 0x34: extension(19); break;
        case 0x35: accel(); extension(16); break;
        case 0x36: IrBasic(report); extension(9); break;
        case 0x37: accel(); IrBasic(report); extension(6); break;
        default: report[0] = 0x30; break;
        }
        return report;
    }

    bool CameraOn() const { return m_irClock && m_irLogic; }

    // The two sensor bar LEDs as the camera sees them, or none when it cannot see the bar.
    int Dots(int (&x)[2], int (&y)[2]) const {
        if (!CameraOn() || !m_pointerVisible) return 0;
        const int cx = static_cast<int>(std::lround(kCameraWidth / 2 - m_pointerX * 380.0f));
        const int cy = static_cast<int>(std::lround(kCameraHeight / 2 + m_pointerY * 300.0f));
        int count = 0;
        for (int side : {-1, 1}) {
            const int dx = cx + side * kDotHalfSpacing;
            if (dx < 0 || dx >= kCameraWidth || cy < 0 || cy >= kCameraHeight) continue;
            x[count] = dx;
            y[count] = cy;
            ++count;
        }
        return count;
    }

    void IrExtended(std::vector<uint8_t>& report) const {
        int x[2] = {};
        int y[2] = {};
        const int count = Dots(x, y);
        for (int i = 0; i < 4; ++i) {
            if (i < count) {
                report.push_back(static_cast<uint8_t>(x[i]));
                report.push_back(static_cast<uint8_t>(y[i]));
                report.push_back(static_cast<uint8_t>((((y[i] >> 8) & 3) << 6) | (((x[i] >> 8) & 3) << 4) |
                                                      kDotSize));
            } else {
                report.insert(report.end(), 3, 0xFF);
            }
        }
    }

    void IrBasic(std::vector<uint8_t>& report) const {
        int x[2] = {};
        int y[2] = {};
        const int count = Dots(x, y);
        if (count == 2) {
            report.push_back(static_cast<uint8_t>(x[0]));
            report.push_back(static_cast<uint8_t>(y[0]));
            report.push_back(static_cast<uint8_t>((((y[0] >> 8) & 3) << 6) | (((x[0] >> 8) & 3) << 4) |
                                                  (((y[1] >> 8) & 3) << 2) | ((x[1] >> 8) & 3)));
            report.push_back(static_cast<uint8_t>(x[1]));
            report.push_back(static_cast<uint8_t>(y[1]));
        } else {
            report.insert(report.end(), 5, 0xFF);
        }
        report.insert(report.end(), 5, 0xFF);
    }

    void PollIfDue() {
        const auto now = Clock::now();
        if (now - m_lastPoll < kPollInterval) return;
        const float dt = m_lastPoll == Clock::time_point{}
                             ? 0.0f
                             : std::chrono::duration<float>(now - m_lastPoll).count();
        m_lastPoll = now;
        Sample sample;
        Poll(sample);
        if (!sample.remote) {
            // Keep the Wii Remote connected while the Siri Remote naps; it just reads idle.
            m_present = false;
            m_buttons = 0;
            m_driving = false;
            return;
        }
        if (!m_seen) {
            m_seen = true;
            m_wake = true;
            RT_LOGF("siri-remote", "Wii Remote mode: the Siri Remote is Wii Remote 1 (gyroscope %s)\n",
                    sample.hasRotationRate ? "available" : "not reported");
        }
        m_present = true;
        Update(sample, now, dt);
    }

    float Side(const Sample& sample) const { return sample.gravity[0] >= 0.0f ? 1.0f : -1.0f; }

    void Update(const Sample& sample, Clock::time_point now, float dt) {
        const bool clickPressed = sample.click && !m_previousClick;
        const bool menuPressed = sample.menu && !m_previousMenu;
        const bool anyPress = clickPressed || menuPressed || (sample.playPause && !m_previousPlay);
        m_previousClick = sample.click;
        m_previousMenu = sample.menu;
        m_previousPlay = sample.playPause;
        if (m_asleep && anyPress) {
            m_asleep = false;
            m_wake = true;
        }

        const float longAxis = std::fabs(sample.gravity[1]);
        const float shortAxis = std::fabs(sample.gravity[0]);
        m_sideways = m_sideways ? longAxis < kLeaveLongAxis && shortAxis > kLeaveShortAxis
                                : longAxis < kEnterLongAxis && shortAxis > kEnterShortAxis;
        const int stage = RaceStageNow();
        const bool inRace = stage >= 0 && stage <= 2;
        if (!inRace) m_suspended = false;
        const bool wasDriving = m_driving;
        // Once it is a wheel it stays one for the race, however far it is turned, until Menu
        // pauses.
        m_driving = inRace && !m_suspended && (m_driving || m_sideways);
        if (m_driving && !wasDriving) {
            m_aimBase = sample.gravity[2];
            m_driveSide = Side(sample);
        }

        UpdateAccelerometer(sample);
        UpdatePointer(sample, dt);

        // Buttons. A press keeps the button it was given while it is held, so a mode change in
        // the middle of a press never turns it into another button.
        if (clickPressed) {
            if (m_driving) {
                m_clickLatch = kOne;
            } else {
                m_clickLatch = static_cast<uint16_t>(kA | (m_sideways ? kTwo : 0));
                m_suspended = false; // every pause menu choice leaves the pause menu
            }
        }
        if (menuPressed) {
            if (m_driving || (inRace && !m_suspended)) {
                m_menuLatch = kPlus;
                m_suspended = true;
            } else if (m_suspended) {
                m_menuLatch = kPlus;
                m_suspended = false;
            } else {
                m_menuLatch = static_cast<uint16_t>(kB | (m_sideways ? kOne : 0));
            }
        }
        uint16_t buttons = 0;
        if (sample.click) buttons |= m_clickLatch;
        if (sample.menu) buttons |= m_menuLatch;
        if (m_driving) {
            if (!sample.click && stage == 2) buttons |= kTwo;
            if (sample.touching && !sample.click) buttons |= kB;
            if (sample.playPause) {
                // The +Control Pad uses the item; toward the player's back throws it backwards.
                buttons |= sample.gravity[2] - m_aimBase < -kItemAim ? kLeft : kRight;
            }
            m_touchActive = false;
        } else {
            buttons |= Swipe(sample, now);
        }
        m_buttons = buttons;

        if (m_diagnosticsLeft > 0 && now >= m_nextDiagnostic) {
            --m_diagnosticsLeft;
            m_nextDiagnostic = now + std::chrono::seconds(3);
            RT_LOGF("siri-remote",
                    "wiimote gravity %.2f %.2f %.2f rate %.2f %.2f %.2f pointer %.2f %.2f sideways %d "
                    "driving %d stage %d buttons %04x mode %02x camera %d\n",
                    sample.gravity[0], sample.gravity[1], sample.gravity[2], sample.rotationRate[0],
                    sample.rotationRate[1], sample.rotationRate[2], m_pointerX, m_pointerY, m_sideways ? 1 : 0,
                    m_driving ? 1 : 0, stage, m_buttons, m_mode, CameraOn() ? 1 : 0);
        }
    }

    // What the Wii Remote's accelerometer would read, in its frame: +X left, +Y toward the pointer
    // end (the Siri Remote's touch surface), +Z out of the face. An accelerometer at rest reads the
    // opposite of gravity, which is how GameController reports it.
    void UpdateAccelerometer(const Sample& sample) {
        float force[3];
        for (int axis = 0; axis < 3; ++axis) {
            force[axis] = sample.gravity[axis] + sample.userAcceleration[axis];
        }
        float x = force[0];
        float y = -force[1];
        const float z = -force[2];
        if (m_driving && m_driveSide > 0.0f) {
            // Held sideways the other way round: present it as a Wii Wheel held the usual way.
            x = -x;
            y = -y;
        }
        if (m_invertTilt) x = -x;
        const float values[3] = {x, y, z};
        for (int axis = 0; axis < 3; ++axis) {
            const int counts = kAccelZero + static_cast<int>(std::lround(values[axis] * kAccelOneG));
            m_accel[axis] = std::clamp(counts, 0, 1023);
        }
    }

    void UpdatePointer(const Sample& sample, float dt) {
        m_pointerVisible = !m_sideways && !m_driving;
        if (!m_pointerVisible) return;
        const float pitch = std::asin(std::clamp(-sample.gravity[1], -1.0f, 1.0f)) * kRadiansToDegrees;
        float x;
        const bool gyro = sample.hasRotationRate &&
                          (sample.rotationRate[0] != 0.0f || sample.rotationRate[1] != 0.0f ||
                           sample.rotationRate[2] != 0.0f);
        if (gyro) {
            // Turning left is a positive rotation about the face's axis; the edges of the screen
            // absorb the gyroscope's drift.
            x = m_pointerX - sample.rotationRate[2] * dt * kRadiansToDegrees / kYawRangeDegrees;
        } else {
            const float roll = std::asin(std::clamp(sample.gravity[0], -1.0f, 1.0f)) * kRadiansToDegrees;
            x = Follow(roll, m_rollCentre, kRollRangeDegrees);
        }
        float y = Follow(pitch, m_pitchCentre, kPitchRangeDegrees);
        if (m_invertX) x = -x;
        if (m_invertY) y = -y;
        x = std::clamp(x, -1.0f, 1.0f);
        y = std::clamp(y, -1.0f, 1.0f);
        // A little smoothing against hand tremor.
        m_pointerX = gyro ? x : m_pointerX + (x - m_pointerX) * 0.5f;
        m_pointerY = m_pointerY + (y - m_pointerY) * 0.5f;
    }

    // Maps an angle to -1..1 around a centre that is dragged along when the angle passes an edge,
    // the way a mouse pointer stops at the edge of the screen.
    static float Follow(float degrees, float& centre, float range) {
        if (degrees > centre + range) centre = degrees - range;
        if (degrees < centre - range) centre = degrees + range;
        return (degrees - centre) / range;
    }

    // Swipes are +Control Pad presses in the remote's own frame; the game turns them for a
    // sideways remote itself.
    uint16_t Swipe(const Sample& sample, Clock::time_point now) {
        if (!sample.touching) {
            m_touchActive = false;
        } else if (!m_touchActive) {
            m_touchActive = true;
            m_anchorX = sample.touchX;
            m_anchorY = sample.touchY;
        } else if (now >= m_swipeUntil + kSwipeGap) {
            const float dx = sample.touchX - m_anchorX;
            const float dy = sample.touchY - m_anchorY;
            if (std::max(std::fabs(dx), std::fabs(dy)) >= kSwipeStep) {
                if (std::fabs(dx) > std::fabs(dy)) {
                    m_swipeButton = dx > 0.0f ? kRight : kLeft;
                } else {
                    m_swipeButton = dy > 0.0f ? kUp : kDown;
                }
                m_swipeUntil = now + kSwipePress;
                m_anchorX = sample.touchX;
                m_anchorY = sample.touchY;
            }
        }
        return now < m_swipeUntil ? m_swipeButton : 0;
    }

    std::array<uint8_t, 0x1700> m_eeprom{};
    std::array<uint8_t, 0x100> m_speakerRegs{};
    std::array<uint8_t, 0x100> m_extensionRegs{};
    std::array<uint8_t, 0x100> m_cameraRegs{};
    std::deque<Output> m_pending;

    uint8_t m_mode = 0x30;
    bool m_continuous = false;
    bool m_irClock = false;
    bool m_irLogic = false;
    bool m_speaker = false;
    uint8_t m_leds = 0;
    bool m_loggedMode = false;
    uint8_t m_loggedModeValue = 0;
    bool m_loggedReady = false;

    bool m_seen = false;
    bool m_present = false;
    bool m_wake = false;
    bool m_asleep = false;
    Clock::time_point m_lastPoll{};
    Clock::time_point m_lastReport{};
    uint16_t m_buttons = 0;
    uint16_t m_lastSentButtons = 0;
    int m_accel[3] = {kAccelZero, kAccelZero, kAccelZero + kAccelOneG};

    bool m_previousClick = false;
    bool m_previousMenu = false;
    bool m_previousPlay = false;
    uint16_t m_clickLatch = 0;
    uint16_t m_menuLatch = 0;
    bool m_sideways = false;
    bool m_driving = false;
    bool m_suspended = false;
    float m_aimBase = 0.0f;
    float m_driveSide = -1.0f;

    bool m_pointerVisible = false;
    float m_pointerX = 0.0f;
    float m_pointerY = 0.0f;
    float m_pitchCentre = kPitchCentreDegrees;
    float m_rollCentre = 0.0f;
    bool m_invertX = false;
    bool m_invertY = false;
    bool m_invertTilt = false;

    bool m_touchActive = false;
    float m_anchorX = 0.0f;
    float m_anchorY = 0.0f;
    uint16_t m_swipeButton = 0;
    Clock::time_point m_swipeUntil{};

    int m_diagnosticsLeft = 200;
    Clock::time_point m_nextDiagnostic{};
};

VirtualWiimote* Instance() {
    static VirtualWiimote* instance = WiimoteMode() ? new VirtualWiimote() : nullptr;
    return instance;
}

} // namespace

bool WiimoteMode() noexcept {
    static const bool enabled = [] {
        if (const char* value = std::getenv("MKW_SIRI_REMOTE")) {
            if (std::strcmp(value, "wiimote") == 0) return true;
            if (std::strcmp(value, "gamecube") == 0 || std::strcmp(value, "0") == 0) return false;
        }
        return RuntimeConfigFile::SiriRemoteMode() == "wiimote";
    }();
    return enabled;
}

Bluetooth::RemoteHost* WiimoteHost() noexcept {
    return Instance();
}

bool WiimoteDriving() noexcept {
    const VirtualWiimote* wiimote = Instance();
    return wiimote != nullptr && wiimote->Driving();
}

bool WiimotePresent() noexcept {
    const VirtualWiimote* wiimote = Instance();
    return wiimote != nullptr && wiimote->Present();
}

} // namespace SiriRemote

#endif // MKW_HOST_TVOS

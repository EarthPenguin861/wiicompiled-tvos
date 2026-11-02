// Reads the Siri Remote through the GameController framework (see platform/siri_remote.h).
//
// SDL is told not to open the remote as a joystick (tvos_app.cpp), so its presses keep arriving as
// keys; this file only polls the remote's motion, buttons and touch surface for the wheel mode.
// Polled from PADRead on the game thread, which on tvOS is the main thread.

#include "platform/apple_target.h"

#if MKW_HOST_TVOS

#include "platform/siri_remote.h"

#include <cstdio>

#import <GameController/GameController.h>

namespace SiriRemote {
namespace {

GCController* FindRemote(bool& gameController) {
    GCController* remote = nil;
    gameController = false;
    for (GCController* controller in [GCController controllers]) {
        if (controller.extendedGamepad != nil) {
            gameController = true;
        } else if (remote == nil && controller.microGamepad != nil && controller.motion != nil) {
            // Only the first-generation remote has motion sensors; later ones never match.
            remote = controller;
        }
    }
    return remote;
}

} // namespace

void Poll(Sample& sample) noexcept {
    sample = Sample{};
    @autoreleasepool {
        bool gameController = false;
        GCController* remote = FindRemote(gameController);
        sample.gameController = gameController;
        if (remote == nil) {
            return;
        }
        static __weak GCController* configured = nil;
        GCMicroGamepad* pad = remote.microGamepad;
        GCMotion* motion = remote.motion;
        if (configured != remote) {
            // Touch positions instead of relative swipes, in the remote's own frame.
            pad.reportsAbsoluteDpadValues = YES;
            pad.allowsRotation = NO;
            if (motion.sensorsRequireManualActivation) {
                motion.sensorsActive = YES;
            }
            // Some controllers only deliver fresh motion samples to an app that subscribed to them.
            if (motion.valueChangedHandler == nil) {
                motion.valueChangedHandler = ^(GCMotion*) {
                };
            }
            // The tvOS 11 flag the 2017 Siri Remote was announced with; the per-feature flags came later.
            // Asked at run time so the build doesn't depend on the SDK still declaring it.
            const bool attitudeAndRate =
                [motion respondsToSelector:NSSelectorFromString(@"hasAttitudeAndRotationRate")] &&
                [[motion valueForKey:@"hasAttitudeAndRotationRate"] boolValue];
            std::fprintf(stderr,
                         "[siri-remote] remote %s: gravity+acceleration %d, rotation rate %d, attitude %d, "
                         "attitude+rotation (tvOS 11 flag) %d, manual activation %d, active %d\n",
                         remote.vendorName.UTF8String ?: "?", motion.hasGravityAndUserAcceleration ? 1 : 0,
                         motion.hasRotationRate ? 1 : 0, motion.hasAttitude ? 1 : 0, attitudeAndRate ? 1 : 0,
                         motion.sensorsRequireManualActivation ? 1 : 0, motion.sensorsActive ? 1 : 0);
            configured = remote;
        }
        sample.remote = true;
        const GCAcceleration gravity = motion.gravity;
        const GCAcceleration user = motion.userAcceleration;
        sample.gravity[0] = static_cast<float>(gravity.x);
        sample.gravity[1] = static_cast<float>(gravity.y);
        sample.gravity[2] = static_cast<float>(gravity.z);
        sample.userAcceleration[0] = static_cast<float>(user.x);
        sample.userAcceleration[1] = static_cast<float>(user.y);
        sample.userAcceleration[2] = static_cast<float>(user.z);
        {
            // Read the gyroscope even when no flag admits to one: a remote that has none reports zeros,
            // which the pointer treats as "no gyroscope".
            const GCRotationRate rate = motion.rotationRate;
            sample.rotationRate[0] = static_cast<float>(rate.x);
            sample.rotationRate[1] = static_cast<float>(rate.y);
            sample.rotationRate[2] = static_cast<float>(rate.z);
            const bool moving = rate.x != 0.0 || rate.y != 0.0 || rate.z != 0.0;
            sample.hasRotationRate = motion.hasRotationRate || moving;
            static bool reported = false;
            if (moving && !reported) {
                reported = true;
                std::fprintf(stderr, "[siri-remote] gyroscope is live: rotation rate %.3f %.3f %.3f\n", rate.x,
                             rate.y, rate.z);
            }
        }
        sample.click = pad.buttonA.isPressed;
        sample.playPause = pad.buttonX.isPressed;
        sample.menu = pad.buttonMenu.isPressed;
        sample.touchX = pad.dpad.xAxis.value;
        sample.touchY = pad.dpad.yAxis.value;
        sample.touching = sample.touchX != 0.0f || sample.touchY != 0.0f;
    }
}

} // namespace SiriRemote

#endif // MKW_HOST_TVOS

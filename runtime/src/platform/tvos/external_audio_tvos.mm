// tvOS implementation of the external-audio probe behind music ducking
// (music_attenuation.cpp polls it every 250 ms on its own thread).
//
// The macOS probe (src/external_audio_macos.cpp) enumerates Core Audio process
// objects, an API that does not exist on tvOS. AVAudioSession answers the same
// question directly: secondaryAudioShouldBeSilencedHint is true while another
// app with a non-mixable session (Music, a video app playing over AirPlay...) is
// playing, which is Apple's documented signal for a game to silence its
// soundtrack. SDL opens the game's session with the mixable "ambient" category
// (see platform/tvos/tvos_app.cpp), so that other audio keeps playing and the
// hint becomes meaningful.

#include "external_audio_macos.h"

#import <AVFoundation/AVFoundation.h>

namespace MusicAttenuation {

MacOSAudioStatus QueryMacOSExternalAudio() noexcept {
    @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        if (session == nil) {
            return {};
        }
        MacOSAudioStatus status;
        status.available = true;
        status.playing = session.secondaryAudioShouldBeSilencedHint ? true : false;
        return status;
    }
}

} // namespace MusicAttenuation

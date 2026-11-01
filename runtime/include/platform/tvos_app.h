#pragma once

// Apple TV process entry. A tvOS app has to run inside UIApplicationMain, so
// main() hands control to UIKit through SDL_RunApp and the runtime starts once
// the application has finished launching - on the main thread, as SDL, UIKit
// and the guest scheduler all require.
namespace RuntimeTvos {

using EntryPoint = int (*)(int argc, char** argv);

// Never returns while the app is running; returns the entry point's exit code
// if it ever finishes.
int RunApplication(int argc, char** argv, EntryPoint entry);

} // namespace RuntimeTvos

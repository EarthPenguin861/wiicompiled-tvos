#pragma once

#include <exception>
#include <string>

// Apple TV process entry. A tvOS app has to run inside UIApplicationMain, so
// main() hands control to UIKit through SDL_RunApp and the runtime starts once
// the application has finished launching - on the main thread, as SDL, UIKit
// and the guest scheduler all require.
namespace RuntimeTvos {

using EntryPoint = int (*)(int argc, char** argv);

// Never returns while the app is running; returns the entry point's exit code
// if it ever finishes.
int RunApplication(int argc, char** argv, EntryPoint entry);

// Starts the built-in sampling profiler when the launch environment sets
// MKW_SAMPLE_PROFILE (src/platform/tvos/sample_profiler_tvos.cpp). Call on the
// main thread.
void StartSampleProfilerFromEnvironment();

// Name, reason and call stack of an Objective-C exception, or an empty string
// when `exception` is not one (src/platform/tvos/objc_exception_tvos.mm).
std::string DescribeObjCException(std::exception_ptr exception) noexcept;

} // namespace RuntimeTvos

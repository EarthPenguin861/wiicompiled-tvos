#pragma once

// One spelling of "is this an Apple TV build" for code that must not depend on a
// build-system definition (translated shards, tests and the runtime all include
// guest_abi headers that branch on it).
//
// TargetConditionals.h defines every TARGET_OS_* macro, as 0 or 1, on all Apple
// SDKs. TARGET_OS_TV is 1 for both the Apple TV device and the Apple TV
// Simulator; TARGET_OS_IPHONE is also 1 there, so never use TARGET_OS_IPHONE
// alone to mean "iPhone/iPad".
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && defined(TARGET_OS_TV) && TARGET_OS_TV
#define MKW_HOST_TVOS 1
#else
#define MKW_HOST_TVOS 0
#endif

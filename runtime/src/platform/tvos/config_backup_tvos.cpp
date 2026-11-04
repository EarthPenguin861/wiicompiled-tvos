// A copy of Config.toml in the app's preferences (NSUserDefaults, through CFPreferences).
//
// tvOS gives an app no persistent files: Config.toml lives in Library/Caches, which the system
// deletes when the Apple TV runs low on storage. The preferences survive that, so the runtime keeps
// the last good Config.toml there and restores it when the file has gone (runtime_config.h).

#include "platform/apple_target.h"

#if MKW_HOST_TVOS

#include "runtime_config.h"

#include <CoreFoundation/CoreFoundation.h>

#include <string>

namespace RuntimeConfigBackup {
namespace {

constexpr const char* kKey = "WiiCompiledConfigToml";

CFStringRef CreateKey() {
    return CFStringCreateWithCString(kCFAllocatorDefault, kKey, kCFStringEncodingUTF8);
}

} // namespace

bool Load(std::string& text) noexcept {
    const CFStringRef key = CreateKey();
    if (key == nullptr) {
        return false;
    }
    const CFPropertyListRef value = CFPreferencesCopyAppValue(key, kCFPreferencesCurrentApplication);
    CFRelease(key);
    if (value == nullptr) {
        return false;
    }
    bool loaded = false;
    if (CFGetTypeID(value) == CFDataGetTypeID()) {
        const auto data = static_cast<CFDataRef>(value);
        text.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                    static_cast<size_t>(CFDataGetLength(data)));
        loaded = !text.empty();
    }
    CFRelease(value);
    return loaded;
}

void Save(const std::string& text) noexcept {
    if (text.empty()) {
        return;
    }
    const CFStringRef key = CreateKey();
    const CFDataRef data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
                                        static_cast<CFIndex>(text.size()));
    if (key != nullptr && data != nullptr) {
        CFPreferencesSetAppValue(key, data, kCFPreferencesCurrentApplication);
        CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);
    }
    if (data != nullptr) {
        CFRelease(data);
    }
    if (key != nullptr) {
        CFRelease(key);
    }
}

} // namespace RuntimeConfigBackup

#endif // MKW_HOST_TVOS

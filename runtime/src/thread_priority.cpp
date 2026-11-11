#include "thread_priority.h"

#include "runtime_log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
#endif

namespace ThreadPriority {

#if defined(__APPLE__)
namespace {

struct State {
    int policy = -1;
    int schedPriority = -1;
    int qos = -1;
    int basePriority = -1;
    int currentPriority = -1;
};

State ReadState() {
    State state;
    sched_param param{};
    if (pthread_getschedparam(pthread_self(), &state.policy, &param) == 0) {
        state.schedPriority = param.sched_priority;
    }
    state.qos = static_cast<int>(qos_class_self());
    thread_extended_info_data_t info{};
    mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
    // pthread_mach_thread_np does not add a port reference, so there is nothing to release.
    if (thread_info(pthread_mach_thread_np(pthread_self()), THREAD_EXTENDED_INFO,
                    reinterpret_cast<thread_info_t>(&info), &count) == KERN_SUCCESS) {
        state.basePriority = info.pth_priority;
        state.currentPriority = info.pth_curpri;
    }
    return state;
}

const char* PolicyName(int policy) {
    switch (policy) {
    case SCHED_OTHER:
        return "other";
    case SCHED_RR:
        return "rr";
    case SCHED_FIFO:
        return "fifo";
    default:
        return "?";
    }
}

void LogState(const char* label, const char* when, const State& state) {
    RT_LOG(RT_TAG_CONFIG) << "thread priority [" << label << "] " << when << ": policy="
                          << PolicyName(state.policy) << " sched=" << state.schedPriority << " qos=0x"
                          << std::hex << state.qos << std::dec << " base=" << state.basePriority
                          << " current=" << state.currentPriority << std::endl;
}

} // namespace

void ApplyToCurrentThread(const char* label, const char* environmentVariable, const char* defaultSpec) {
    const char* fromEnvironment = environmentVariable != nullptr ? std::getenv(environmentVariable) : nullptr;
    const std::string spec = fromEnvironment != nullptr ? fromEnvironment : (defaultSpec != nullptr ? defaultSpec : "");
    LogState(label, "before", ReadState());
    if (spec.empty() || spec == "off") {
        return;
    }

    char* end = nullptr;
    const long requested = std::strtol(spec.c_str(), &end, 10);
    if (end == spec.c_str()) {
        RT_LOG(RT_TAG_CONFIG) << "thread priority [" << label << "] ignoring '" << spec << "'" << std::endl;
        return;
    }
    const int policy = std::strcmp(end, "rr") == 0 ? SCHED_RR : SCHED_OTHER;
    sched_param param{};
    param.sched_priority = static_cast<int>(std::clamp<long>(requested, sched_get_priority_min(policy),
                                                             sched_get_priority_max(policy)));
    // pthread_setschedparam takes the thread out of the QoS system for good. That is the point for
    // a thread SDL already took out of it (SDL_SetCurrentThreadPriority does the same call), where
    // pthread_set_qos_class_self_np can only fail with EPERM.
    const int result = pthread_setschedparam(pthread_self(), policy, &param);
    RT_LOG(RT_TAG_CONFIG) << "thread priority [" << label << "] set '" << spec << "' result=" << result
                          << std::endl;
    LogState(label, "after", ReadState());
}

#else

void ApplyToCurrentThread(const char*, const char*, const char*) {}

#endif

} // namespace ThreadPriority

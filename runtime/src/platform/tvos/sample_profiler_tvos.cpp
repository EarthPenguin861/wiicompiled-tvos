// Built-in sampling profiler for the Apple TV build.
//
// An Apple TV cannot run Instruments on a sideloaded app, and the game thread
// spends its time in ~28k translated functions, so "where does the frame go?"
// is answered from inside the process instead. Off unless the launch
// environment sets MKW_SAMPLE_PROFILE (e.g. through
// `pymobiledevice3 developer dvt launch ... --env MKW_SAMPLE_PROFILE=45:60:1000`):
//
//   MKW_SAMPLE_PROFILE=<delay seconds>:<duration seconds>:<samples per second>
//
// After the delay, a background thread repeatedly suspends every thread that
// used CPU time since the previous tick, copies its program counter and frame
// pointer chain, resumes it and appends the record to
//   Library/Caches/WiiCompiled/Logs/profile-<pid>.bin
// Nothing that can take a lock (malloc, stdio, the dyld lock) runs while a
// thread is suspended. A text summary with every thread's CPU time, its
// scheduling priority and the loaded images (to map addresses back to
// symbols offline) is written beside it as profile-<pid>.txt.
//
// File layout (little endian): "MKWPROF1", then records of
//   u32 tick, u16 thread slot, u16 frame count, frame count x u64 address
// where address 0 is the sampled program counter and address 1 the link register.

#include "platform/apple_target.h"

#if MKW_HOST_TVOS

#include "platform/tvos_app.h"

#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_info.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace RuntimeTvos {
namespace {

constexpr int kMaxFrames = 32;
constexpr size_t kMaxThreads = 128;

struct Config {
    double delaySeconds = 45.0;
    double durationSeconds = 60.0;
    double hertz = 1000.0;
};

struct ThreadSlot {
    thread_act_t port = MACH_PORT_NULL;
    uint64_t threadId = 0;
    uint64_t cpuMicrosAtStart = 0;
    uint64_t lastCpuMicros = 0;
    uint64_t samples = 0;
    bool seen = false;
    bool alive = false;
    char name[64] = {};
};

struct Region {
    uint64_t low = 0;
    uint64_t high = 0;
};

// The UIKit main thread, which also runs the guest CPU (see tvos_app.cpp).
uint64_t g_mainThreadId = 0;

uint64_t NowNanos() {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

uint64_t ThreadCpuMicros(thread_act_t thread, integer_t* runState = nullptr) {
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(thread, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0;
    }
    if (runState != nullptr) {
        *runState = info.run_state;
    }
    return static_cast<uint64_t>(info.user_time.seconds + info.system_time.seconds) * 1000000u +
           static_cast<uint64_t>(info.user_time.microseconds + info.system_time.microseconds);
}

uint64_t ThreadId(thread_act_t thread) {
    thread_identifier_info_data_t info{};
    mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
    if (thread_info(thread, THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0;
    }
    return info.thread_id;
}

class Profiler {
public:
    explicit Profiler(Config config) : m_config(config) {}

    void Run() {
        Sleep(m_config.delaySeconds);
        const std::string base = OutputBase();
        m_out = std::fopen((base + ".bin").c_str(), "wb");
        if (m_out == nullptr) {
            std::fprintf(stderr, "[profile] cannot write %s.bin\n", base.c_str());
            return;
        }
        static char buffer[1 << 18];
        std::setvbuf(m_out, buffer, _IOFBF, sizeof(buffer));
        std::fwrite("MKWPROF1", 1, 8, m_out);
        std::fprintf(stderr, "[profile] sampling %.0f s at %.0f Hz into %s.bin\n",
                     m_config.durationSeconds, m_config.hertz, base.c_str());

        m_self = mach_thread_self();
        const uint64_t start = NowNanos();
        const uint64_t end = start + static_cast<uint64_t>(m_config.durationSeconds * 1e9);
        const uint64_t period = static_cast<uint64_t>(1e9 / (m_config.hertz > 1 ? m_config.hertz : 1));
        uint64_t nextRefresh = 0;
        uint64_t next = start;
        uint32_t tick = 0;
        while (NowNanos() < end) {
            const uint64_t now = NowNanos();
            if (now >= nextRefresh) {
                RefreshThreads();
                nextRefresh = now + 100'000'000ull;
            }
            SampleOnce(tick++);
            next += period;
            const uint64_t after = NowNanos();
            if (next > after) {
                SleepNanos(next - after);
            } else {
                next = after;
            }
        }
        const uint64_t wallMicros = (NowNanos() - start) / 1000u;
        std::fclose(m_out);
        m_out = nullptr;
        WriteSummary(base + ".txt", wallMicros, tick);
        std::fprintf(stderr, "[profile] done: %u ticks, %llu records\n", tick,
                     static_cast<unsigned long long>(m_records));
        mach_port_deallocate(mach_task_self(), m_self);
    }

private:
    static void Sleep(double seconds) {
        if (seconds > 0) {
            SleepNanos(static_cast<uint64_t>(seconds * 1e9));
        }
    }

    static void SleepNanos(uint64_t nanos) {
        timespec ts{static_cast<time_t>(nanos / 1000000000ull), static_cast<long>(nanos % 1000000000ull)};
        while (nanosleep(&ts, &ts) != 0) {
        }
    }

    static std::string OutputBase() {
        const char* home = std::getenv("HOME");
        std::string dir = std::string(home != nullptr ? home : ".") + "/Library/Caches/WiiCompiled";
        mkdir(dir.c_str(), 0755);
        dir += "/Logs";
        mkdir(dir.c_str(), 0755);
        return dir + "/profile-" + std::to_string(static_cast<long>(getpid()));
    }

    void RefreshThreads() {
        thread_act_array_t threads = nullptr;
        mach_msg_type_number_t count = 0;
        if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
            return;
        }
        for (auto& slot : m_slots) {
            slot.alive = false;
        }
        for (mach_msg_type_number_t i = 0; i < count; ++i) {
            const thread_act_t thread = threads[i];
            const uint64_t id = ThreadId(thread);
            ThreadSlot* slot = FindSlot(id);
            if (slot == nullptr && m_slotCount < kMaxThreads && thread != m_self) {
                slot = &m_slots[m_slotCount++];
                slot->threadId = id;
                slot->port = thread;
                slot->cpuMicrosAtStart = ThreadCpuMicros(thread);
                slot->lastCpuMicros = slot->cpuMicrosAtStart;
                slot->seen = true;
                // Kept for the rest of the run: this slot now owns the port reference.
                slot->alive = true;
                ReadName(*slot);
                continue;
            }
            if (slot != nullptr) {
                slot->alive = true;
                if (slot->name[0] == '\0') {
                    ReadName(*slot);
                }
            }
            mach_port_deallocate(mach_task_self(), thread);
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
    }

    ThreadSlot* FindSlot(uint64_t id) {
        for (size_t i = 0; i < m_slotCount; ++i) {
            if (m_slots[i].threadId == id) {
                return &m_slots[i];
            }
        }
        return nullptr;
    }

    static void ReadName(ThreadSlot& slot) {
        pthread_t pthread = pthread_from_mach_thread_np(slot.port);
        if (pthread != nullptr) {
            pthread_getname_np(pthread, slot.name, sizeof(slot.name));
            if (slot.name[0] == '\0' && pthread_main_np() != 0 && pthread == pthread_self()) {
                std::snprintf(slot.name, sizeof(slot.name), "main");
            }
        }
    }

    // The VM region holding the stack pointer bounds the frame-pointer walk, so a
    // corrupt or foreign frame pointer can never make the sampler read unmapped
    // memory. Looked up for every sample (one cheap system call) rather than
    // cached: guest fiber stacks come and go during the run.
    static bool StackRegion(uint64_t sp, Region& region) {
        vm_address_t address = static_cast<vm_address_t>(sp);
        vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                           reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS) {
            return false;
        }
        if (address > sp || (info.protection & VM_PROT_READ) == 0) {
            return false;
        }
        region = {address, address + size};
        return true;
    }

    void SampleOnce(uint32_t tick) {
        for (size_t i = 0; i < m_slotCount; ++i) {
            ThreadSlot& slot = m_slots[i];
            if (!slot.alive) {
                continue;
            }
            integer_t runState = 0;
            const uint64_t cpu = ThreadCpuMicros(slot.port, &runState);
            const bool ran = cpu != slot.lastCpuMicros || runState == TH_STATE_RUNNING;
            slot.lastCpuMicros = cpu;
            if (!ran) {
                continue;
            }
            std::array<uint64_t, kMaxFrames> frames{};
            int depth = 0;
            if (thread_suspend(slot.port) != KERN_SUCCESS) {
                continue;
            }
            arm_thread_state64_t state{};
            mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
            if (thread_get_state(slot.port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count) ==
                KERN_SUCCESS) {
                frames[depth++] = arm_thread_state64_get_pc(state);
                // The link register is the caller of a leaf function that never
                // built a frame record; the analysis drops it again when it
                // repeats the first frame-pointer return address.
                frames[depth++] = arm_thread_state64_get_lr(state);
                const uint64_t sp = arm_thread_state64_get_sp(state);
                uint64_t fp = arm_thread_state64_get_fp(state);
                Region region{};
                if (StackRegion(sp, region)) {
                    while (depth < kMaxFrames && fp >= sp && fp >= region.low && fp + 16 <= region.high &&
                           (fp & 7u) == 0) {
                        const auto* record = reinterpret_cast<const uint64_t*>(fp);
                        const uint64_t nextFp = record[0];
                        const uint64_t returnAddress = record[1];
                        if (returnAddress == 0) {
                            break;
                        }
                        frames[depth++] = returnAddress;
                        if (nextFp <= fp) {
                            break;
                        }
                        fp = nextFp;
                    }
                }
            }
            thread_resume(slot.port);

            ++slot.samples;
            ++m_records;
            const uint32_t header[2] = {tick, static_cast<uint32_t>(i) | (static_cast<uint32_t>(depth) << 16)};
            std::fwrite(header, sizeof(header), 1, m_out);
            std::fwrite(frames.data(), sizeof(uint64_t), static_cast<size_t>(depth), m_out);
        }
    }

    void WriteSummary(const std::string& path, uint64_t wallMicros, uint32_t ticks) {
        FILE* out = std::fopen(path.c_str(), "w");
        if (out == nullptr) {
            return;
        }
        std::fprintf(out, "wall_us %llu ticks %u records %llu hz %.0f\n",
                     static_cast<unsigned long long>(wallMicros), ticks,
                     static_cast<unsigned long long>(m_records), m_config.hertz);
        std::fprintf(out, "# slot thread_id cpu_us cpu_pct samples base_prio cur_prio name\n");
        for (size_t i = 0; i < m_slotCount; ++i) {
            ThreadSlot& slot = m_slots[i];
            const uint64_t cpu = slot.alive ? ThreadCpuMicros(slot.port) : slot.lastCpuMicros;
            const uint64_t used = cpu >= slot.cpuMicrosAtStart ? cpu - slot.cpuMicrosAtStart : 0;
            thread_extended_info_data_t ext{};
            mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
            int basePriority = -1;
            int currentPriority = -1;
            if (slot.alive && thread_info(slot.port, THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&ext),
                                          &count) == KERN_SUCCESS) {
                basePriority = ext.pth_priority;
                currentPriority = ext.pth_curpri;
                if (slot.name[0] == '\0' && ext.pth_name[0] != '\0') {
                    std::snprintf(slot.name, sizeof(slot.name), "%s", ext.pth_name);
                }
            }
            std::fprintf(out, "thread %zu %llu %llu %.1f %llu %d %d %s\n", i,
                         static_cast<unsigned long long>(slot.threadId), static_cast<unsigned long long>(used),
                         wallMicros ? 100.0 * static_cast<double>(used) / static_cast<double>(wallMicros) : 0.0,
                         static_cast<unsigned long long>(slot.samples), basePriority, currentPriority,
                         slot.name[0] != '\0' ? slot.name : "-");
        }
        const uint32_t images = _dyld_image_count();
        for (uint32_t i = 0; i < images; ++i) {
            const char* name = _dyld_get_image_name(i);
            std::fprintf(out, "image 0x%llx 0x%llx %s\n",
                         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(_dyld_get_image_header(i))),
                         static_cast<unsigned long long>(_dyld_get_image_vmaddr_slide(i)),
                         name != nullptr ? name : "?");
        }
        std::fclose(out);
    }

    Config m_config;
    FILE* m_out = nullptr;
    thread_act_t m_self = MACH_PORT_NULL;
    std::array<ThreadSlot, kMaxThreads> m_slots{};
    size_t m_slotCount = 0;
    uint64_t m_records = 0;
};

bool ParseConfig(const char* text, Config& config) {
    if (text == nullptr || *text == '\0' || std::strcmp(text, "0") == 0) {
        return false;
    }
    double values[3] = {config.delaySeconds, config.durationSeconds, config.hertz};
    int index = 0;
    const char* cursor = text;
    while (*cursor != '\0' && index < 3) {
        char* end = nullptr;
        const double value = std::strtod(cursor, &end);
        if (end == cursor) {
            break;
        }
        values[index++] = value;
        cursor = *end == ':' ? end + 1 : end;
        if (*end != ':') {
            break;
        }
    }
    config.delaySeconds = values[0] < 0 ? 0 : values[0];
    config.durationSeconds = values[1] < 1 ? 1 : values[1];
    config.hertz = values[2] < 1 ? 1 : (values[2] > 4000 ? 4000 : values[2]);
    return true;
}

void* ProfilerThread(void* argument) {
    pthread_setname_np("mkw.sample-profiler");
    auto* profiler = static_cast<Profiler*>(argument);
    profiler->Run();
    delete profiler;
    return nullptr;
}

} // namespace

void StartSampleProfilerFromEnvironment() {
    static std::atomic<bool> started{false};
    Config config;
    {
        const thread_act_t self = mach_thread_self();
        g_mainThreadId = ThreadId(self);
        mach_port_deallocate(mach_task_self(), self);
    }
    if (!ParseConfig(std::getenv("MKW_SAMPLE_PROFILE"), config) || started.exchange(true)) {
        return;
    }
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    // Above the game's threads so a tick is taken on time even when both cores are busy.
    pthread_attr_set_qos_class_np(&attributes, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_t thread;
    auto* profiler = new Profiler(config);
    if (pthread_create(&thread, &attributes, &ProfilerThread, profiler) != 0) {
        delete profiler;
    }
    pthread_attr_destroy(&attributes);
}

} // namespace RuntimeTvos

#endif // MKW_HOST_TVOS

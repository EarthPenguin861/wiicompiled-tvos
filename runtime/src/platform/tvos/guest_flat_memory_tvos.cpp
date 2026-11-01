// tvOS backend for guest_flat_memory.h.
//
// The macOS backend (src/guest_flat_memory_macos.cpp) reserves a fixed 4 GiB
// guest view and remaps every region's backing store into it. A tvOS process
// cannot get that reservation (see guest_abi/flat_memory.h), so this backend
// keeps only the part the checked Memory::* path needs: one anonymous backing
// store per backing kind, with the cached/uncached/physical mirrors of MEM1 and
// MEM2 resolving to the same host bytes. HostPointer() therefore answers
// exactly what the macOS backend answers (it, too, hands out backing-store
// pointers, never the remapped view), and the layout validation is the same,
// so a layout that one backend rejects the other rejects too.
//
// RequiresCheckedAccess() is a compile-time true on tvOS, so nothing ever
// dereferences MKW_FLAT_GUEST_BASE and there are no page protections to manage:
// MMIO, deferred EFB reads and executable-write policy are all applied by the
// checked path itself.

#include "platform/apple_target.h"

#if MKW_HOST_TVOS

#include "guest_flat_memory.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

static_assert(GuestFlat::RequiresCheckedAccess(),
              "the tvOS guest memory backend has no flat view; every access must be checked");

namespace GuestFlat {
namespace {

struct TvosStore {
    Backing kind = Backing::Owned;
    uint32_t owned = 0;
    uint64_t size = 0;
    uint8_t* host = nullptr;
};

struct TvosMapping {
    uint32_t base = 0;
    uint64_t size = 0;
    uint8_t* host = nullptr;
};

std::mutex g_tvosMutex;
std::vector<TvosStore> g_tvosStores;
std::vector<TvosMapping> g_tvosMappings;
std::vector<RegionRequest> g_tvosLayout;
bool g_tvosInitialized = false;

inline uint64_t TvosRoundUp(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

// Offset of a region inside its backing store; identical to the macOS backend.
uint64_t TvosStoreOffset(const RegionRequest& r) {
    if (r.backing == Backing::Mem1) return r.base & 0x1fffffffu;
    if (r.backing == Backing::Mem2) return (r.base & 0x1fffffffu) - 0x10000000u;
    return 0;
}

bool TvosSameLayout(const std::vector<RegionRequest>& a, const std::vector<RegionRequest>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](const auto& x, const auto& y) { return x.base == y.base && x.size == y.size && x.backing == y.backing; });
}

} // namespace

// The flat path is never usable on tvOS.
bool IsActive() { return false; }

void Initialize(const std::vector<RegionRequest>& regions) {
    std::lock_guard lock(g_tvosMutex);
    const uint64_t pageSize = static_cast<uint64_t>(getpagesize());
    if (g_tvosInitialized) {
        if (!TvosSameLayout(g_tvosLayout, regions))
            throw std::runtime_error("flat guest layout cannot be remapped");
        return;
    }

    for (size_t i = 0; i < regions.size(); ++i) {
        const auto& r = regions[i];
        if (r.size == 0) continue;
        if (r.size > kGuestSpaceSize - r.base ||
            r.base % pageSize != 0 || TvosStoreOffset(r) % pageSize != 0) {
            throw std::runtime_error("invalid flat guest memory region");
        }
        const uint64_t end = static_cast<uint64_t>(r.base) + TvosRoundUp(r.size, pageSize);
        for (size_t j = 0; j < i; ++j) {
            const auto& prior = regions[j];
            if (prior.size == 0) continue;
            const uint64_t priorEnd = static_cast<uint64_t>(prior.base) + TvosRoundUp(prior.size, pageSize);
            if (r.base < priorEnd && prior.base < end) {
                throw std::runtime_error("overlapping flat guest memory regions");
            }
        }
    }

    std::vector<TvosStore> stores;
    std::vector<TvosMapping> mappings;
    try {
        for (const auto& r : regions) {
            if (!r.size) continue;
            const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
            auto it = std::find_if(stores.begin(), stores.end(), [&](const TvosStore& s) {
                return s.kind == r.backing && s.owned == owned;
            });
            const uint64_t need = TvosRoundUp(TvosStoreOffset(r) + r.size, pageSize);
            if (it == stores.end()) {
                stores.push_back({r.backing, owned, need, nullptr});
            } else {
                it->size = std::max(it->size, need);
            }
        }

        // Anonymous memory is zero-filled and only counts against the app's
        // memory footprint once touched, so sizing a store for its largest
        // mirror costs nothing up front.
        for (auto& s : stores) {
            void* host = mmap(nullptr, static_cast<size_t>(s.size), PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON, -1, 0);
            if (host == MAP_FAILED) {
                throw std::runtime_error("unable to allocate anonymous tvOS guest backing store of " +
                                         std::to_string(s.size) + " bytes");
            }
            s.host = static_cast<uint8_t*>(host);
        }

        for (const auto& r : regions) {
            if (!r.size) continue;
            const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
            const auto& s = *std::find_if(stores.begin(), stores.end(), [&](const TvosStore& x) {
                return x.kind == r.backing && x.owned == owned;
            });
            mappings.push_back({r.base, r.size, s.host + TvosStoreOffset(r)});
        }
    } catch (...) {
        for (auto& s : stores) {
            if (s.host != nullptr) {
                munmap(s.host, static_cast<size_t>(s.size));
            }
        }
        throw;
    }

    g_tvosLayout = regions;
    g_tvosStores = std::move(stores);
    g_tvosMappings = std::move(mappings);
    g_tvosInitialized = true;
}

uint8_t* HostPointer(uint32_t a) {
    for (const auto& m : g_tvosMappings) {
        if (a >= m.base && uint64_t(a - m.base) < m.size) {
            return m.host + (a - m.base);
        }
    }
    return nullptr;
}

// Without a flat view there is nothing to protect; the checked path consults the
// deferred-read and executable-range tables directly.
void ProtectDeferredRange(uint32_t, size_t) {}
void UnprotectDeferredRange(uint32_t, size_t) {}
void RegisterExecutableRange(uint32_t, uint32_t) {}
FaultCounters Counters() { return {}; }
void LogFaultSummary() noexcept {}
// No guest-space fault can occur, so every access violation is a genuine crash.
bool HandleAccessViolation(void*, bool) noexcept { return false; }

} // namespace GuestFlat

#endif // MKW_HOST_TVOS

// SPDX-FileCopyrightText: 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// HLE for the libSceLibcInternal pieces that are otherwise only available from a firmware dump:
//  - sceLibcMspace* : private heaps carved out of a caller-provided memory region
//                     (NpToolkit2 builds one on its flexible-memory pool and aborts on NULL)
//  - __cxa_guard_*  : thread-safe function-local static initialization (Itanium C++ ABI)
//  - _Getpctype     : Dinkumware ctype classification table

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#include "common/alignment.h"
#include "common/logging/log.h"
#include "core/libraries/libs.h"
#include "libc_internal_mspace.h"

namespace Libraries::LibcInternal {

namespace {

constexpr u64 MinAlign = 16;

struct Mspace {
    std::mutex lock;
    std::string name;
    u8* base = nullptr;
    u64 capacity = 0;
    bool owns_memory = false;
    u64 in_use = 0;
    u64 peak = 0;
    std::map<u64, u64> free_blocks;            // offset -> size, sorted for coalescing
    std::unordered_map<u64, u64> used_blocks;  // offset -> size

    void* Alloc(u64 size, u64 align) {
        size = Common::AlignUp(std::max<u64>(size, 1), MinAlign);
        align = std::max(align, MinAlign);
        for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it) {
            const u64 start = it->first;
            const u64 end = start + it->second;
            const u64 addr = Common::AlignUp(reinterpret_cast<u64>(base) + start, align) -
                             reinterpret_cast<u64>(base);
            if (addr + size > end) {
                continue;
            }
            free_blocks.erase(it);
            if (addr > start) {
                free_blocks.emplace(start, addr - start);
            }
            if (addr + size < end) {
                free_blocks.emplace(addr + size, end - (addr + size));
            }
            used_blocks.emplace(addr, size);
            in_use += size;
            peak = std::max(peak, in_use);
            return base + addr;
        }
        return nullptr;
    }

    // Returns the size of the freed block, 0 if ptr does not belong to this mspace.
    u64 Free(void* ptr) {
        const u64 off = static_cast<u8*>(ptr) - base;
        const auto used = used_blocks.find(off);
        if (used == used_blocks.end()) {
            return 0;
        }
        u64 start = off;
        u64 size = used->second;
        used_blocks.erase(used);
        in_use -= size;
        auto next = free_blocks.lower_bound(start);
        if (next != free_blocks.end() && next->first == start + size) {
            size += next->second;
            next = free_blocks.erase(next);
        }
        if (next != free_blocks.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == start) {
                start = prev->first;
                size += prev->second;
                free_blocks.erase(prev);
            }
        }
        free_blocks.emplace(start, size);
        return size;
    }

    u64 UsableSize(void* ptr) {
        const auto it = used_blocks.find(static_cast<u8*>(ptr) - base);
        return it == used_blocks.end() ? 0 : it->second;
    }
};

Mspace* AsMspace(void* msp) {
    return static_cast<Mspace*>(msp);
}

} // namespace

void* PS4_SYSV_ABI sceLibcMspaceCreate(const char* name, void* base, u64 capacity, u32 flag) {
    auto* msp = new Mspace();
    msp->name = name ? name : "";
    msp->capacity = capacity;
    if (base) {
        msp->base = static_cast<u8*>(base);
    } else {
        msp->base = static_cast<u8*>(::operator new(capacity, std::align_val_t{MinAlign}));
        msp->owns_memory = true;
    }
    // Reserve a small header area like dlmalloc does, so the heap never starts at `base`.
    constexpr u64 Header = 0x100;
    if (capacity > Header) {
        msp->free_blocks.emplace(Header, capacity - Header);
    }
    LOG_INFO(Lib_LibcInternal, "name = {}, base = {}, capacity = {:#x}, flag = {:#x} -> {}",
             msp->name, base, capacity, flag, static_cast<void*>(msp));
    return msp;
}

s32 PS4_SYSV_ABI sceLibcMspaceDestroy(void* msp) {
    auto* m = AsMspace(msp);
    if (!m) {
        return -1;
    }
    LOG_INFO(Lib_LibcInternal, "name = {}, in_use = {:#x}, peak = {:#x}", m->name, m->in_use,
             m->peak);
    if (m->owns_memory) {
        ::operator delete(m->base, std::align_val_t{MinAlign});
    }
    delete m;
    return 0;
}

void* PS4_SYSV_ABI sceLibcMspaceMalloc(void* msp, u64 size) {
    auto* m = AsMspace(msp);
    std::scoped_lock lk{m->lock};
    void* p = m->Alloc(size, MinAlign);
    if (!p) {
        LOG_ERROR(Lib_LibcInternal, "mspace {} out of memory: size = {:#x}, in_use = {:#x}",
                  m->name, size, m->in_use);
    }
    return p;
}

void* PS4_SYSV_ABI sceLibcMspaceMemalign(void* msp, u64 align, u64 size) {
    auto* m = AsMspace(msp);
    std::scoped_lock lk{m->lock};
    return m->Alloc(size, align);
}

s32 PS4_SYSV_ABI sceLibcMspacePosixMemalign(void* msp, void** out, u64 align, u64 size) {
    void* p = sceLibcMspaceMemalign(msp, align, size);
    if (!p) {
        return 12; // ENOMEM
    }
    *out = p;
    return 0;
}

void* PS4_SYSV_ABI sceLibcMspaceCalloc(void* msp, u64 count, u64 size) {
    const u64 total = count * size;
    if (size != 0 && total / size != count) {
        return nullptr;
    }
    void* p = sceLibcMspaceMalloc(msp, total);
    if (p) {
        std::memset(p, 0, total);
    }
    return p;
}

s32 PS4_SYSV_ABI sceLibcMspaceFree(void* msp, void* ptr) {
    if (!ptr) {
        return 0;
    }
    auto* m = AsMspace(msp);
    std::scoped_lock lk{m->lock};
    if (m->Free(ptr) == 0) {
        LOG_ERROR(Lib_LibcInternal, "mspace {}: free of unknown pointer {}", m->name, ptr);
    }
    return 0;
}

void* PS4_SYSV_ABI sceLibcMspaceReallocalign(void* msp, void* ptr, u64 align, u64 size) {
    if (!ptr) {
        return sceLibcMspaceMemalign(msp, align, size);
    }
    if (size == 0) {
        sceLibcMspaceFree(msp, ptr);
        return nullptr;
    }
    auto* m = AsMspace(msp);
    std::scoped_lock lk{m->lock};
    const u64 old_size = m->UsableSize(ptr);
    if (old_size >= size && (reinterpret_cast<u64>(ptr) % std::max(align, MinAlign)) == 0) {
        return ptr;
    }
    void* p = m->Alloc(size, align);
    if (!p) {
        return nullptr;
    }
    std::memcpy(p, ptr, std::min(old_size, size));
    m->Free(ptr);
    return p;
}

void* PS4_SYSV_ABI sceLibcMspaceRealloc(void* msp, void* ptr, u64 size) {
    return sceLibcMspaceReallocalign(msp, ptr, MinAlign, size);
}

u64 PS4_SYSV_ABI sceLibcMspaceMallocUsableSize(void* ptr) {
    // Not tied to an mspace argument on PS4; nothing in this game relies on it.
    LOG_WARNING(Lib_LibcInternal, "(PARTIAL) ptr = {}", ptr);
    return 0;
}

s32 PS4_SYSV_ABI sceLibcMspaceIsHeapEmpty(void* msp) {
    auto* m = AsMspace(msp);
    std::scoped_lock lk{m->lock};
    return m->used_blocks.empty() ? 1 : 0;
}

struct OrbisMallocManagedSize {
    u16 size;
    u16 version;
    u32 reserved1;
    u64 max_system_size;
    u64 current_system_size;
    u64 max_inuse_size;
    u64 current_inuse_size;
};

s32 PS4_SYSV_ABI sceLibcMspaceMallocStats(void* msp, OrbisMallocManagedSize* stats) {
    auto* m = AsMspace(msp);
    if (!m || !stats) {
        return 22; // EINVAL
    }
    std::scoped_lock lk{m->lock};
    stats->max_system_size = m->capacity;
    stats->current_system_size = m->capacity;
    stats->max_inuse_size = m->peak;
    stats->current_inuse_size = m->in_use;
    return 0;
}

// ---- Itanium C++ ABI static-init guards ----
// Byte 0 of the guard = "initialized", byte 1 = "initialization in progress".

namespace {
std::mutex g_guard_mutex;
std::condition_variable g_guard_cv;
} // namespace

s32 PS4_SYSV_ABI __cxa_guard_acquire(u64* guard) {
    auto* bytes = reinterpret_cast<u8*>(guard);
    if (std::atomic_ref<u8>(bytes[0]).load(std::memory_order_acquire) != 0) {
        return 0;
    }
    std::unique_lock lk{g_guard_mutex};
    g_guard_cv.wait(lk, [bytes] { return bytes[1] == 0; });
    if (bytes[0] != 0) {
        return 0;
    }
    bytes[1] = 1;
    return 1;
}

void PS4_SYSV_ABI __cxa_guard_release(u64* guard) {
    auto* bytes = reinterpret_cast<u8*>(guard);
    {
        std::scoped_lock lk{g_guard_mutex};
        bytes[1] = 0;
        std::atomic_ref<u8>(bytes[0]).store(1, std::memory_order_release);
    }
    g_guard_cv.notify_all();
}

void PS4_SYSV_ABI __cxa_guard_abort(u64* guard) {
    auto* bytes = reinterpret_cast<u8*>(guard);
    {
        std::scoped_lock lk{g_guard_mutex};
        bytes[1] = 0;
    }
    g_guard_cv.notify_all();
}

// ---- Dinkumware ctype table ----

namespace {
// Dinkumware classification bits.
constexpr s16 XD = 0x001, UP = 0x002, SP = 0x004, PU = 0x008, LO = 0x010, DI = 0x020,
              CN = 0x040, BB = 0x080;

struct CtypeTable {
    s16 table[257]{}; // [0] is EOF

    CtypeTable() {
        for (int c = 0; c < 128; ++c) {
            s16 v = 0;
            if (c < 0x20 || c == 0x7f) {
                v |= (c >= '\t' && c <= '\r') ? CN : BB;
            }
            if (c == ' ') {
                v |= SP;
            }
            if (c >= '0' && c <= '9') {
                v |= DI | XD;
            }
            if (c >= 'A' && c <= 'Z') {
                v |= UP | ((c <= 'F') ? XD : 0);
            }
            if (c >= 'a' && c <= 'z') {
                v |= LO | ((c <= 'f') ? XD : 0);
            }
            if (c > 0x20 && c < 0x7f && !(v & (DI | UP | LO))) {
                v |= PU;
            }
            table[c + 1] = v;
        }
    }
};
const CtypeTable g_ctype;
} // namespace

const s16* PS4_SYSV_ABI _Getpctype() {
    return &g_ctype.table[1];
}

void RegisterlibSceLibcInternalMspace(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("-hn1tcVHq5Q", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceCreate);
    LIB_FUNCTION("W6SiVSiCDtI", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceDestroy);
    LIB_FUNCTION("OJjm-QOIHlI", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceMalloc);
    LIB_FUNCTION("Vla-Z+eXlxo", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceFree);
    LIB_FUNCTION("LYo3GhIlB38", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceCalloc);
    LIB_FUNCTION("gigoVHZvVPE", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceRealloc);
    LIB_FUNCTION("iF1iQHzxBJU", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceMemalign);
    LIB_FUNCTION("qWESlyXMI3E", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspacePosixMemalign);
    LIB_FUNCTION("p6lrRW8-MLY", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceReallocalign);
    LIB_FUNCTION("fEoW6BJsPt4", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceMallocUsableSize);
    LIB_FUNCTION("pzUa7KEoydw", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceIsHeapEmpty);
    LIB_FUNCTION("mfHdJTIvhuo", "libSceLibcInternal", 1, "libSceLibcInternal",
                 sceLibcMspaceMallocStats);
    LIB_FUNCTION("3GPpjQdAMTw", "libSceLibcInternal", 1, "libSceLibcInternal",
                 __cxa_guard_acquire);
    LIB_FUNCTION("9rAeANT2tyE", "libSceLibcInternal", 1, "libSceLibcInternal",
                 __cxa_guard_release);
    LIB_FUNCTION("2emaaluWzUw", "libSceLibcInternal", 1, "libSceLibcInternal",
                 __cxa_guard_abort);
    LIB_FUNCTION("sUP1hBaouOw", "libSceLibcInternal", 1, "libSceLibcInternal", _Getpctype);
}

} // namespace Libraries::LibcInternal

// SPDX-FileCopyrightText: 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "common/logging/log.h"
#include "medievil/frame_trace.h"

namespace MediEvil::FrameTrace {

const bool g_enabled = [] {
    const char* env = std::getenv("MEDIEVIL_FRAMETRACE");
    return env && *env && *env != '0';
}();

std::array<std::atomic<u32>, Counter::Count> g_counters{};

namespace {
constexpr u32 RingSize = 96;
std::mutex g_ring_mutex;
DrawRecord g_ring[RingSize];
u32 g_ring_count = 0;
FILE* g_draw_file = nullptr;

void DumpRing(u32 flip_number) {
    std::scoped_lock lk{g_ring_mutex};
    if (!g_draw_file) {
        g_draw_file = std::fopen("medievil_draws.txt", "w");
        if (!g_draw_file) {
            return;
        }
    }
    const u32 n = g_ring_count < RingSize ? g_ring_count : RingSize;
    std::fprintf(g_draw_file, "F%u total %u\n", flip_number, g_ring_count);
    for (u32 i = 0; i < n; ++i) {
        const DrawRecord& r = g_ring[(g_ring_count - n + i) % RingSize];
        std::fprintf(g_draw_file, " ps %016llx", static_cast<unsigned long long>(r.ps_hash));
        for (u32 j = 0; j < r.num_images; ++j) {
            std::fprintf(g_draw_file, " %u@%llx", r.image_index[j],
                         static_cast<unsigned long long>(r.image_addr[j]));
        }
        std::fputc('\n', g_draw_file);
    }
    std::fflush(g_draw_file);
    g_ring_count = 0;
}
} // namespace

void RecordDraw(const DrawRecord& rec) {
    std::scoped_lock lk{g_ring_mutex};
    g_ring[g_ring_count % RingSize] = rec;
    ++g_ring_count;
}

void OnFlip(u32 flip_number, u32 draws, u32 dispatches) {
    if (!g_enabled) {
        return;
    }
    DumpRing(flip_number);
    u32 c[Counter::Count];
    for (u32 i = 0; i < Counter::Count; ++i) {
        c[i] = g_counters[i].exchange(0, std::memory_order_relaxed);
    }
    LOG_WARNING(Debug,
                "frametrace F{} draws {} disp {} filt {} nopipe {} bindfail {} img+{} -{} exp{} "
                "metadisp {} fce {} fillmeta {}",
                flip_number, draws, dispatches, c[DrawsFiltered], c[DrawsNoPipeline],
                c[DrawsBindFailed], c[ImagesCreated], c[ImagesFreed], c[ImagesExpanded],
                c[MetaClearDispatches], c[FceClears], c[FillMetaClears]);
}

} // namespace MediEvil::FrameTrace

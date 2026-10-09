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

const bool g_checkpoints = [] {
    const char* env = std::getenv("MEDIEVIL_CHECKPOINTS");
    return env && *env == '1';
}();

namespace {
struct CheckpointEntry {
    u64 hash;
    u32 frame;
    bool compute;
    char note[160];
};
constexpr u32 CheckpointRing = 1u << 16;
std::array<CheckpointEntry, CheckpointRing> g_checkpoint_ring{};
std::atomic<u32> g_checkpoint_seq{1};
std::atomic<u32> g_checkpoint_frame{0};
} // namespace

u32 CheckpointTag(u64 shader_hash, bool compute) {
    const u32 tag = g_checkpoint_seq.fetch_add(1, std::memory_order_relaxed);
    auto& e = g_checkpoint_ring[tag % CheckpointRing];
    e.hash = shader_hash;
    e.frame = g_checkpoint_frame.load();
    e.compute = compute;
    e.note[0] = 0;
    return tag;
}

void SetCheckpointNote(u32 tag, const char* note) {
    auto& e = g_checkpoint_ring[tag % CheckpointRing];
    std::snprintf(e.note, sizeof(e.note), "%s", note);
}

const char* GetCheckpointNote(u32 tag) {
    return g_checkpoint_ring[tag % CheckpointRing].note;
}

void DescribeCheckpoint(u32 tag, u64& shader_hash, bool& compute, u32& frame) {
    const auto& e = g_checkpoint_ring[tag % CheckpointRing];
    shader_hash = e.hash;
    compute = e.compute;
    frame = e.frame;
}

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
    g_checkpoint_frame.store(flip_number, std::memory_order_relaxed);
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

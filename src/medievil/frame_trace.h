// SPDX-FileCopyrightText: 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>

#include "common/types.h"

// Per-flip debug counters (MEDIEVIL_FRAMETRACE=1): logs one line per presented frame and prints
// the flip number on screen, so glitches found in a recording can be matched to the log.
namespace MediEvil::FrameTrace {

enum Counter : u32 {
    DrawsFiltered,
    DrawsNoPipeline,
    DrawsBindFailed,
    ImagesCreated,
    ImagesFreed,
    ImagesExpanded,
    MetaClearDispatches,
    FceClears,
    FillMetaClears,
    Count,
};

extern const bool g_enabled;
extern std::array<std::atomic<u32>, Counter::Count> g_counters;

inline void Inc(Counter c) {
    if (g_enabled) {
        g_counters[c].fetch_add(1, std::memory_order_relaxed);
    }
}

void OnFlip(u32 flip_number, u32 draws, u32 dispatches);

// Per-draw resource record: the last draws of every frame are written to medievil_draws.txt
// so a broken frame can be compared with good ones (which images each pass read and wrote).
struct DrawRecord {
    u64 ps_hash;
    u32 num_images;
    u32 image_index[8];
    u64 image_addr[8];
};
void RecordDraw(const DrawRecord& rec);

// GPU breadcrumbs (MEDIEVIL_CHECKPOINTS=1, NVIDIA VK_NV_device_diagnostic_checkpoints): every
// draw/dispatch gets a sequence number whose shader hash is remembered, so the last breadcrumb the
// GPU reached can be named after a device loss.
extern const bool g_checkpoints;
u32 CheckpointTag(u64 shader_hash, bool compute);
void DescribeCheckpoint(u32 tag, u64& shader_hash, bool& compute, u32& frame);
void SetCheckpointNote(u32 tag, const char* note);
const char* GetCheckpointNote(u32 tag);

} // namespace MediEvil::FrameTrace

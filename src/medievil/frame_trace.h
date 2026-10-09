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

} // namespace MediEvil::FrameTrace

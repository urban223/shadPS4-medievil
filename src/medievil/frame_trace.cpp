// SPDX-FileCopyrightText: 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>

#include "common/logging/log.h"
#include "medievil/frame_trace.h"

namespace MediEvil::FrameTrace {

const bool g_enabled = [] {
    const char* env = std::getenv("MEDIEVIL_FRAMETRACE");
    return env && *env && *env != '0';
}();

std::array<std::atomic<u32>, Counter::Count> g_counters{};

void OnFlip(u32 flip_number, u32 draws, u32 dispatches) {
    if (!g_enabled) {
        return;
    }
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

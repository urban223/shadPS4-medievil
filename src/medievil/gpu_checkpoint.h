// SPDX-FileCopyrightText: 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "medievil/frame_trace.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace MediEvil::FrameTrace {

// Set by the Vulkan instance when VK_NV_device_diagnostic_checkpoints is enabled.
inline std::atomic<bool> g_checkpoints_active{false};

// Drops a named breadcrumb into the command buffer (no-op unless checkpoints are active).
inline void MarkCheckpoint(vk::CommandBuffer cmdbuf, const char* what) {
    if (!g_checkpoints_active.load(std::memory_order_relaxed)) {
        return;
    }
    const u32 tag = CheckpointTag(0xFFFF0000'00000000ULL, false);
    SetCheckpointNote(tag, what);
    cmdbuf.setCheckpointNV(reinterpret_cast<void*>(uintptr_t(tag)));
}

} // namespace MediEvil::FrameTrace

// SPDX-FileCopyrightText: 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace MediEvil {

// Starts the in-process sampling profiler if MEDIEVIL_PROFILE is set (see profiler.cpp).
void StartProfilerFromEnv();

} // namespace MediEvil

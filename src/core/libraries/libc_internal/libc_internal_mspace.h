// SPDX-FileCopyrightText: 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::LibcInternal {
void RegisterlibSceLibcInternalMspace(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::LibcInternal

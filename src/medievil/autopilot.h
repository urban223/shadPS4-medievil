// SPDX-FileCopyrightText: 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Input {
class GameControllers;
}

namespace MediEvil {

// Replays a scripted gamepad session from the file named by the MEDIEVIL_AUTOPILOT
// environment variable. Used for unattended testing (menus, level loads, screenshots).
// Does nothing when the variable is not set.
void StartAutopilot(Input::GameControllers* controllers);

} // namespace MediEvil

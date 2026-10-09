// SPDX-FileCopyrightText: 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// Script format, one command per line, '#' starts a comment:
//   wait <ms>                       sleep
//   press <button> [hold_ms=120]    press and release a button
//   down <button> / up <button>     hold / release a button
//   stick <lx|ly|rx|ry> <-1..1> [ms]  deflect a stick, released after ms (if given)
//   shot                            request a game-only screenshot
//   every <ms>                      screenshot periodically in the background (0 = off)
//   repeat <n> ... end              repeat the enclosed block n times
//   include <file>                  splice another script (path relative to this one)
//   quit                            close the window
// Buttons: cross circle square triangle l1 r1 l3 r3 options share touchpad up down left right
// Axes (stick): lx ly rx ry l2 r2 (triggers 0..1)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <memory>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#include <fmt/ranges.h>

#include "common/logging/log.h"
#include "core/debug_state.h"
#include "medievil/autopilot.h"
#include "medievil/profiler.h"
#include "video_core/renderdoc.h"

namespace MediEvil {


namespace {

struct Command {
    std::vector<std::string> args;
    std::vector<Command> body; // for repeat
};

std::vector<Command> ParseFile(const std::filesystem::path& path);

std::filesystem::path g_script_dir;

std::vector<Command> Parse(std::istream& in, bool nested) {
    std::vector<Command> out;
    std::string line;
    while (std::getline(in, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.resize(hash);
        }
        std::istringstream ls(line);
        Command cmd;
        for (std::string tok; ls >> tok;) {
            cmd.args.push_back(tok);
        }
        if (cmd.args.empty()) {
            continue;
        }
        if (cmd.args[0] == "end") {
            if (nested) {
                return out;
            }
            continue;
        }
        if (cmd.args[0] == "repeat") {
            cmd.body = Parse(in, true);
        }
        if (cmd.args[0] == "include" && cmd.args.size() > 1) {
            auto inc = ParseFile(g_script_dir / cmd.args[1]);
            out.insert(out.end(), inc.begin(), inc.end());
            continue;
        }
        out.push_back(std::move(cmd));
    }
    return out;
}

std::vector<Command> ParseFile(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) {
        LOG_ERROR(Input, "autopilot: cannot open {}", path.string());
        return {};
    }
    return Parse(f, false);
}

// Inputs go through an SDL virtual gamepad, so the game *and* host-side UI (ImGui system
// dialogs such as the save-data list) see exactly what a physical pad would produce.
const std::unordered_map<std::string, SDL_GamepadButton> kButtons = {
    {"cross", SDL_GAMEPAD_BUTTON_SOUTH},
    {"circle", SDL_GAMEPAD_BUTTON_EAST},
    {"square", SDL_GAMEPAD_BUTTON_WEST},
    {"triangle", SDL_GAMEPAD_BUTTON_NORTH},
    {"l1", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"r1", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"l3", SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"r3", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"options", SDL_GAMEPAD_BUTTON_START},
    {"share", SDL_GAMEPAD_BUTTON_BACK},
    {"touchpad", SDL_GAMEPAD_BUTTON_TOUCHPAD},
    {"up", SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};

const std::unordered_map<std::string, SDL_GamepadAxis> kAxes = {
    {"lx", SDL_GAMEPAD_AXIS_LEFTX},       {"ly", SDL_GAMEPAD_AXIS_LEFTY},
    {"rx", SDL_GAMEPAD_AXIS_RIGHTX},      {"ry", SDL_GAMEPAD_AXIS_RIGHTY},
    {"l2", SDL_GAMEPAD_AXIS_LEFT_TRIGGER}, {"r2", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
};

SDL_Joystick* g_vpad = nullptr;

bool CreateVirtualPad() {
    SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD);
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x054c; // Sony
    desc.product_id = 0x09cc; // DualShock 4
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = 0xFFFFFFFFu >> (32 - SDL_GAMEPAD_BUTTON_COUNT);
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.name = "MediEvil Autopilot Pad";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (id == 0) {
        LOG_ERROR(Input, "autopilot: SDL_AttachVirtualJoystick failed: {}", SDL_GetError());
        return false;
    }
    g_vpad = SDL_OpenJoystick(id);
    return g_vpad != nullptr;
}

void SetButton(SDL_GamepadButton b, bool down) {
    SDL_SetJoystickVirtualButton(g_vpad, b, down);
}

void SetAxis(SDL_GamepadAxis a, float v) {
    const bool trigger = a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    v = std::clamp(v, trigger ? 0.0f : -1.0f, 1.0f);
    SDL_SetJoystickVirtualAxis(g_vpad, a, static_cast<Sint16>(v * 32767.0f));
}

std::atomic<int> g_shot_interval_ms{0};

void Sleep(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int ArgInt(const Command& c, size_t i, int def) {
    return c.args.size() > i ? std::atoi(c.args[i].c_str()) : def;
}

void Run(const std::vector<Command>& cmds) {
    for (const auto& c : cmds) {
        const auto& op = c.args[0];
        LOG_INFO(Input, "autopilot: {}", fmt::join(c.args, " "));
        if (op == "wait") {
            Sleep(ArgInt(c, 1, 0));
        } else if (op == "press" || op == "down" || op == "up") {
            const auto it = c.args.size() > 1 ? kButtons.find(c.args[1]) : kButtons.end();
            if (it == kButtons.end()) {
                LOG_ERROR(Input, "autopilot: unknown button");
                continue;
            }
            if (op != "up") {
                SetButton(it->second, true);
            }
            if (op == "press") {
                Sleep(ArgInt(c, 2, 120));
            }
            if (op != "down") {
                SetButton(it->second, false);
            }
        } else if (op == "stick") {
            const auto it = c.args.size() > 2 ? kAxes.find(c.args[1]) : kAxes.end();
            if (it == kAxes.end()) {
                LOG_ERROR(Input, "autopilot: bad stick command");
                continue;
            }
            SetAxis(it->second, std::strtof(c.args[2].c_str(), nullptr));
            if (c.args.size() > 3) {
                Sleep(ArgInt(c, 3, 0));
                SetAxis(it->second, 0.0f);
            }
        } else if (op == "shot") {
            VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::WithOverlays);
        } else if (op == "every") {
            g_shot_interval_ms = ArgInt(c, 1, 0);
        } else if (op == "repeat") {
            for (int i = 0, n = ArgInt(c, 1, 1); i < n; ++i) {
                Run(c.body);
            }
        } else if (op == "quit") {
            SDL_Event e{};
            e.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&e);
        } else {
            LOG_ERROR(Input, "autopilot: unknown command {}", op);
        }
    }
}

} // namespace

void StartAutopilot(Input::GameControllers* controllers) {
    StartProfilerFromEnv();
    const char* path = std::getenv("MEDIEVIL_AUTOPILOT");
    if (!path || !*path) {
        return;
    }
    g_script_dir = std::filesystem::path(path).parent_path();
    auto cmds = std::make_shared<std::vector<Command>>(ParseFile(path));
    LOG_INFO(Input, "autopilot: loaded {} commands from {}", cmds->size(), path);
    if (!CreateVirtualPad()) {
        return;
    }
    std::thread([cmds] {
        Sleep(1000); // let the window pick up the hot-plugged pad
        Run(*cmds);
    }).detach();
    std::thread([] {
        auto next_shot = std::chrono::steady_clock::now();
        auto last_perf = std::chrono::steady_clock::now();
        u32 last_frames = DebugState.GetFrameNum();
        float worst_delta = 0.0f;
        for (;;) {
            Sleep(50);
            const auto now = std::chrono::steady_clock::now();
            worst_delta = std::max(worst_delta, DebugState.FrameDeltaTime);
            if (now - last_perf >= std::chrono::seconds(1)) {
                const u32 frames = DebugState.GetFrameNum();
                const double secs = std::chrono::duration<double>(now - last_perf).count();
                LOG_INFO(Input, "perf: {:.1f} fps, worst frame {:.1f} ms, draws {:.0f}",
                         (frames - last_frames) / secs, worst_delta * 1000.0f,
                         DebugState.GetDrawCallsAvg());
                last_frames = frames;
                last_perf = now;
                worst_delta = 0.0f;
            }
            const int ms = g_shot_interval_ms.load();
            if (ms > 0 && now >= next_shot) {
                VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::WithOverlays);
                next_shot = now + std::chrono::milliseconds(ms);
            }
        }
    }).detach();
}

} // namespace MediEvil

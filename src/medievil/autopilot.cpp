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
//   quit                            close the window
// Buttons: cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad up down left right

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <memory>

#include <SDL3/SDL_events.h>
#include <fmt/ranges.h>

#include "common/logging/log.h"
#include "core/libraries/pad/pad.h"
#include "input/controller.h"
#include "medievil/autopilot.h"
#include "video_core/renderdoc.h"

namespace MediEvil {

using Libraries::Pad::OrbisPadButtonDataOffset;

namespace {

struct Command {
    std::vector<std::string> args;
    std::vector<Command> body; // for repeat
};

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
        out.push_back(std::move(cmd));
    }
    return out;
}

const std::unordered_map<std::string, OrbisPadButtonDataOffset> kButtons = {
    {"cross", OrbisPadButtonDataOffset::Cross},
    {"circle", OrbisPadButtonDataOffset::Circle},
    {"square", OrbisPadButtonDataOffset::Square},
    {"triangle", OrbisPadButtonDataOffset::Triangle},
    {"l1", OrbisPadButtonDataOffset::L1},
    {"r1", OrbisPadButtonDataOffset::R1},
    {"l2", OrbisPadButtonDataOffset::L2},
    {"r2", OrbisPadButtonDataOffset::R2},
    {"l3", OrbisPadButtonDataOffset::L3},
    {"r3", OrbisPadButtonDataOffset::R3},
    {"options", OrbisPadButtonDataOffset::Options},
    {"touchpad", OrbisPadButtonDataOffset::TouchPad},
    {"up", OrbisPadButtonDataOffset::Up},
    {"down", OrbisPadButtonDataOffset::Down},
    {"left", OrbisPadButtonDataOffset::Left},
    {"right", OrbisPadButtonDataOffset::Right},
};

const std::unordered_map<std::string, Input::Axis> kAxes = {
    {"lx", Input::Axis::LeftX},
    {"ly", Input::Axis::LeftY},
    {"rx", Input::Axis::RightX},
    {"ry", Input::Axis::RightY},
};

std::atomic<int> g_shot_interval_ms{0};

void Sleep(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int ArgInt(const Command& c, size_t i, int def) {
    return c.args.size() > i ? std::atoi(c.args[i].c_str()) : def;
}

void Run(Input::GameController* pad, const std::vector<Command>& cmds) {
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
                pad->Button(it->second, true);
            }
            if (op == "press") {
                Sleep(ArgInt(c, 2, 120));
            }
            if (op != "down") {
                pad->Button(it->second, false);
            }
        } else if (op == "stick") {
            const auto it = c.args.size() > 2 ? kAxes.find(c.args[1]) : kAxes.end();
            if (it == kAxes.end()) {
                LOG_ERROR(Input, "autopilot: bad stick command");
                continue;
            }
            const float v = std::strtof(c.args[2].c_str(), nullptr);
            pad->Axis(it->second, Input::GetAxis(-0x80, 0x7f, static_cast<int>(v * 0x7f)), false);
            if (c.args.size() > 3) {
                Sleep(ArgInt(c, 3, 0));
                pad->Axis(it->second, Input::GetAxis(-0x80, 0x7f, 0), false);
            }
        } else if (op == "shot") {
            VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
        } else if (op == "every") {
            g_shot_interval_ms = ArgInt(c, 1, 0);
        } else if (op == "repeat") {
            for (int i = 0, n = ArgInt(c, 1, 1); i < n; ++i) {
                Run(pad, c.body);
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
    const char* path = std::getenv("MEDIEVIL_AUTOPILOT");
    if (!path || !*path) {
        return;
    }
    std::ifstream f(path);
    if (!f) {
        LOG_ERROR(Input, "autopilot: cannot open {}", path);
        return;
    }
    auto cmds = std::make_shared<std::vector<Command>>(Parse(f, false));
    LOG_INFO(Input, "autopilot: loaded {} commands from {}", cmds->size(), path);
    auto* pad = (*controllers)[0];
    std::thread([pad, cmds] { Run(pad, *cmds); }).detach();
    std::thread([] {
        for (;;) {
            const int ms = g_shot_interval_ms.load();
            Sleep(ms > 0 ? ms : 250);
            if (ms > 0) {
                VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
            }
        }
    }).detach();
}

} // namespace MediEvil

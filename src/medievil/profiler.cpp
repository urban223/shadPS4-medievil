// SPDX-FileCopyrightText: 2026 MediEvil PC port
// SPDX-License-Identifier: GPL-2.0-or-later

// In-process sampling profiler (Windows only).
// MEDIEVIL_PROFILE="<thread name substring>;<delay s>;<duration s>;<out file>"
// Suspends the target thread every ~2ms, walks its stack and writes a self/inclusive report
// symbolized through the executable's PDB.

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>
// windows.h must come first
#include <dbghelp.h>
#include <tlhelp32.h>
#include <psapi.h>

#include "common/logging/log.h"
#include "common/types.h"
#include "medievil/profiler.h"

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

namespace MediEvil {

namespace {

std::wstring ThreadName(HANDLE h) {
    PWSTR desc = nullptr;
    std::wstring name;
    if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
        name = desc;
        LocalFree(desc);
    }
    return name;
}

HANDLE FindThread(const std::wstring& needle) {
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{.dwSize = sizeof(te)};
    HANDLE found = nullptr;
    for (BOOL ok = Thread32First(snap, &te); ok && !found; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) {
            continue;
        }
        HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (!h) {
            continue;
        }
        if (ThreadName(h).find(needle) != std::wstring::npos) {
            found = h;
        } else {
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    return found;
}

std::string Symbolize(HANDLE proc, u64 addr) {
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512]{};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    if (SymFromAddr(proc, addr, &disp, sym)) {
        return sym->Name;
    }
    char mod[32];
    std::snprintf(mod, sizeof(mod), "?%llx", static_cast<unsigned long long>(addr & ~0xFFFull));
    return mod;
}

void Run(std::wstring needle, int delay_s, int duration_s, std::string out_path) {
    std::this_thread::sleep_for(std::chrono::seconds(delay_s));
    HANDLE thread = FindThread(needle);
    if (!thread) {
        LOG_ERROR(Debug, "profiler: thread not found");
        return;
    }
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);

    std::vector<std::vector<u64>> stacks;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(duration_s);
    while (std::chrono::steady_clock::now() < end) {
        if (SuspendThread(thread) != DWORD(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            std::vector<u64> frames;
            if (GetThreadContext(thread, &ctx)) {
                STACKFRAME64 sf{};
                sf.AddrPC = {ctx.Rip, 0, AddrModeFlat};
                sf.AddrFrame = {ctx.Rbp, 0, AddrModeFlat};
                sf.AddrStack = {ctx.Rsp, 0, AddrModeFlat};
                for (int i = 0; i < 48; ++i) {
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &sf, &ctx, nullptr,
                                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                        sf.AddrPC.Offset == 0) {
                        break;
                    }
                    frames.push_back(sf.AddrPC.Offset);
                }
            }
            ResumeThread(thread);
            stacks.push_back(std::move(frames));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    std::unordered_map<u64, std::string> names;
    const auto name_of = [&](u64 a) -> const std::string& {
        auto it = names.find(a);
        if (it == names.end()) {
            it = names.emplace(a, Symbolize(proc, a)).first;
        }
        return it->second;
    };
    // Range of our own module, to attribute driver/CRT time to the nearest caller in our code.
    MODULEINFO mi{};
    GetModuleInformation(proc, GetModuleHandleW(nullptr), &mi, sizeof(mi));
    const u64 mod_lo = reinterpret_cast<u64>(mi.lpBaseOfDll);
    const u64 mod_hi = mod_lo + mi.SizeOfImage;
    std::unordered_map<std::string, u64> self, inclusive, attributed;
    for (const auto& st : stacks) {
        if (st.empty()) {
            continue;
        }
        for (u64 a : st) {
            if (a >= mod_lo && a < mod_hi) {
                ++attributed[name_of(a)];
                break;
            }
        }
        ++self[name_of(st.front())];
        std::unordered_set<std::string> seen;
        for (u64 a : st) {
            if (seen.insert(name_of(a)).second) {
                ++inclusive[name_of(a)];
            }
        }
    }
    const auto dump = [&](std::ofstream& f, const std::unordered_map<std::string, u64>& m,
                          size_t top) {
        std::vector<std::pair<std::string, u64>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < std::min(top, v.size()); ++i) {
            f << std::fixed;
            f.precision(1);
            f << (100.0 * v[i].second / stacks.size()) << "%\t" << v[i].first << "\n";
        }
    };
    std::ofstream f(out_path);
    f << stacks.size() << " samples\n\n-- self --\n";
    dump(f, self, 50);
    f << "\n-- inclusive --\n";
    dump(f, inclusive, 120);
    SymCleanup(proc);
    CloseHandle(thread);
    LOG_WARNING(Debug, "profiler: wrote {} samples to {}", stacks.size(), out_path);
}

} // namespace

void StartProfilerFromEnv() {
    const char* env = std::getenv("MEDIEVIL_PROFILE");
    if (!env || !*env) {
        return;
    }
    std::string spec = env;
    std::vector<std::string> parts;
    for (size_t pos = 0;;) {
        const size_t next = spec.find(';', pos);
        parts.push_back(spec.substr(pos, next - pos));
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }
    if (parts.size() < 4) {
        return;
    }
    std::wstring needle(parts[0].begin(), parts[0].end());
    std::thread(Run, needle, std::atoi(parts[1].c_str()), std::atoi(parts[2].c_str()), parts[3])
        .detach();
}

} // namespace MediEvil

#else

namespace MediEvil {
void StartProfilerFromEnv() {}
} // namespace MediEvil

#endif

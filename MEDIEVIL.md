# shadPS4 — MediEvil (PS4, CUSA12982) branch

This branch is a personal study fork of [shadPS4](https://github.com/shadps4-emu/shadPS4) tuned to run the
PS4 remake of **MediEvil** (Unreal Engine 4.21) natively on Windows with good performance and without
rendering artifacts. It contains **no game data**: you need your own legally obtained copy of the game.

Nothing here is meant for upstream as-is: some changes are MediEvil-specific (defaults, autopilot,
diagnostics). General emulation fixes are described below so they can be ported separately.

## Status

- All 22 levels load and play; 55–60 FPS at 1080p on an RTX 4070 Ti SUPER (heaviest scene ~55).
- No black-frame flicker, no effect/HUD dropouts (verified with frame-by-frame video analysis of every level).
- Known issue: rare GPU timeout (device lost) during the Asylum Grounds intro (~1 in 10 loads). Under investigation.
- Input: gamepad only (keyboard/mouse not mapped yet).

## Emulation fixes (general, not game-specific)

| Area | Fix |
|---|---|
| Buffer cache | Unmapping/decommitting memory drops its GPU-written state (`gpu_modified_ranges` + page GPU bits). Pooled memory is recommitted constantly by UE4; stale GPU copies were read instead of fresh CPU data → zeroed post-process constants → black frames. |
| Buffer cache | Small read-only bindings use the stream buffer unless their exact bytes were GPU-written (page-level check was too coarse). |
| Memory tracking | `Rasterizer::MapMemory` invalidates freshly mapped memory: a new host mapping has no write protection, so caches that still considered the pages protected missed CPU writes → one-frame dropouts of particles/HUD and occasional device loss. |
| Shader recompiler | `FMax/FMin` → `OpNMax/OpNMin` (GCN NaN semantics). |
| Shader recompiler | `V_RCP/RSQ_CLAMP` return ±FLT_MAX, `*_LEGACY` return ±0 for infinities; `V_LOG_CLAMP`. |
| Shader recompiler | `V_CVT_PKRTZ_F16_F32` saturates finite values to ±65504 (round toward zero) instead of overflowing to infinity. |
| Texture cache | 3D LUTs rendered as 2D-array targets reuse the 3D image (no per-frame re-creation of UE4 color-grading LUTs). |
| Memory | `TryWriteBacking` advances the source pointer across physical areas. |
| HLE | `sceLibcMspace*`, `__cxa_guard_*`, `_Getpctype`. |

## Performance work

Bottleneck is the GPU command processor thread (5–7k draws/frame in heavy UE4 scenes).
Entrance Hall benchmark: **25.4 → 56 FPS**.

- Buffer access tracking for barriers: O(n²) → near O(1) with interval coalescing.
- `FindImage` memoization keyed on the registry generation.
- Semaphore polling throttled per draw; reusable high-resolution waitable timer per thread.
- Warning-level logging by default.
- Recommended build: clang-cl, `/O2 /Ob2` (see `scripts/build-fast.bat` in the parent project).

## Port defaults

- Internal resolution 1920×1080 (was 1280×720 — the main cause of a blurry image), FidelityFX RCAS
  sharpening (0.25) at native resolution, borderless fullscreen, pipeline cache on.
- Window title "MediEvil", user directory next to the executable, no first-run prompts, gamepad-only bindings.

## Diagnostics (environment variables)

| Variable | Purpose |
|---|---|
| `MEDIEVIL_AUTOPILOT=<script>` | Scripted SDL virtual gamepad (`wait`, `press`, `stick`, `skipuntil`, `shot`, `repeat`, `include`, `quit`), per-second FPS log. |
| `MEDIEVIL_PROFILE=thread;delay;dur;file` | In-process sampling profiler (self/inclusive time per function). |
| `MEDIEVIL_FRAMETRACE=1` | Flip number overlay, per-flip counters, last draws/dispatches with bound images (`medievil_draws.txt`). |
| `MEDIEVIL_PROBE=<ps hash>,...` | Reads back render-target texels and compares constant buffers (guest vs GPU) after the given pixel shaders. |
| `MEDIEVIL_CHECKPOINTS=1` | NVIDIA diagnostic checkpoints on every GPU command; logged on device loss together with `VK_EXT_device_fault` info. |
| `MEDIEVIL_FULLBARRIERS=1` | Full pipeline barriers around every pass (sync debugging). |
| `MEDIEVIL_NO_MAP_INVALIDATE=1` | Disables the map-invalidation fix (A/B testing). |

## Building

Same as upstream shadPS4 (CMake). The parent project uses clang-cl 21 + Ninja on Windows:
`cmake -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo`
with `/O2 /Ob2` for release-like performance.

License: GPL-2.0-or-later, as upstream shadPS4.

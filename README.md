# skyrim_render_clean

A clean, minimal SKSE plugin for Skyrim Special Edition 1.5.97 (32-bit
TESV.exe). Hooks the D3D9 render pipeline to skip redundant state changes,
caches shader-constant uploads, and replaces D3DX9 math with SSE-vectorised
versions.

This is the rebuild of `skyrim_render_boost` after the 39-version
investigation. Only the optimisations that demonstrably help in cold-boot
A/B testing are kept. Everything that touched OS scheduling, process
priority, thread affinity, or power throttling has been removed - those
are things users can do themselves and they were responsible for the
Windows 7 compatibility issues (error 126 / missing MSVCP140D.dll).

## What it does

| Subsystem            | What                                                      |
| -------------------- | --------------------------------------------------------- |
| D3D9 vtable cache    | 24 vtable hooks skip redundant SetXxx calls               |
| Engine cache hooks   | 4 hooks inside TESV's PS/VS dispatchers                   |
| D3DX9 math IAT       | 9 D3DX9 math functions replaced with SSE versions         |
| Frame profiling      | Histogram + p50/p95/p99 + spike events (opt-in)           |
| Timer resolution     | timeBeginPeriod(1), one call, smooths frame pacing        |

## What it does NOT do

| Removed                    | Why                                                |
| -------------------------- | -------------------------------------------------- |
| Process priority elevation | Users can do this via Task Manager                 |
| Render thread affinity     | Hurt as often as it helped; user can set with SetAffinity tools |
| MMCSS registration         | Marginal gain; required Vista+ load                |
| EcoQoS / PowerThrottling   | Win10-1709-only API caused Win7 load failures      |
| SetThreadDescription       | Win10+ API caused Win7 load failures               |

## Build configuration

This MUST be built as **Release with static CRT** to ship without runtime
DLL dependencies. Debug builds need `MSVCP140D.dll` which doesn't exist
outside Visual Studio installs - shipping a debug build is the cause of
error-126 reports from end users.

### Recommended Visual Studio project settings

| Setting                          | Value                       | Why                                    |
| -------------------------------- | --------------------------- | -------------------------------------- |
| Configuration                    | Release                     | No debug runtime deps                  |
| Platform                         | Win32 (x86)                 | TESV.exe is 32-bit                     |
| C/C++ > Code Generation > Runtime Library | Multi-threaded (`/MT`) | Statically link CRT - zero MSVCR/MSVCP/VCRUNTIME deps |
| C/C++ > Optimization             | Maximum (`/O2`)             | Standard release optimisation          |
| C/C++ > Whole Program Optimization | Yes                       | LTCG, ~1-3% extra perf                 |
| Linker > Optimization > LTCG     | Use Link-Time Code Gen      | Pairs with WPO above                   |
| Linker > System > Subsystem      | `Windows (/SUBSYSTEM:WINDOWS,6.01)` | Win7 floor                |
| Linker > Manifest > UAC          | No                          | Plugins don't need elevation           |

The `/MT` flag is the single most important one. It bakes the CRT into
your DLL so end users don't need MSVCP140.dll, MSVCR140.dll, VCRUNTIME140.dll,
or any debug variant of those.

### Verifying zero runtime dependencies before shipping

After building, run this from the Developer Command Prompt:

```
dumpbin /dependents skyrim_render_clean.dll
```

The output should list ONLY core Windows DLLs that exist on every Windows
install since XP. Acceptable:

```
KERNEL32.dll
USER32.dll
d3d9.dll
D3DX9_42.dll
PSAPI.DLL
```

If you see ANY of these, the build is wrong - rebuild with `/MT`:

```
MSVCP140.dll      <- WRONG, dynamic link to release CRT
MSVCP140D.dll     <- WRONG, debug build
MSVCR140.dll      <- WRONG
VCRUNTIME140.dll  <- WRONG
VCRUNTIME140D.dll <- WRONG, debug build
api-ms-win-*.dll  <- WRONG, UCRT not statically linked
```

`D3DX9_42.dll` is fine - it ships with DirectX runtime installed on every
Windows + Steam machine, and Skyrim itself depends on it.

## Source tree

```
include/
  core/          Common.h, ini_config.h
  hooks/         d3d9_wrapper.h, device_hooks.h, engine_cache_hooks.h, hooks.h
  optimize/      d3dx_cache.h, matrix_math.h
  profile/       session_fingerprint.h
  arch/          function_relocator.h    (architecture-specific)
src/             mirrors include/ tree
```

The `arch/` subdirectory is set up for a future 64-bit Skyrim SE 1.6+ port.
The 32-bit-specific code (instruction relocation, RVAs into TESV.exe,
inline assembly trampolines) is kept there. To port to 64-bit:

1. Replace every RVA in `engine_cache_hooks.cpp` (`kProlog_*` tables) with
   values from a fresh IDA dump of SkyrimSE.exe 1.6+.
2. Rewrite `CacheHook_PS/VS/PS_Alt/VS_Alt` inline-asm bodies for the x64
   calling convention (RCX/RDX/R8/R9 instead of __thiscall ECX).
3. Update vtable index constants and patch offsets where 64-bit pointers
   shift struct layouts.
4. Add x64 toolchain configuration alongside the existing Win32 in the
   project file.

This is documented preparation only. It is NOT implemented today and
won't build in 64-bit mode without the work above.

## Configuration

`skyrim_render_clean.ini` is auto-generated next to the DLL on first run.
The schema is deliberately minimal:

```ini
[Master]
Enabled=true                ; master switch for all optimisations

[Profiling]
Enabled=false               ; write skyrim_render_clean.log
StutterThresholdMs=25       ; log per-frame spikes over this
MaxSpikesPerWindow=200      ; safety cap on per-window spike events
HotspotProfiling=false      ; opt-in: hook fixed list of TESV subs and
                            ; count calls per 30s window
```

When `[Master] Enabled=false`, the DLL behaves like a pass-through. The
D3D9 hook is still installed (needed for `Hook_Present` profiling) but no
optimisations run. This lets users produce mod-on/mod-off comparison logs
from the same DLL by editing the INI between runs.

When `[Profiling] HotspotProfiling=true` (requires `Enabled=true` in
`[Profiling]`), a fixed list of ~100 TESV.exe subroutine entry points is
hooked at startup. Each call increments an atomic counter; every 30
seconds the counters are written sorted-descending to a separate log
(`skyrim_render_clean_hotspots.log`) and reset for the next window.
Adds 1-3% overhead to hot paths - production users keep this off.

## Compatibility

- Skyrim SE 1.5.97 (32-bit). Other 1.5.x versions: untested but likely.
- Windows 7 SP1 through Windows 11 (when built with the settings above).
- ENB, ReShade, DXVK, all visual mods: compatible.
- Other SKSE plugins: compatible.

## License

User code only - no Bethesda assets, no SKSE source, no ENB headers, no
proprietary content of any kind.

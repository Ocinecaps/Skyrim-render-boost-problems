# BUILD.md - Visual Studio project configuration

This document specifies every setting needed to build skyrim_render_clean
correctly. Hand this to Windsurf (or any tool that generates Visual Studio
projects) and it will have everything required.

If you're setting up the project manually, work through this top to bottom.

## Project type

- Output type: **DLL**
- Target name: `skyrim_render_clean.dll`
- Configuration types needed: **Release** (only — never ship Debug)
- Platform: **Win32 (x86)** (TESV.exe is 32-bit)

## Source files (22 total)

```
src/arch/function_relocator.cpp
src/core/dllmain.cpp
src/core/ini_config.cpp
src/hooks/d3d9_wrapper.cpp
src/hooks/device_hooks.cpp
src/hooks/engine_cache_hooks.cpp
src/hooks/hooks.cpp
src/optimize/d3dx_cache.cpp
src/optimize/matrix_math.cpp
src/profile/function_counters.cpp
src/profile/session_fingerprint.cpp
```

## Header files (11 total)

```
include/arch/function_relocator.h
include/core/Common.h
include/core/ini_config.h
include/hooks/d3d9_wrapper.h
include/hooks/device_hooks.h
include/hooks/engine_cache_hooks.h
include/hooks/hooks.h
include/optimize/d3dx_cache.h
include/optimize/matrix_math.h
include/profile/function_counters.h
include/profile/session_fingerprint.h
```

## Other files

```
skyrim_render_clean.manifest    (linker manifest input - see below)
skyrim_render_clean.ini         (ships alongside the DLL, read at runtime)
README.md                       (documentation)
```

## Include paths (Additional Include Directories)

The cpp files use flat `#include "name.h"` (no subdirectory prefix), so
ALL five include subdirectories must be on the include path:

```
$(ProjectDir)include
$(ProjectDir)include\core
$(ProjectDir)include\hooks
$(ProjectDir)include\optimize
$(ProjectDir)include\profile
$(ProjectDir)include\arch
```

Plus the SKSE 1.5.97 source tree (path varies by your dev setup):

```
$(SKSE_ROOT)
$(SKSE_ROOT)\common
$(SKSE_ROOT)\skse
```

The cpp files reference SKSE headers as:
```cpp
#include "common/ITypes.h"
#include "skse/skse/PluginAPI.h"
#include "skse/skse/skse_version.h"
```

So `$(SKSE_ROOT)` should point at the directory CONTAINING `common/` and
`skse/` — typically the SKSE 1.7.3 source root.

## C/C++ compiler settings (Release|Win32)

| Property                                    | Value                              |
| ------------------------------------------- | ---------------------------------- |
| General > C++ Language Standard             | ISO C++17 Standard (`/std:c++17`)  |
| General > Treat Warnings As Errors          | No                                 |
| Optimization > Optimization                 | Maximum Optimization (`/O2`)       |
| Optimization > Inline Function Expansion    | Any Suitable (`/Ob2`)              |
| Optimization > Favor Size Or Speed          | Favor Fast Code (`/Ot`)            |
| Optimization > Whole Program Optimization   | Yes (`/GL`)                        |
| Code Generation > Runtime Library           | **Multi-threaded (`/MT`)**         |
| Code Generation > Enable Function-Level Linking | Yes (`/Gy`)                    |
| Code Generation > Enable Enhanced Instruction Set | Streaming SIMD Extensions 2 (`/arch:SSE2`) |
| Code Generation > Floating Point Model      | Fast (`/fp:fast`)                  |
| Code Generation > Buffer Security Check     | No (`/GS-`)                        |
| Code Generation > Security Development Lifecycle Checks | No (`/sdl-`)           |
| Language > Conformance Mode                 | Yes (`/permissive-`)               |
| Preprocessor > Preprocessor Definitions     | `WIN32;_WINDOWS;NDEBUG;_USRDLL`    |

**Critical: Runtime Library MUST be `/MT`, NOT `/MD`.** This is the
single most important setting — it statically links the C runtime into
the DLL so end users don't need MSVCP140.dll, VCRUNTIME140.dll, or any
debug variants. A `/MD` build is what caused the error-126 reports
("MSVCP140D.dll could not be found").

## Linker settings (Release|Win32)

| Property                                    | Value                              |
| ------------------------------------------- | ---------------------------------- |
| General > Output File                       | `$(OutDir)skyrim_render_clean.dll` |
| Manifest File > Generate Manifest           | Yes                                |
| Manifest File > Additional Manifest Files   | `skyrim_render_clean.manifest`     |
| Manifest File > UAC Execution Level         | asInvoker                          |
| Optimization > References                   | Eliminate Unreferenced Data (`/OPT:REF`) |
| Optimization > Enable COMDAT Folding        | Remove Redundant COMDATs (`/OPT:ICF`) |
| Optimization > Link Time Code Generation    | Use Link Time Code Generation (`/LTCG`) |
| Advanced > Entry Point                      | (leave blank — DllMain auto-detects) |
| Advanced > No Entry Point                   | No                                 |
| System > Subsystem                          | Windows (`/SUBSYSTEM:WINDOWS,6.01`) |
| System > Minimum Required Version           | 6.01                               |
| Debugging > Generate Debug Info             | Yes (`/DEBUG`)                     |
| Debugging > Generate Program Database File  | `$(OutDir)skyrim_render_clean.pdb` |
| Debugging > Strip Private Symbols           | (path to a stripped pdb to ship if you want) |

The `/SUBSYSTEM:WINDOWS,6.01` flag is the second most important setting —
it sets Windows 7 SP1 as the minimum OS version. Without it, modern VS
toolsets may default to a Windows 10 floor that prevents loading on Win7.

`/DEBUG` plus `/OPT:REF` plus `/OPT:ICF` is the standard "ship optimized
release with separate symbols" combination. The `.pdb` does not get
embedded in the DLL; it can stay on your dev machine for crash analysis.

## What the binary should look like

After building, run from a Developer Command Prompt:

```
dumpbin /dependents Release\skyrim_render_clean.dll
```

You should see ONLY core Windows DLLs that exist on every Windows
install since XP. Acceptable output:

```
KERNEL32.dll
USER32.dll
d3d9.dll
D3DX9_42.dll
PSAPI.DLL
WINMM.dll
```

If you see ANY of these, the build is wrong — `/MT` didn't take and you
need to rebuild:

```
MSVCP140.dll      <- /MD release CRT - WRONG
MSVCP140D.dll     <- debug CRT - WRONG
MSVCR140.dll      <- WRONG
VCRUNTIME140.dll  <- WRONG
VCRUNTIME140D.dll <- debug - WRONG
api-ms-win-*.dll  <- UCRT not statically linked - WRONG
```

`D3DX9_42.dll` is fine — it ships with the DirectX runtime that every
Skyrim install already has.

## Toolset version

VS2022 (`v143`) recommended. VS2019 (`v142`) also works. Older toolsets
will compile but may not understand `/std:c++17` cleanly — if you must
use VS2017 or earlier, drop the C++17 flag and use `/std:c++14`. The
code does not require C++17 features.

## SDL3 / Vulkan

Not used. The `sdl3vk_bridge` module from older builds was removed —
no SDL or Vulkan dependencies in this version.

## Output layout (post-build)

```
Release\
  skyrim_render_clean.dll            <- the binary
  skyrim_render_clean.pdb            <- symbols (do not ship)
  skyrim_render_clean.exp            <- export library (do not ship)
  skyrim_render_clean.lib            <- import library (do not ship)
```

For shipping, you only need `skyrim_render_clean.dll` plus the
`skyrim_render_clean.ini` template. Place both in:

```
<Skyrim install>\Data\SKSE\Plugins\
```

## Test load (smoke check)

When the DLL loads correctly into Skyrim SE 1.5.97 with SKSE 1.7.3 +,
you should see in `Documents\My Games\Skyrim Special Edition\SKSE\skse.log`:

```
plugin <path>\skyrim_render_clean.dll (00000001 skyrim_render_clean 00000001) loaded correctly
```

If `[Profiling] Enabled=true` was set, the file `skyrim_render_clean.log`
will be created next to the DLL with session fingerprint and frame timing.

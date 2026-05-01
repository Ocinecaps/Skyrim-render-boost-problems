//
// dllmain.cpp - SkyrimRenderBoost SKSE plugin (v39).
//
// PHILOSOPHY
//   v39 is a pure measurement-quality build. It adds a session
//   fingerprint to the start of every log file (TESV.exe size/mtime,
//   display mode, timer resolution, loaded DLLs of interest) and dumps
//   a full frame-time histogram in every stats window. These changes
//   make A/B test logs self-documenting, so when comparing two runs
//   we can see immediately whether the test conditions actually matched.
//
//   The optimizations themselves are unchanged from v37/v38. The hot
//   path is byte-identical. v39 only adds logging.
//
//   See engine_cache_hooks.cpp for the portability/64-bit notes.
//
// LIFECYCLE
//   1. DLL_PROCESS_ATTACH: load INI; init logger if profiling is on.
//   2. SKSEPlugin_Load:    install hooks gated on g_cfg.Enabled.
//                          When Enabled=false, none of the optimization
//                          installs happen and the hot paths run vanilla
//                          D3D9.
//   3. (profiling on)      stats thread emits aggregate frame timing
//                          every 30 seconds. Hot-path Hook_Present
//                          additionally writes individual SPIKE events
//                          for frames above StutterThresholdMs.
//   4. DLL_PROCESS_DETACH: graceful uninstall on non-process-exit.
//

#include "Common.h"
#include "ini_config.h"
#include "hooks.h"
#include "engine_cache_hooks.h"
#include "device_hooks.h"
#include "matrix_math.h"
#include "session_fingerprint.h"
#include "function_counters.h"
#include "function_caches.h"

#include "common/ITypes.h"
#include "skse/skse/PluginAPI.h"
#include "skse/skse/skse_version.h"

#include <windows.h>

static PluginHandle      g_pluginHandle = kPluginHandle_Invalid;
static HANDLE            g_statsThread  = nullptr;
static volatile LONG     g_statsStop    = 0;

DWORD WINAPI StatsThreadProc(LPVOID)
{
    // 10s grace period so the first stats line lands AFTER the game's
    // initial loading screen and main menu, not during them. This makes
    // the very first window's numbers meaningful instead of polluted
    // by startup overhead.
    for (int i = 0; i < 10 && !g_statsStop; ++i) {
        Sleep(1000);
    }

    while (g_statsStop == 0) {
        for (int i = 0; i < 30 && g_statsStop == 0; ++i) {
            Sleep(1000);
        }
        if (g_statsStop != 0) break;

        // A/B test mode: flip engine cache hooks between windows so each
        // 30s frame-timing window can be tagged MOD-ON or MOD-OFF and
        // compared directly. The toggle happens BEFORE we dump LogStats
        // so the dump reflects what's about to be the state for the next
        // window. To make the first window MOD-ON (which is the more
        // intuitive read), we flip after the dump - the dump shows the
        // window we just FINISHED.
        DeviceHooks::LogStats();
        FunctionCaches::LogStats();
        if (IniConfig::g_cfg.HotspotProfiling) {
            FunctionCounters::DumpAndReset();
        }
        if (IniConfig::g_cfg.ABTestMode) {
            bool wasActive = EngineCacheHooks::IsActive();
            EngineCacheHooks::SetActive(!wasActive);
            // When flipping back to ON, the cache contents are stale -
            // every register is whatever the game last uploaded plus
            // whatever the cache had cached during the previous ON
            // window. Stale content is fine in correctness terms (the
            // cache is an optimization layer, not a source of truth)
            // but hit rates will spike low for the first few seconds
            // until the cache re-warms. That's expected.
        }
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        IniConfig::Load();
        if (IniConfig::g_cfg.ProfilingEnabled) {
            Logger::Initialize("skyrim_render_clean.log");
            LOG("=== DLL_PROCESS_ATTACH: skyrim_render_clean ===");
            LOG("INI Profiling=%d StutterMs=%d MaxSpikes=%d",
                (int)IniConfig::g_cfg.ProfilingEnabled,
                IniConfig::g_cfg.StutterThresholdMs,
                IniConfig::g_cfg.MaxSpikesPerWindow);
            LOG("INI Master Enabled=%d %s",
                (int)IniConfig::g_cfg.Enabled,
                IniConfig::g_cfg.Enabled
                    ? "(optimizations active)"
                    : "(optimizations DISABLED - vanilla baseline mode)");

            // Capture session metadata to the log header so future
            // log comparisons can detect when the test environment
            // changed (different game build, different display mode,
            // different timer resolution at attach, different mod
            // stack loaded). Fingerprint lives BEFORE we install any
            // hooks - the "before" state is what we want to record.
            SessionFingerprint::Emit();
        }
        break;

    case DLL_PROCESS_DETACH:
        // v38: log shutdown intent BEFORE we tear anything down, so the
        // log file ends with a recognizable marker even when the user
        // closes Skyrim normally. This makes it possible to distinguish
        // between "session ended cleanly" and "process crashed" when
        // looking at logs after the fact - the latter has no such line.
        if (lpReserved == nullptr) {
            LOG("=== DLL_PROCESS_DETACH: graceful unload (lpReserved=NULL) ===");
        } else {
            // Process exiting: still write a final line so logs are
            // self-documenting. After this we deliberately do NOT
            // unhook because threads may still be running.
            LOG("=== DLL_PROCESS_DETACH: process exit (skipping unhook for safety) ===");
        }

        // Only do real cleanup when we're being explicitly unloaded
        // (lpReserved == NULL). On process exit, threads may still be
        // running and reverting patches risks racing them.
        if (lpReserved == nullptr) {
            InterlockedExchange(&g_statsStop, 1);
            if (g_statsThread) {
                WaitForSingleObject(g_statsThread, 2000);
                CloseHandle(g_statsThread);
                g_statsThread = nullptr;
            }
            EngineCacheHooks::UninstallAll();
            DeviceHooks::Uninstall();
            MatrixMath::Uninstall();
            FunctionCaches::UninstallAll();
            FunctionCounters::UninstallAll();
            RemoveHooks();
            LOG("=== Cleanup complete ===");
        }
        Logger::Shutdown();
        break;
    }
    return TRUE;
}

extern "C" __declspec(dllexport)
bool SKSEPlugin_Query(const SKSEInterface* skse, PluginInfo* info)
{
    LOG("=== SKSEPlugin_Query ===");
    if (!info) return false;
    info->infoVersion = PluginInfo::kInfoVersion;
    info->name        = "skyrim_render_clean";
    info->version     = 1;
    if (!skse || skse->isEditor) return false;
    g_pluginHandle = skse->GetPluginHandle();
    return true;
}

extern "C" __declspec(dllexport)
bool SKSEPlugin_Load(const SKSEInterface* /*skse*/)
{
    LOG("=== SKSEPlugin_Load ===");

    // The D3D9 hook ALWAYS installs - it's how we get the device
    // pointer we need to install vtable hooks AND it's needed for
    // frame-timing / spike logging via Hook_Present even when the
    // master switch is OFF (vanilla baseline mode for comparison).
    //
    // When Enabled=false, Hook_Present runs profiling logic only and
    // forwards to the original. No state caching, no cache hooks.
    if (!InstallHooks()) {
        LOG("FATAL: InstallHooks failed");
        return false;
    }
    LOG("  D3D9 hook installed (always-on; profiling needs Hook_Present)");

    // -----------------------------------------------------------------
    // From here, every install is gated on the master switch.
    // When Enabled=false, all blocks log "skipped (master switch off)".
    // -----------------------------------------------------------------

    if (IniConfig::g_cfg.Enabled) {
        bool ok = EngineCacheHooks::InstallAll();
        LOG("  Engine cache hooks: %s", ok ? "installed" : "FAILED");

        int patched = MatrixMath::Install();
        LOG("  Matrix optimizations: %d D3DX9 math IAT slot(s) patched", patched);

        int caches = FunctionCaches::InstallAll();
        LOG("  Function caches: %d hot-function hook(s) installed", caches);
    } else {
        LOG("  Engine cache hooks: skipped (master switch off)");
        LOG("  Matrix optimizations: skipped (master switch off)");
        LOG("  Function caches: skipped (master switch off)");
    }

    // CacheHooks (D3D9 vtable cache) applies inside DeviceHooks, gated
    // at runtime on g_cfg.Enabled. No separate Install gate here.
    LOG("  CacheHooks runtime: %s",
        IniConfig::g_cfg.Enabled ? "ENABLED" : "DISABLED");

    if (IniConfig::g_cfg.Enabled && IniConfig::g_cfg.ABTestMode) {
        LOG("  A/B test mode: ENABLED - engine cache hooks will toggle every 30s");
        LOG("    First window will be MOD-ON, then alternating");
        // Make sure we start in the ON state.
        EngineCacheHooks::SetActive(true);
    }

    // Hotspot profiling - opt-in measurement that hooks a fixed list
    // of TESV.exe subroutines and counts calls per 30s window. Output
    // goes to a separate log file. Adds 1-3% overhead on hot paths -
    // production builds ship with this OFF.
    //
    // Requires ProfilingEnabled too; without it, no stats thread runs
    // to dump the counters.
    if (IniConfig::g_cfg.ProfilingEnabled && IniConfig::g_cfg.HotspotProfiling) {
        int n = FunctionCounters::InstallAll();
        LOG("  Hotspot profiling: %d function counter(s) installed", n);
    } else {
        LOG("  Hotspot profiling: disabled");
    }

    // Stats thread runs whenever profiling is on, regardless of master
    // switch. That's the point: we want stats in BOTH modes.
    if (IniConfig::g_cfg.ProfilingEnabled) {
        g_statsThread = CreateThread(nullptr, 0, StatsThreadProc, nullptr, 0, nullptr);
        if (g_statsThread) {
            LOG("  Stats thread started (frame timing every 30s, "
                "spike events when frame > %d ms)",
                IniConfig::g_cfg.StutterThresholdMs);
        }
    }

    return true;
}

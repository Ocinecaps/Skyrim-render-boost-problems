#pragma once
//
// session_fingerprint.h - emit self-describing session metadata to the log.
//
// PURPOSE
//   The single biggest pain point in our A/B testing methodology has
//   been: when comparing two log files days apart, you can't tell what
//   was different about the test conditions. Frame limiter on or off?
//   Same Skyrim build? Same resolution? Same other mods loaded?
//
//   This module writes a "session fingerprint" near the top of every
//   log file, capturing things that affect performance but aren't
//   visible from frame timing alone:
//
//     * TESV.exe file size and modification time (detects game updates)
//     * The mod DLL's own SHA-1 (detects which build was used)
//     * Display resolution as observed by Windows
//     * Process timer resolution at startup (before our SystemTuning runs)
//     * List of currently-loaded DLLs in the process matching common
//       mod loader patterns (skse, enb, reshade, dxvk, etc.)
//
//   No state is modified. No new threads. Pure introspection.
//

namespace SessionFingerprint {

// Emit the fingerprint block to the log. Safe to call before any hooks
// are installed. Should be called from dllmain very early (right after
// Logger::Initialize), since some of the data we collect (initial timer
// resolution, loaded modules at attach time) is more meaningful before
// our own modifications fire.
//
// Cost: ~10 ms once, never again. Logger::IsActive() gates the cost.
void Emit();

} // namespace SessionFingerprint

#pragma once
//
// ini_config.h - skyrim_render_clean.ini configuration.
//
// SCHEMA (deliberately minimal - one master toggle plus profiling):
//
//   [Master]
//   Enabled=true                ; master switch for all optimizations
//
//   [Profiling]
//   Enabled=false               ; write skyrim_render_clean.log
//   StutterThresholdMs=25       ; log per-frame spikes over this
//   MaxSpikesPerWindow=200      ; safety cap on per-window spike events
//   HotspotProfiling=false      ; opt-in: hook fixed list of TESV subs and
//                               ; count calls; output to a separate log
//
// SAFE DEFAULTS:
//   * Profiling=false (no log file written for normal play)
//   * HotspotProfiling=false (zero overhead unless explicitly enabled)
//   * Enabled=true (all optimizations on)
//
// When Enabled=false, all optimizations are skipped but profiling can
// still run, allowing same-DLL mod-on/mod-off comparisons without a
// rebuild or file swap.
//

namespace IniConfig {

struct Config {
    // [Master]
    bool        Enabled;            // false = all opts off, profiling still runs

    // [Profiling]
    bool        ProfilingEnabled;   // master log on/off
    int         StutterThresholdMs; // log individual spikes above this (ms). 0 = disabled
    int         MaxSpikesPerWindow; // safety cap to prevent log spam
    bool        HotspotProfiling;   // hook list of TESV subroutines, count calls, dump every 30s
    bool        ABTestMode;         // toggle engine cache hooks every 30s for A/B testing

    // [Optimization]
    bool        CacheB06250;        // sub_B06250 cache skip on hit (safe - pure arithmetic)
    bool        CacheCB7E80;        // sub_CB7E80 cache skip on hit (unsafe - reads globals)
    bool        CacheCA2610;        // sub_CA2610 cache skip on hit (unsafe - reads globals)
};

// Read INI from the directory containing the DLL. Auto-generates a
// default file if missing. Never fails; returns safe defaults if
// the INI is unreadable.
Config Load();

// Global config. Set once by Load(); read-only after.
extern Config g_cfg;

} // namespace IniConfig

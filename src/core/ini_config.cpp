//
// ini_config.cpp - INI loading for skyrim_render_clean.
//

#include "ini_config.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace IniConfig {

Config g_cfg = {};

static bool ReadBool(const char* section, const char* key, bool defVal,
                     const char* iniPath)
{
    char buf[32] = {};
    GetPrivateProfileStringA(section, key,
        defVal ? "true" : "false", buf, sizeof(buf), iniPath);
    if (_stricmp(buf, "true")  == 0) return true;
    if (_stricmp(buf, "false") == 0) return false;
    if (_stricmp(buf, "yes")   == 0) return true;
    if (_stricmp(buf, "no")    == 0) return false;
    if (_stricmp(buf, "1")     == 0) return true;
    if (_stricmp(buf, "0")     == 0) return false;
    return defVal;
}

static int ReadInt(const char* section, const char* key, int defVal,
                   const char* iniPath)
{
    return GetPrivateProfileIntA(section, key, defVal, iniPath);
}

static void GetIniPath(char* out, size_t cap)
{
    HMODULE hSelf = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&GetIniPath, &hSelf);
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(hSelf, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        strncpy_s(out, cap, "skyrim_render_clean.ini", _TRUNCATE);
        return;
    }
    char* dot = strrchr(path, '.');
    if (dot && (_stricmp(dot, ".dll") == 0)) {
        strcpy_s(dot, MAX_PATH - (dot - path), ".ini");
    } else {
        strncat_s(path, MAX_PATH, ".ini", _TRUNCATE);
    }
    strncpy_s(out, cap, path, _TRUNCATE);
}

static void WriteDefaultIni(const char* path)
{
    FILE* f = fopen(path, "w");
    if (!f) return;
    fputs(
        "; =====================================================================\r\n"
        "; skyrim_render_clean configuration\r\n"
        "; =====================================================================\r\n"
        ";\r\n"
        "; This mod adds CPU-side optimizations to Skyrim Special Edition's\r\n"
        "; rendering pipeline. It does not modify any game settings - your\r\n"
        "; Skyrim.ini and SkyrimPrefs.ini control LOD distances, shadow\r\n"
        "; quality, etc., as normal.\r\n"
        ";\r\n"
        "; Compatible with: ENB, ReShade, DXVK, SKSE, all visual mods.\r\n"
        ";\r\n"
        "\r\n"
        "; =====================================================================\r\n"
        "; HOW TO COMPARE MOD-ON VS MOD-OFF\r\n"
        "; =====================================================================\r\n"
        "; 1. Set [Profiling] Enabled=true\r\n"
        "; 2. Set [Master] Enabled=true. Play 60s in a fixed spot.\r\n"
        ";    Rename skyrim_render_clean.log to mod_on.log.\r\n"
        "; 3. Set [Master] Enabled=false. Restart Skyrim. Same\r\n"
        ";    spot, 60s. The log is now your vanilla baseline.\r\n"
        "; 4. Compare the avg/p50/p95/p99 values from the two logs.\r\n"
        ";\r\n"
        "; The DLL is the same in both runs - only the INI changed.\r\n"
        "; =====================================================================\r\n"
        "\r\n"
        "[Master]\r\n"
        "; *** MASTER KILL SWITCH ***\r\n"
        ";\r\n"
        "; When false, ALL optimizations are skipped - the DLL behaves like a\r\n"
        "; pass-through. Profiling/logging still runs if enabled.\r\n"
        "Enabled=true\r\n"
        "\r\n"
        "\r\n"
        "[Profiling]\r\n"
        "; Set true to write skyrim_render_clean.log with frame timing every\r\n"
        "; 30 seconds. Set false for normal play (no log file written).\r\n"
        "Enabled=false\r\n"
        "\r\n"
        "; Stutter detection: when an individual frame takes longer than\r\n"
        "; this many milliseconds, log a SPIKE event. 0 disables.\r\n"
        "StutterThresholdMs=25\r\n"
        "\r\n"
        "; Cap on how many SPIKE lines per 30s window.\r\n"
        "MaxSpikesPerWindow=200\r\n"
        "\r\n"
        "; Hotspot profiling: hook a fixed list of TESV.exe subroutines and\r\n"
        "; count how often each is called per 30s window. Output goes to a\r\n"
        "; separate file (skyrim_render_clean_hotspots.log). Used to identify\r\n"
        "; which functions are worth optimizing in future builds.\r\n"
        ";\r\n"
        "; Adds 1-3% overhead to hot-path frames. OFF for normal play.\r\n"
        "HotspotProfiling=false\r\n"
        "\r\n"
        "\r\n"
        "[Optimization]\r\n"
        "; Per-function cache toggles for the three hottest functions found\r\n"
        "; via hotspot profiling. Each one is INSTRUMENTED whenever the master\r\n"
        "; switch is on - hit/miss counters always run and log every 30s.\r\n"
        "; These flags only control whether a cache HIT actually skips the\r\n"
        "; function body (vs running it as normal but counting the duplicate).\r\n"
        ";\r\n"
        "; sub_B06250 - pure arithmetic, deterministic on its inputs.\r\n"
        ";              SAFE to skip duplicates. Default: true.\r\n"
        "CacheB06250=true\r\n"
        ";\r\n"
        "; sub_CB7E80 - reads mutable global state. Skipping risks visual\r\n"
        ";              artifacts. Instrumentation-only by default. Flip on\r\n"
        ";              after confirming a clean instrumented run.\r\n"
        "CacheCB7E80=false\r\n"
        ";\r\n"
        "; sub_CA2610 - same caveat as CB7E80 plus a 19-case dispatch.\r\n"
        ";              Instrumentation-only by default.\r\n"
        "CacheCA2610=false\r\n",
        f);
    fclose(f);
}

Config Load()
{
    Config c = {};

    // Safe defaults.
    c.Enabled              = true;
    c.ProfilingEnabled     = false;
    c.StutterThresholdMs   = 25;
    c.MaxSpikesPerWindow   = 200;
    c.HotspotProfiling     = false;
    c.ABTestMode           = false;  // off by default - requires explicit enable
    c.CacheB06250          = true;   // pure arithmetic, safe to skip duplicates
    c.CacheCB7E80          = false;  // instrument-only by default
    c.CacheCA2610          = false;  // instrument-only by default

    char iniPath[MAX_PATH] = {};
    GetIniPath(iniPath, sizeof(iniPath));

    DWORD attr = GetFileAttributesA(iniPath);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        WriteDefaultIni(iniPath);
        attr = GetFileAttributesA(iniPath);
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            g_cfg = c;
            return c;
        }
    }

    c.Enabled            = ReadBool("Master",       "Enabled",            c.Enabled,            iniPath);
    c.ProfilingEnabled   = ReadBool("Profiling",    "Enabled",            c.ProfilingEnabled,   iniPath);
    c.StutterThresholdMs = ReadInt ("Profiling",    "StutterThresholdMs", c.StutterThresholdMs, iniPath);
    c.MaxSpikesPerWindow = ReadInt ("Profiling",    "MaxSpikesPerWindow", c.MaxSpikesPerWindow, iniPath);
    c.HotspotProfiling   = ReadBool("Profiling",    "HotspotProfiling",   c.HotspotProfiling,   iniPath);
    c.ABTestMode         = ReadBool("Profiling",    "ABTestMode",         c.ABTestMode,         iniPath);
    c.CacheB06250        = ReadBool("Optimization", "CacheB06250",        c.CacheB06250,        iniPath);
    c.CacheCB7E80        = ReadBool("Optimization", "CacheCB7E80",        c.CacheCB7E80,        iniPath);
    c.CacheCA2610        = ReadBool("Optimization", "CacheCA2610",        c.CacheCA2610,        iniPath);

    g_cfg = c;
    return c;
}

} // namespace IniConfig

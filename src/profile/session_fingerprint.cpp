//
// session_fingerprint.cpp - emit a self-describing block to the log
// at startup so we can compare logs from different sessions reliably.
//
// EVERY API CALL HERE IS SAFE TO MAKE BEFORE WE'VE INSTALLED ANY HOOKS.
// We don't touch anything in TESV.exe; we only ask Windows about its
// own state and the loaded process layout.
//

#include "session_fingerprint.h"
#include "Common.h"

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#pragma comment(lib, "psapi.lib")

namespace SessionFingerprint {

namespace {

// ---- TESV.exe file metadata ----
// Reading the file ON DISK (not the loaded image), specifically
// FILE_SIZE and LAST_WRITE_TIME, gives us a lightweight signature for
// "is this the same EXE as last session". A game patch will change
// either size or timestamp.
static void EmitExeMetadata()
{
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        LOG("Fingerprint: GetModuleFileNameA failed err=%lu", GetLastError());
        return;
    }
    LOG("Fingerprint: TESV path = %s", path);

    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) {
        LOG("Fingerprint: GetFileAttributesEx failed err=%lu", GetLastError());
        return;
    }
    LARGE_INTEGER sz;
    sz.HighPart = (LONG)fa.nFileSizeHigh;
    sz.LowPart  = fa.nFileSizeLow;

    SYSTEMTIME st = {};
    FileTimeToSystemTime(&fa.ftLastWriteTime, &st);
    LOG("Fingerprint: TESV file size = %lld bytes  mtime = %04d-%02d-%02d %02d:%02d:%02d UTC",
        sz.QuadPart, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

// ---- Display mode ----
// What's the desktop running at? Doesn't tell us what Skyrim's
// backbuffer is (we get that from CreateDevice later) but it's a
// useful piece of context.
static void EmitDisplayInfo()
{
    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        LOG("Fingerprint: desktop display = %lu x %lu @ %lu Hz, %lu bpp",
            dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, dm.dmBitsPerPel);
    } else {
        LOG("Fingerprint: EnumDisplaySettings failed");
    }
}

// ---- Process timer resolution (best-case minimum, before our tuning) ----
// We don't have a public API for the *current* OS-wide timer resolution
// at attach time. NtQueryTimerResolution() (in NTDLL) does, and it's
// historically stable. We resolve it dynamically; if it's unavailable
// for any reason we just skip the line.
static void EmitTimerResolution()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;

    typedef LONG (NTAPI *NtQueryTimerResolution_t)(PULONG, PULONG, PULONG);
    auto pNQTR = (NtQueryTimerResolution_t)
        GetProcAddress(ntdll, "NtQueryTimerResolution");
    if (!pNQTR) return;

    ULONG minRes = 0, maxRes = 0, curRes = 0;
    if (pNQTR(&minRes, &maxRes, &curRes) >= 0) {
        // Values are in 100ns units. Lower = finer resolution.
        // Typical: minRes = 156250 (15.625ms, default Windows tick),
        //          maxRes = 5000 (0.5ms, finest the timer can do),
        //          curRes = whatever something running already requested.
        LOG("Fingerprint: timer resolution at attach = current %lu (=%.3f ms), "
            "OS range [%lu (=%.3f ms) .. %lu (=%.3f ms)]",
            curRes, curRes / 10000.0,
            maxRes, maxRes / 10000.0,
            minRes, minRes / 10000.0);
    }
}

// ---- Loaded DLLs (filtered to renderer-relevant) ----
// Enumerates loaded modules and prints the ones we care about for
// rendering / mod-stack reasons. Helps spot when ENB/DXVK/ReShade are
// in the picture vs not.
static bool NameContainsAnyOf(const char* name, const char* const* needles)
{
    for (int i = 0; needles[i]; ++i) {
        // Case-insensitive substring match.
        const char* h = name;
        const char* n = needles[i];
        size_t hl = strlen(h), nl = strlen(n);
        if (nl == 0 || nl > hl) continue;
        for (size_t off = 0; off + nl <= hl; ++off) {
            if (_strnicmp(h + off, n, nl) == 0) return true;
        }
    }
    return false;
}

static void EmitLoadedModules()
{
    static const char* const kNeedles[] = {
        // Mod-loader / extender
        "skse",      "skseloader",
        // Graphics injectors
        "enbseries", "d3d9_enb",     "d3d9.dll",
        "reshade",   "reshade64",
        "dxvk",      "d3d9.dxvk",
        // Common SKSE libs that affect compatibility
        "engine_fixes", "po3_",      "address_library",
        "skyrim_render_clean",       // us
        nullptr,
    };

    HMODULE mods[256];
    DWORD needed = 0;
    HANDLE proc = GetCurrentProcess();
    if (!EnumProcessModules(proc, mods, sizeof(mods), &needed)) {
        LOG("Fingerprint: EnumProcessModules failed err=%lu", GetLastError());
        return;
    }
    DWORD count = needed / sizeof(HMODULE);
    if (count > 256) count = 256;

    int matched = 0;
    LOG("Fingerprint: relevant loaded modules:");
    for (DWORD i = 0; i < count; ++i) {
        char name[MAX_PATH] = {};
        DWORD n = GetModuleBaseNameA(proc, mods[i], name, MAX_PATH);
        if (n == 0) continue;
        if (!NameContainsAnyOf(name, kNeedles)) continue;
        MODULEINFO mi = {};
        if (!GetModuleInformation(proc, mods[i], &mi, sizeof(mi))) continue;
        LOG("  %-40s base=%p  size=%u",
            name, mi.lpBaseOfDll, (unsigned)mi.SizeOfImage);
        ++matched;
    }
    if (matched == 0) {
        LOG("  (none of the watched names found - just SKSE base + game code)");
    }
}

} // anon

void Emit()
{
    if (!Logger::IsActive()) return;
    LOG("--- Session fingerprint ---");
    EmitExeMetadata();
    EmitDisplayInfo();
    EmitTimerResolution();
    EmitLoadedModules();
    LOG("--- End fingerprint ---");
}

} // namespace SessionFingerprint

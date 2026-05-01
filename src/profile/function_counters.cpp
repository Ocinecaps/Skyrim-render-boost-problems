//
// function_counters.cpp - per-function call-frequency profiler.
//
// See function_counters.h for design overview.
//
// IMPLEMENTATION DETAIL
// =====================
// At install time, for each slot:
//   1. Decode the function's leading instructions until we have >= 5
//      contiguous bytes that are safe to relocate (no rel-relative
//      ops within those bytes).
//   2. Allocate an executable thunk page. Lay out:
//          inc dword ptr [&counter]        ; FF 05 <abs32>
//          <copied prologue bytes>          ; N bytes
//          jmp rel32 -> original + N        ; E9 <rel32>
//   3. Patch the original function's prologue with: jmp rel32 -> thunk.
//      NOP-pad to N bytes.
//
// On any decode failure (instruction we don't recognize, prologue
// shorter than 5 bytes overall, internal jump that lands inside
// the bytes we'd copy) the slot is marked failed and skipped.
//

#include "function_counters.h"
#include "Common.h"
#include "ini_config.h"
#include "function_relocator.h"

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <algorithm>
#include <stdint.h>

namespace FunctionCounters {

// ---------------------------------------------------------------------
// Slot table - the 100 RVAs to hook.
// RVAs are relative to the TESV.exe imagebase (0x00400000).
// ---------------------------------------------------------------------
struct Slot {
    uint32_t      rva;
    const char*   name;          // "sub_XXXXXX" for log readability
    void*         original;      // resolved at install time (RVA + base)
    uint8_t*      thunk;         // our allocated counter+prologue+jmp
    int           prologueLen;   // bytes we copied
    volatile LONG count;         // call counter (non-atomic inc, see InstallSlot)
    bool          installed;
};

#define SLOT(addr) { 0x##addr - 0x00400000u, "sub_" #addr, nullptr, nullptr, 0, 0, false }

static Slot g_slots[] = {
    // ---- D3DX helper subroutines ----
    SLOT(CCB830), SLOT(CCC150), SLOT(F85780), SLOT(F85890),
    SLOT(CE7300), SLOT(F83830), SLOT(C8D2B0), SLOT(633400),
    SLOT(B06250), SLOT(C8E110), SLOT(C95FE0), SLOT(CB6F30),
    SLOT(C8DC50), SLOT(CA13C0), SLOT(CB3790), SLOT(CB8C20),
    SLOT(CD30C0), SLOT(C9CEB0), SLOT(CA2610), SLOT(CA9B10),
    SLOT(CAD9A0), SLOT(CAE5F0), SLOT(CB3900), SLOT(CB7750),
    SLOT(CB7E80), SLOT(CC1A50), SLOT(CCCCA0), SLOT(690D70),
    SLOT(CA2DA0), SLOT(CAD140), SLOT(CAD4D0), SLOT(CA1C10),
    SLOT(CA08C0), SLOT(CA1D10),

    // ---- Import-thunk callers ----
    SLOT(F79E22),

    // ---- Skyrim-specific ----
    SLOT(8E2AE0),

    // ---- NiDX9 renderer subroutines ----
    SLOT(102DD50), SLOT(CE8D30), SLOT(CE8190), SLOT(102DDF0),
    SLOT(102DD90), SLOT(102DDD0), SLOT(102DEC0), SLOT(102DE60),
    SLOT(102DDB0), SLOT(CEA5A0), SLOT(CEA610), SLOT(CEA770),
    SLOT(CD67F0), SLOT(CDAD60), SLOT(CEA930), SLOT(102DE40),
    SLOT(102DEE0), SLOT(CED3E0), SLOT(CED4B0), SLOT(CEC960),
    SLOT(CECAA0), SLOT(102DCF0), SLOT(CD98A0), SLOT(CDB620),
    SLOT(CE8F00), SLOT(CE9140), SLOT(CD6230), SLOT(CD57D0),
    SLOT(CD60D0), SLOT(CD8890), SLOT(CD2D20), SLOT(CD2A30),
    SLOT(CDA560), SLOT(CD8F20), SLOT(CD9580), SLOT(CD9690),
    SLOT(CE82C0), SLOT(CE83C0), SLOT(CE84D0), SLOT(CECDC0),
    SLOT(CECC60), SLOT(CECCC0), SLOT(CECD40), SLOT(CE6A30),
    SLOT(CE70E0), SLOT(CE6C90), SLOT(CE6920), SLOT(CE6B90),
    SLOT(CE7190), SLOT(CE9400), SLOT(102DE80), SLOT(102DEA0),
    SLOT(CE86F0), SLOT(CE8860), SLOT(CE89B0), SLOT(CE8AC0),
    SLOT(102DD70), SLOT(CEB1E0),
};

#undef SLOT

static const size_t kSlotCount = sizeof(g_slots) / sizeof(g_slots[0]);

static bool g_installed = false;

// Separate log handle for hotspots output. We don't want to clutter
// the main frame-timing log.
static FILE* g_hotspotLog = nullptr;
static CRITICAL_SECTION g_hotspotLogLock;
static bool g_hotspotLogInit = false;

// ---------------------------------------------------------------------
// Hotspot log - separate file from the main frame-timing log.
// ---------------------------------------------------------------------
static void OpenHotspotLog()
{
    if (g_hotspotLogInit) return;
    InitializeCriticalSection(&g_hotspotLogLock);
    g_hotspotLogInit = true;

    HMODULE hSelf = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&OpenHotspotLog, &hSelf);
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(hSelf, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        strncpy_s(path, MAX_PATH, "skyrim_render_clean_hotspots.log", _TRUNCATE);
    } else {
        char* dot = strrchr(path, '.');
        if (dot) {
            *dot = '\0';
            strncat_s(path, MAX_PATH, "_hotspots.log", _TRUNCATE);
        }
    }
    g_hotspotLog = fopen(path, "w");
    if (g_hotspotLog) {
        fprintf(g_hotspotLog,
            "# skyrim_render_clean - hotspot profiling log\n"
            "# Counts are calls per 30-second window, not cumulative.\n"
            "# Counts include all overhead from this profiling build.\n"
            "# Functions absent from a window report had zero calls or\n"
            "# failed to install (see install lines for which is which).\n"
            "#\n");
        fflush(g_hotspotLog);
    }
}

static void HotspotLog(const char* fmt, ...)
{
    if (!g_hotspotLog) return;
    EnterCriticalSection(&g_hotspotLogLock);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_hotspotLog, "[%02d:%02d:%02d.%03d] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_hotspotLog, fmt, ap);
    va_end(ap);
    fputc('\n', g_hotspotLog);
    fflush(g_hotspotLog);
    LeaveCriticalSection(&g_hotspotLogLock);
}

// ---------------------------------------------------------------------
// Build the per-slot thunk:
//
//   inc dword ptr [<addr of counter>]         FF 05 <abs32>        6 bytes
//   <copied prologue bytes>                                        N bytes (>=5)
//   jmp rel32 -> original + N                  E9 <rel32>           5 bytes
//
// We over-allocate one page (MEM_COMMIT|RESERVE PAGE_EXECUTE_READWRITE)
// per slot for simplicity. Page granularity is 4 KiB; we use ~32 bytes.
// Memory pressure for 100 slots: 400 KiB, negligible.
// ---------------------------------------------------------------------
static bool InstallSlot(Slot& s, uintptr_t imageBase)
{
    s.original = (void*)(imageBase + s.rva);

    // Use the relocator's prologue measurement. It handles the full
    // common-x86 instruction set including TEST imm (F6/F7), FPU ops
    // (D8-DF), and various MOV/LEA forms - far more than the v1
    // hand-rolled scanner did. Functions with a rel32 call/jump in
    // their first 5 bytes are still refused.
    const uint8_t* p = (const uint8_t*)s.original;
    bool hasInternalRel = false;
    int n = FunctionRelocator::MeasurePrologue(s.original, 5, &hasInternalRel);
    if (n == 0 || hasInternalRel) {
        return false;
    }
    s.prologueLen = n;

    // Allocate thunk page.
    uint8_t* thunk = (uint8_t*)VirtualAlloc(nullptr, 64,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!thunk) return false;

    // Layout:
    //   [0..5]   FF 05 <abs32>              inc dword ptr [counter]
    //   [6..5+n] copied prologue bytes
    //   [6+n..]  E9 <rel32>                 jmp original+n
    //
    // Note: non-atomic inc. We may undercount by a fraction of a percent
    // in rare cross-thread races, but the lock prefix costs ~5-10ns per
    // call. At 380k+ calls/sec across the hot functions, dropping it
    // saves measurable time vs the lossless count's accuracy gain.
    int off = 0;
    thunk[off++] = 0xFF;                  // INC mem
    thunk[off++] = 0x05;                  // ModR/M: /0, abs32
    uint32_t counterAddr = (uint32_t)(uintptr_t)&s.count;
    memcpy(thunk + off, &counterAddr, 4);
    off += 4;

    memcpy(thunk + off, p, n);            // original prologue bytes
    off += n;

    thunk[off++] = 0xE9;                  // JMP rel32
    int32_t backRel = (int32_t)(((uint8_t*)s.original + n) - (thunk + off + 4));
    memcpy(thunk + off, &backRel, 4);
    off += 4;

    FlushInstructionCache(GetCurrentProcess(), thunk, off);

    // Patch original entry: JMP rel32 -> thunk, NOP-pad to n.
    uint8_t* at = (uint8_t*)s.original;
    DWORD oldProtect = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(thunk, 0, MEM_RELEASE);
        return false;
    }
    at[0] = 0xE9;
    int32_t fwdRel = (int32_t)(thunk - (at + 5));
    memcpy(at + 1, &fwdRel, 4);
    for (int i = 5; i < n; ++i) at[i] = 0x90;
    DWORD tmp;
    VirtualProtect(at, n, oldProtect, &tmp);
    FlushInstructionCache(GetCurrentProcess(), at, n);

    s.thunk = thunk;
    s.installed = true;
    return true;
}

int InstallAll()
{
    if (g_installed) return 0;
    if (!IniConfig::g_cfg.ProfilingEnabled) {
        // Without profiling, the dump thread won't run, so installing
        // would just add overhead with no output. Refuse.
        return 0;
    }

    OpenHotspotLog();

    HMODULE tesv = GetModuleHandleW(NULL);
    if (!tesv) {
        HotspotLog("InstallAll: GetModuleHandleW(NULL) failed");
        return 0;
    }
    uintptr_t imageBase = (uintptr_t)tesv;
    HotspotLog("InstallAll: TESV.exe base = 0x%08X", (unsigned)imageBase);

    int ok = 0, failed = 0;
    for (size_t i = 0; i < kSlotCount; ++i) {
        Slot& s = g_slots[i];
        bool installed = InstallSlot(s, imageBase);
        if (installed) {
            ++ok;
        } else {
            ++failed;
            HotspotLog("  SKIP %s @ 0x%08X (unrecognized prologue)",
                s.name, (unsigned)(imageBase + s.rva));
        }
    }
    HotspotLog("InstallAll: %d installed, %d skipped (of %zu total)",
        ok, failed, kSlotCount);

    g_installed = true;
    return ok;
}

void UninstallAll()
{
    if (!g_installed) return;
    int restored = 0;
    for (size_t i = 0; i < kSlotCount; ++i) {
        Slot& s = g_slots[i];
        if (!s.installed) continue;
        // Best-effort restore: copy our thunk's prologue copy back into
        // the original. Thunk layout: 6 bytes counter + N prologue + 5 jmp.
        uint8_t* at = (uint8_t*)s.original;
        DWORD oldProtect = 0;
        if (VirtualProtect(at, s.prologueLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(at, s.thunk + 7, s.prologueLen);
            DWORD tmp;
            VirtualProtect(at, s.prologueLen, oldProtect, &tmp);
            FlushInstructionCache(GetCurrentProcess(), at, s.prologueLen);
            ++restored;
        }
        // Don't VirtualFree the thunk - the function being unhooked
        // could still be in mid-flight on another thread. Leak the
        // page; it's 64 bytes per slot.
        s.installed = false;
    }
    HotspotLog("UninstallAll: restored %d entries", restored);
    g_installed = false;
}

// ---------------------------------------------------------------------
// Periodic dump - sorts slots by call count, writes top entries,
// resets all counters atomically (read+swap).
// ---------------------------------------------------------------------
namespace {
struct Snapshot {
    const char* name;
    LONG        count;
};
}

void DumpAndReset()
{
    if (!g_installed || !g_hotspotLog) return;

    static int windowNum = 0;
    ++windowNum;

    // Snapshot + atomic reset. InterlockedExchange returns the prior
    // value, so we capture and clear in one op.
    Snapshot snaps[kSlotCount];
    LONG total = 0;
    int activeCount = 0;
    for (size_t i = 0; i < kSlotCount; ++i) {
        if (!g_slots[i].installed) {
            snaps[i].name  = g_slots[i].name;
            snaps[i].count = -1;        // sentinel: not installed
            continue;
        }
        LONG prev = InterlockedExchange(&g_slots[i].count, 0);
        snaps[i].name  = g_slots[i].name;
        snaps[i].count = prev;
        total += prev;
        ++activeCount;
    }

    // Sort descending by count. -1 (not installed) sorts to the end.
    std::sort(snaps, snaps + kSlotCount,
        [](const Snapshot& a, const Snapshot& b) {
            if (a.count < 0) return false;
            if (b.count < 0) return true;
            return a.count > b.count;
        });

    EnterCriticalSection(&g_hotspotLogLock);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_hotspotLog,
        "\n[%02d:%02d:%02d.%03d] --- Hotspot window %d  (%d hooks active, %ld total calls) ---\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        windowNum, activeCount, total);

    int shown = 0;
    int zeroCount = 0;
    int notInstalled = 0;
    for (size_t i = 0; i < kSlotCount; ++i) {
        if (snaps[i].count < 0)        { ++notInstalled; continue; }
        if (snaps[i].count == 0)       { ++zeroCount;    continue; }
        // Show all non-zero entries, sorted descending.
        double pct = total > 0 ? (100.0 * snaps[i].count / total) : 0.0;
        fprintf(g_hotspotLog,
            "  %-14s  %12ld  (%5.1f%%)\n",
            snaps[i].name, snaps[i].count, pct);
        ++shown;
    }
    if (zeroCount > 0) {
        fprintf(g_hotspotLog,
            "  (%d functions with zero calls this window)\n", zeroCount);
    }
    if (notInstalled > 0) {
        fprintf(g_hotspotLog,
            "  (%d functions skipped at install - see startup log)\n", notInstalled);
    }
    fflush(g_hotspotLog);
    LeaveCriticalSection(&g_hotspotLogLock);
}

} // namespace FunctionCounters

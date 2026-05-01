#pragma once

// =====================================================================
// Windows version baseline.
// =====================================================================
// Set BEFORE including <windows.h> so headers expose only Win7-era APIs.
// Without this, MSVC's STL pulls in chrono types that import
// GetSystemTimePreciseAsFileTime (Win8+ only). Older Skyrim users on
// Windows 7 then see "entry point ... not found in KERNEL32.dll" at
// load time and the DLL never runs.
//
// We also avoid <mutex> and <atomic> here for the same reason - they
// transitively pull in chrono on MSVC. We use CRITICAL_SECTION and
// InterlockedXxx instead.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif

#include <windows.h>
#include <cstdio>
#include <cstdarg>

// =====================================================================
// SkyrimRenderBoost - thread-safe file logger.
// =====================================================================
// Logger::Initialize is called from dllmain only when the INI says
// profiling is on. Otherwise s_active stays false and Logger::Write
// returns immediately on the atomic check.
//
// Cost when profiling off: 1 volatile load + branch.
// Cost when profiling on:  full file I/O on log lines themselves.
//
// HOT PATH RULE: nothing calls LOG from per-frame code paths.
// LOG is used from install/cleanup, the 30-second stats thread, and
// the new spike event recorder (which only fires on detected stutters,
// not every frame). The gate adds zero perceptible cost.

class Logger {
public:
    static void Initialize(const char* filename = "skyrim_render_clean.log") {
        EnsureInit();
        EnterCriticalSection(&s_cs);
        if (!s_file) {
            s_file = fopen(filename, "w");
            if (s_file) {
                InterlockedExchange(&s_active, 1);
                SYSTEMTIME st; GetLocalTime(&st);
                fprintf(s_file, "# SkyrimRenderBoost v39 - started %04d-%02d-%02d %02d:%02d:%02d\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                fflush(s_file);
            }
        }
        LeaveCriticalSection(&s_cs);
    }

    static void Shutdown() {
        if (s_csInit != 1) return;
        EnterCriticalSection(&s_cs);
        InterlockedExchange(&s_active, 0);
        if (s_file) { fclose(s_file); s_file = nullptr; }
        LeaveCriticalSection(&s_cs);
    }

    static bool IsActive() {
        return s_active != 0;
    }

    static void Write(const char* fmt, ...) {
        if (s_active == 0) return;
        EnterCriticalSection(&s_cs);
        if (s_file) {
            SYSTEMTIME st; GetLocalTime(&st);
            fprintf(s_file, "[%02d:%02d:%02d.%03d] ",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            va_list args; va_start(args, fmt); vfprintf(s_file, fmt, args); va_end(args);
            fputc('\n', s_file); fflush(s_file);
        }
        LeaveCriticalSection(&s_cs);
    }

private:
    // Lazy CRITICAL_SECTION init. Done before first use, idempotent
    // via InterlockedCompareExchange. Avoids global C++ ctor ordering.
    static void EnsureInit() {
        if (InterlockedCompareExchange(&s_csInit, 1, 0) == 0) {
            InitializeCriticalSection(&s_cs);
            InterlockedExchange(&s_csInit, 2);  // mark fully initialized
        } else {
            // Another thread already started or finished init.
            // Spin until init is complete (s_csInit == 2). In practice
            // only DllMain calls this first, so contention is negligible.
            while (s_csInit != 2) Sleep(0);
        }
    }

    inline static FILE*             s_file    = nullptr;
    inline static CRITICAL_SECTION  s_cs      = {};
    inline static volatile LONG     s_active  = 0;
    inline static volatile LONG     s_csInit  = 0;
};

#define LOG(fmt, ...) do { if (Logger::IsActive()) Logger::Write(fmt, ##__VA_ARGS__); } while (0)

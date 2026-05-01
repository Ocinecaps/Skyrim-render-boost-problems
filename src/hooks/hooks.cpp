//
// hooks.cpp - Inline detour on d3d9!Direct3DCreate9.
//
// Proven detour code, unchanged across many builds. Patches the
// function prologue in d3d9.dll's memory to JMP into our hook, with
// a trampoline that runs the saved bytes and jumps back.
//
#include "hooks.h"
#include "Common.h"
#include "d3d9_wrapper.h"

#include <windows.h>
#include <d3d9.h>
#include <cstdint>

namespace {
    using Direct3DCreate9_t = IDirect3D9* (WINAPI*)(UINT);

    BYTE* g_target       = nullptr;
    BYTE  g_savedBytes[5] = { 0 };
    BYTE* g_trampoline   = nullptr;
    bool  g_installed    = false;

    void WriteRelJmp(BYTE* at, const void* dest)
    {
        at[0] = 0xE9;
        int32_t rel = (int32_t)((BYTE*)dest - (at + 5));
        memcpy(at + 1, &rel, 4);
    }
}

extern "C" IDirect3D9* WINAPI HookedDirect3DCreate9(UINT SDKVersion)
{
    LOG("=== Direct3DCreate9 intercepted (SDK=%u) ===", SDKVersion);

    if (!g_trampoline) {
        LOG("  no trampoline available - cannot forward");
        return nullptr;
    }

    auto real = (Direct3DCreate9_t)g_trampoline;
    IDirect3D9* realD3D9 = real(SDKVersion);
    if (!realD3D9) {
        LOG("  real Direct3DCreate9 returned NULL");
        return nullptr;
    }

    HookedD3D9* wrapper = new HookedD3D9(realD3D9);
    LOG("  wrapped IDirect3D9: real=%p wrapper=%p", realD3D9, wrapper);
    return wrapper;
}

bool InstallHooks()
{
    if (g_installed) {
        LOG("InstallHooks: already installed");
        return true;
    }

    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) {
        d3d9 = LoadLibraryW(L"d3d9.dll");
        if (!d3d9) {
            LOG("InstallHooks: LoadLibraryW(d3d9.dll) failed (err=%lu)", GetLastError());
            return false;
        }
    }
    LOG("InstallHooks: d3d9.dll base = %p", d3d9);

    FARPROC pCreate = GetProcAddress(d3d9, "Direct3DCreate9");
    if (!pCreate) {
        LOG("InstallHooks: GetProcAddress failed (err=%lu)", GetLastError());
        return false;
    }
    BYTE* target = (BYTE*)pCreate;
    LOG("InstallHooks: d3d9!Direct3DCreate9 at %p", target);
    LOG("  prologue: %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X %02X",
        target[0], target[1], target[2], target[3], target[4],
        target[5], target[6], target[7], target[8], target[9], target[10]);

    for (int hops = 0; hops < 4 && target[0] == 0xE9; ++hops) {
        int32_t rel = *(int32_t*)(target + 1);
        BYTE* next  = target + 5 + rel;
        LOG("  prologue is a JMP; following to %p", next);
        target = next;
    }

    if (target[0] == 0xE8 || target[0] == 0xEB) {
        LOG("  prologue starts with relative-flow instruction 0x%02X, aborting", target[0]);
        return false;
    }

    memcpy(g_savedBytes, target, 5);

    g_trampoline = (BYTE*)VirtualAlloc(
        nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_trampoline) {
        LOG("InstallHooks: VirtualAlloc failed (err=%lu)", GetLastError());
        return false;
    }
    memcpy(g_trampoline, g_savedBytes, 5);
    WriteRelJmp(g_trampoline + 5, target + 5);

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG("InstallHooks: VirtualProtect failed (err=%lu)", GetLastError());
        VirtualFree(g_trampoline, 0, MEM_RELEASE);
        g_trampoline = nullptr;
        return false;
    }
    WriteRelJmp(target, (void*)&HookedDirect3DCreate9);
    DWORD tmp;
    VirtualProtect(target, 5, oldProtect, &tmp);
    FlushInstructionCache(GetCurrentProcess(), target, 5);

    g_target    = target;
    g_installed = true;

    LOG("InstallHooks: detour installed successfully");
    LOG("  hook fn   = %p", &HookedDirect3DCreate9);
    LOG("  trampoline= %p", g_trampoline);
    return true;
}

void RemoveHooks()
{
    if (!g_installed) return;

    if (g_target) {
        DWORD oldProtect = 0;
        if (VirtualProtect(g_target, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(g_target, g_savedBytes, 5);
            DWORD tmp;
            VirtualProtect(g_target, 5, oldProtect, &tmp);
            FlushInstructionCache(GetCurrentProcess(), g_target, 5);
            LOG("RemoveHooks: restored prologue");
        }
    }

    if (g_trampoline) {
        VirtualFree(g_trampoline, 0, MEM_RELEASE);
        g_trampoline = nullptr;
    }
    g_target    = nullptr;
    g_installed = false;
}

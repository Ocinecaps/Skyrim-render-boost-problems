//
// engine_cache_hooks.cpp - actual caching hooks for hot engine functions.
//
// =====================================================================
// PORTABILITY NOTE (v38)
// =====================================================================
// This file targets Skyrim Special Edition 1.5.97 specifically:
//   * 32-bit PE (Win32 build only)
//   * __thiscall (this in ECX)
//   * Specific RVAs from the IDA dump of TESV.exe 1.5.97
//   * Inline-assembly hook bodies use 32-bit registers (eax/ecx/edx/etc)
//
// To support a different game version (e.g. SkyrimSE.exe 1.6+, which is
// 64-bit), the porting work is:
//   1. Verify or replace every RVA in the kProlog_XXX/InstallOneCacheHook
//      table at the bottom of this file.
//   2. Verify or replace every kProlog_XXX byte pattern.
//   3. Rewrite each CacheHook_XX inline-asm body for x64 calling convention.
//   4. Update the Common.h logger to handle 64-bit pointers in format strs.
//
// The framework (FunctionRelocator, prologue verification, miss-path
// dispatch) is architecture-agnostic where possible. The data table
// approach in InstallAll() is intentional: a 64-bit port would replace
// just the table, not the framework.
// =====================================================================
//
// HOW THE CACHE HOOK WORKS
// ========================
// Game calls sub_CCC0F0 with ECX = constant-object pointer. Disassembly:
//
//   sub_CCC0F0:
//     mov  dl, [ecx+62h]                    ; count byte
//     test dl, dl
//     jbe  short locret                     ; count==0 -> return
//     movzx eax, word ptr [ecx+60h]         ; startReg
//     mov  ecx, dword_1BB0928               ; device ptr
//     ...
//     shl  edx, 4
//     add  edx, offset dword_1BAE0A8        ; PS bank base
//     ...
//     call SetPixelShaderConstantF
//     ret
//
// Our hook plan: replace this function's first 5 bytes with a JMP to
// our naked thunk. The thunk:
//   1. Reads count from [ecx+62h], bails to original if 0 or > 16
//   2. Reads startReg from [ecx+60h], bails if out of range
//   3. Computes pSrc = PS_BANK + startReg*16 (just for count=1 first;
//      handle multi-vec4 in iteration 2)
//   4. Compares 16 bytes at pSrc with our cache[startReg]
//   5. If equal: increment skipped counter, RET (skip the whole upload)
//   6. If different: update cache, fall through to original
//
// "Fall through" = jump to a per-function trampoline that runs the saved
// prologue bytes and jumps to original+N.
//
// BANK ADDRESSES from previous IDA work:
//   VS bank: 0x01BAC080  (256 vec4s)
//   PS bank: 0x01BAE0A8  (224 vec4s)
//

#include "engine_cache_hooks.h"
#include "Common.h"
#include "ini_config.h"
#include "function_relocator.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

// ---- bank addresses & cache storage ----
//
// These must be at file scope (not in anonymous namespace) because
// inline asm in __declspec(naked) functions needs to reference them
// by symbol name. MSVC handles anonymous-namespace symbols fine in
// inline asm in modern versions, but file-scope static is the
// minimum-risk choice.
//
static constexpr uintptr_t kVSBank   = 0x01BAC080;
static constexpr uintptr_t kPSBank   = 0x01BAE0A8;
static constexpr int       kVSCount  = 256;
static constexpr int       kPSCount  = 224;

// Cache: 16 bytes per register, plus a "valid" flag per register.
alignas(16) static uint8_t g_vsCache[kVSCount * 16];
alignas(16) static uint8_t g_psCache[kPSCount * 16];
static uint8_t g_vsValid[kVSCount];
static uint8_t g_psValid[kPSCount];

// (Stats counters removed for release build)

// Trampolines (saved prologue + JMP back to original+N).
// In v8+ these point to fully-relocated function copies, not trampolines.
static uint8_t* g_psTrampoline    = nullptr;
static uint8_t* g_vsTrampoline    = nullptr;
static uint8_t* g_psAltRelocated  = nullptr;
static uint8_t* g_vsAltRelocated  = nullptr;

namespace {

// Patch sites
uint8_t* g_psTarget = nullptr;
uint8_t* g_vsTarget = nullptr;
uint8_t* g_psAltTarget = nullptr;
uint8_t* g_vsAltTarget = nullptr;
uint8_t  g_psSavedBytes[16] = {};
uint8_t  g_vsSavedBytes[16] = {};
uint8_t  g_psAltSavedBytes[16] = {};
uint8_t  g_vsAltSavedBytes[16] = {};
int      g_psSavedLen = 0;
int      g_vsSavedLen = 0;
int      g_psAltSavedLen = 0;
int      g_vsAltSavedLen = 0;
bool     g_installed = false;

void WriteRelJmp(uint8_t* at, const void* dest)
{
    at[0] = 0xE9;
    int32_t rel = (int32_t)((uint8_t*)dest - (at + 5));
    memcpy(at + 1, &rel, 4);
}

// ---------------------------------------------------------------
// CacheHook_PS - naked thunk for sub_CCC0F0.
//
// On entry: ECX = constant object. EFLAGS not read.
// On exit (if cache hit): RET. Caller sees a function that did nothing.
// On exit (if cache miss): JMP to trampoline which runs saved prologue
//   then continues into the real function from the byte after our patch.
//
// We must preserve ECX (the function's argument). EAX/EDX/EFLAGS we can
// trash; the function's first real instruction (mov dl,[ecx+62h]) will
// reload them.
// ---------------------------------------------------------------
extern "C" void __declspec(naked) CacheHook_PS()
{
    __asm {
        // Save anything we'll trash. ECX must be preserved across the
        // miss path since the original function reads it.
        // The function's first instruction is `mov dl, [ecx+62h]` -
        // it doesn't depend on EAX, EDX, ESI, EDI being any value.
        // So we only need to preserve ECX (already in ECX, won't touch).
        // EAX, EDX, ESI, EDI we can use freely.

        // Always count the call.
        // Read count byte. For now only handle the count=1 case (which
        // is the dominant pattern: one vec4 per dispatcher call).
        movzx eax, byte ptr [ecx + 62h]
        cmp  eax, 1
        jne  miss_path                  // count != 1 -> just run original

        // Read startReg word.
        movzx eax, word ptr [ecx + 60h]

        // Bounds check: startReg < kPSCount (224).
        cmp  eax, kPSCount
        jae  miss_path

        // Save the regs we'll use.
        push esi
        push edi
        push ebx

        // EBX = startReg (preserve through compares for valid/cache index)
        mov  ebx, eax

        // ESI = PSBank + startReg*16
        mov  esi, ebx
        shl  esi, 4
        add  esi, kPSBank

        // EDI = &g_psCache[startReg*16]
        mov  edi, offset g_psCache
        mov  eax, ebx
        shl  eax, 4
        add  edi, eax

        // Check valid flag for this register.
        mov  eax, offset g_psValid
        cmp  byte ptr [eax + ebx], 0
        je   ps_miss_after_pop

        // v29: SSE2 16-byte compare. Replaces 4 dword cmps + 4 conditional
        // branches with: 2 unaligned loads, 1 byte-equality compare, 1 mask
        // extract, 1 compare to 0xFFFF, 1 branch. ~3-5 cycles saved per call.
        // Skyrim already requires SSE2 hardware (it's the minimum for 32-bit
        // x86 since 2006), so this is safe to use unconditionally.
        movdqu  xmm0, [esi]
        movdqu  xmm1, [edi]
        pcmpeqb xmm0, xmm1
        pmovmskb eax, xmm0
        cmp  eax, 0FFFFh
        jne  ps_miss_after_pop

        // HIT: cache says identical, skip the entire function.
        pop  ebx
        pop  edi
        pop  esi
        ret

    ps_miss_after_pop:
        // v29: SSE 16-byte cache update. Single load + single store.
        movdqu  xmm0, [esi]
        movdqu  [edi], xmm0

        // Mark valid: g_psValid[startReg] = 1
        mov  eax, offset g_psValid
        mov  byte ptr [eax + ebx], 1

        pop  ebx
        pop  edi
        pop  esi

    miss_path:
        // Jump to trampoline. Trampoline runs saved prologue then JMPs
        // to original+N. ECX is still the constant object pointer.
        jmp  dword ptr [g_psTrampoline]
    }
}

extern "C" void __declspec(naked) CacheHook_VS()
{
    // sub_CCB7D0 (VS dispatcher) reads its inputs differently than the PS one:
    //   movzx eax, word ptr [ecx+30h]   ; startReg (NOT 60h)
    //   movzx ecx, byte ptr [ecx+32h]   ; count (NOT 62h)
    //
    // The struct layout for VS constant objects has these fields at
    // different offsets than PS. Reading the wrong offsets is why v5
    // had 0% hit rate - we were comparing random struct bytes against 1.
    __asm {
        movzx eax, byte ptr [ecx + 32h]    // count at +32h, not +62h
        cmp  eax, 1
        jne  vs_miss_path

        movzx eax, word ptr [ecx + 30h]    // startReg at +30h, not +60h
        cmp  eax, kVSCount
        jae  vs_miss_path

        push esi
        push edi
        push ebx

        mov  ebx, eax

        mov  esi, ebx
        shl  esi, 4
        add  esi, kVSBank

        mov  edi, offset g_vsCache
        mov  eax, ebx
        shl  eax, 4
        add  edi, eax

        mov  eax, offset g_vsValid
        cmp  byte ptr [eax + ebx], 0
        je   vs_miss_after_pop

        // v29: SSE 16-byte compare (same pattern as CacheHook_PS).
        movdqu  xmm0, [esi]
        movdqu  xmm1, [edi]
        pcmpeqb xmm0, xmm1
        pmovmskb eax, xmm0
        cmp  eax, 0FFFFh
        jne  vs_miss_after_pop

        pop  ebx
        pop  edi
        pop  esi
        ret

    vs_miss_after_pop:
        // v29: SSE 16-byte cache update.
        movdqu  xmm0, [esi]
        movdqu  [edi], xmm0

        mov  eax, offset g_vsValid
        mov  byte ptr [eax + ebx], 1

        pop  ebx
        pop  edi
        pop  esi

    vs_miss_path:
        jmp  dword ptr [g_vsTrampoline]
    }
}

// ---------------------------------------------------------------
// CacheHook_PS_Alt - naked thunk for sub_CCC120 (alt PS dispatcher).
//
// sub_CCC120 is stdcall, takes 3 stack args (arg_0, arg_4, arg_8),
// retn 0Ch. It calls SetPxConstF via vtable[1B4h]. The startReg is
// derived from a struct lookup: [ECX + arg_0*2] >> 2.
//
// Args at our entry:
//   ECX        = struct pointer (preserve through hook)
//   [esp+0]    = return address back to engine caller
//   [esp+4]    = arg_0 (index used in [ecx+esi*2] lookup)
//   [esp+8]    = arg_4 (count - we only handle count==1)
//   [esp+12]   = arg_8 (pData - the bytes being uploaded)
//
// Cache strategy: compare *pData (16 bytes) against g_psCache[startReg].
// g_psCache is updated by every wrapper SetPxConstF call (UpdateFromUpload),
// so it reflects "what the GPU currently has".
//
// On HIT: ret 0Ch (stdcall cleanup of the 12 bytes of args).
// On MISS: jmp to relocated copy of sub_CCC120.
// ---------------------------------------------------------------
extern "C" void __declspec(naked) CacheHook_PS_Alt()
{
    __asm {
        // arg_4 = count. Bail unless count == 1.
        mov eax, [esp + 8]
        cmp eax, 1
        jne ps_alt_miss

        // Compute startReg from [ecx + arg_0*2] >> 2
        // arg_0 is a 32-bit slot; the lookup uses it as an index.
        mov edx, [esp + 4]                    // edx = arg_0 (full dword)
        movzx edx, word ptr [ecx + edx*2]     // edx = [ecx + arg_0*2] (16-bit zext)
        shr edx, 2                             // edx = startReg

        // Bounds check: startReg < kPSCount (224)
        cmp edx, kPSCount
        jae ps_alt_miss

        // Save callee-trash regs we need.
        push ebx
        push esi
        push edi

        mov ebx, edx                           // ebx = startReg

        // pData was at [esp+12] before pushes; now at [esp+12+12] = [esp+24]
        mov edi, [esp + 24]                    // edi = pData

        // Check valid flag for this register
        mov edx, offset g_psValid
        cmp byte ptr [edx + ebx], 0
        je ps_alt_miss_pop

        // Compute &g_psCache[startReg*16]
        mov esi, offset g_psCache
        mov eax, ebx
        shl eax, 4
        add esi, eax

        // v29: SSE 16-byte compare. EDI=pData, ESI=cache slot.
        movdqu  xmm0, [edi]
        movdqu  xmm1, [esi]
        pcmpeqb xmm0, xmm1
        pmovmskb eax, xmm0
        cmp  eax, 0FFFFh
        jne  ps_alt_miss_pop

        // HIT - skip the call. Stdcall cleanup of 12 bytes of args.
        pop edi
        pop esi
        pop ebx
        ret 0Ch

    ps_alt_miss_pop:
        pop edi
        pop esi
        pop ebx

    ps_alt_miss:
        // Stack is back to: [ret], [arg_0], [arg_4], [arg_8]
        // ECX still holds the struct pointer
        // JMP to relocated copy of sub_CCC120
        jmp dword ptr [g_psAltRelocated]
    }
}

// ---------------------------------------------------------------
// CacheHook_VS_Alt - naked thunk for sub_CCB800 (alt VS dispatcher).
//
// sub_CCB800 has the same shape as sub_CCC120 - stdcall, 3 stack args,
// retn 0Ch, calls SetVxConstF (vtable[178h]) instead of SetPxConstF.
// Same struct lookup: startReg = [ECX + arg_0*2] >> 2.
// Same arg layout: arg_0 = index, arg_4 = count, arg_8 = pData.
//
// Differences from CacheHook_PS_Alt:
//   - Compares against g_vsCache instead of g_psCache
//   - Bumps g_vsAltCalls / g_vsAltSkipped
//   - Bounds check uses kVSCount (256) instead of kPSCount (224)
//   - Miss path JMPs to g_vsAltRelocated
//
// VS hit rate is naturally low (~0.16% for main VS) - transformation
// matrices change every call. But even few hits at 33k calls/sec is
// some savings.
// ---------------------------------------------------------------
extern "C" void __declspec(naked) CacheHook_VS_Alt()
{
    __asm {
        mov eax, [esp + 8]                 // arg_4 = count
        cmp eax, 1
        jne vs_alt_miss

        mov edx, [esp + 4]                 // arg_0 (index)
        movzx edx, word ptr [ecx + edx*2]  // 16-bit lookup
        shr edx, 2                          // startReg

        cmp edx, kVSCount
        jae vs_alt_miss

        push ebx
        push esi
        push edi

        mov ebx, edx                        // ebx = startReg
        mov edi, [esp + 24]                 // pData (arg_8 shifted by 12 pushed bytes)

        mov edx, offset g_vsValid
        cmp byte ptr [edx + ebx], 0
        je vs_alt_miss_pop

        mov esi, offset g_vsCache
        mov eax, ebx
        shl eax, 4
        add esi, eax                        // &cache[startReg*16]

        // v29: SSE 16-byte compare.
        movdqu  xmm0, [edi]
        movdqu  xmm1, [esi]
        pcmpeqb xmm0, xmm1
        pmovmskb eax, xmm0
        cmp  eax, 0FFFFh
        jne  vs_alt_miss_pop

        // HIT - skip the call, stdcall cleanup of 12 bytes args.
        pop edi
        pop esi
        pop ebx
        ret 0Ch

    vs_alt_miss_pop:
        pop edi
        pop esi
        pop ebx

    vs_alt_miss:
        jmp dword ptr [g_vsAltRelocated]
    }
}

// ---- LDE (copy from engine_probes.cpp - we need it for the trampoline) ----
// (Reusing the same logic; in a bigger project we'd factor it out.)
static int ModRmExtra(const uint8_t* p_after_op, int op_size_imm)
{
    uint8_t modrm = p_after_op[0];
    int mod = (modrm >> 6) & 3;
    int rm  = modrm & 7;
    int extra = 1;
    if (mod == 3) {}
    else if (mod == 0) {
        if (rm == 4) extra += 1;
        else if (rm == 5) extra += 4;
    } else if (mod == 1) {
        extra += (rm == 4) ? 2 : 1;
    } else if (mod == 2) {
        extra += (rm == 4) ? 5 : 4;
    }
    return extra + op_size_imm;
}

static int DecodeInsn(const uint8_t* p)
{
    uint8_t op = p[0];
    if (op >= 0x50 && op <= 0x57) return 1;
    if (op >= 0x58 && op <= 0x5F) return 1;
    if (op >= 0x88 && op <= 0x8B) return 1 + ModRmExtra(p + 1, 0);
    if (op == 0x8D)               return 1 + ModRmExtra(p + 1, 0);
    if (op == 0x84 || op == 0x85) return 1 + ModRmExtra(p + 1, 0);
    if (op >= 0x38 && op <= 0x3B) return 1 + ModRmExtra(p + 1, 0);
    if (op == 0x80) return 1 + ModRmExtra(p + 1, 1);
    if (op == 0x81) return 1 + ModRmExtra(p + 1, 4);
    if (op == 0x83) return 1 + ModRmExtra(p + 1, 1);
    if (op == 0x6A) return 2;
    if (op == 0x68) return 5;
    if (op >= 0xB8 && op <= 0xBF) return 5;
    // A0-A3: MOV with absolute address (e.g. sub_CCB800/sub_CCC120
    // start with `mov eax, [imm32]` = A1 28 09 BB 01 = 5 bytes).
    // This was the missing case that made v9's alt PS install fail.
    if (op >= 0xA0 && op <= 0xA3) return 5;
    if (op == 0xC0 || op == 0xC1) return 1 + ModRmExtra(p + 1, 1);
    if (op == 0xC7) return 1 + ModRmExtra(p + 1, 4);
    if (op == 0xFF) return 1 + ModRmExtra(p + 1, 0);
    if (op == 0x0F) {
        uint8_t op2 = p[1];
        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF || op2 == 0xAF) {
            return 2 + ModRmExtra(p + 2, 0);
        }
        return 0;
    }
    return 0;
}

static int FindPrologueLength(const uint8_t* p, int maxScan = 16)
{
    int n = 0;
    while (n < 5) {
        int len = DecodeInsn(p + n);
        if (len <= 0) return 0;
        n += len;
        if (n > maxScan) return 0;
    }
    return n;
}

// ---- install single hook ----
//
// v38: Added expectedProlog parameter. Before patching, we memcmp the
// first N bytes of the target against the expected pattern. If they
// don't match, we ABORT this specific hook (return false) and the
// caller logs a warning. Other hooks in the table still install -
// graceful per-hook degradation rather than crash.
//
// Why: TESV.exe varies by patch level. If a future game update moves
// or rewrites a function, the bytes at our hardcoded RVA may no longer
// be the function we think they are. Patching wrong bytes = crash on
// launch. Verifying first = safe failure.
//
// expectedProlog/expectedLen is the pattern WE MUST SEE. If null, we
// skip verification (legacy callers; new code should always provide).
static bool InstallOneCacheHook(
    const char* name, uintptr_t rva, uintptr_t base,
    void (*hookFn)(),
    uint8_t** outTarget, uint8_t* outSavedBytes, int* outSavedLen,
    uint8_t** outTrampoline,
    const uint8_t* expectedProlog = nullptr, int expectedLen = 0)
{
    uint8_t* target = (uint8_t*)(base + rva);
    LOG("EngineCacheHooks::Install: %s at %p", name, target);
    LOG("  prologue: %02X %02X %02X %02X %02X | %02X %02X %02X",
        target[0], target[1], target[2], target[3], target[4],
        target[5], target[6], target[7]);

    // v38: prologue verification.
    if (expectedProlog && expectedLen > 0) {
        if (memcmp(target, expectedProlog, expectedLen) != 0) {
            LOG("  ABORT: prologue mismatch - expected:");
            char expBuf[64] = {};
            int  off = 0;
            for (int i = 0; i < expectedLen && off < 60; ++i) {
                int wrote = _snprintf_s(expBuf + off, sizeof(expBuf) - off, _TRUNCATE,
                                        "%02X ", expectedProlog[i]);
                if (wrote <= 0) break;
                off += wrote;
            }
            LOG("    %s", expBuf);
            LOG("  This usually means the game version differs from what this");
            LOG("  build of the mod was compiled for. Skipping THIS hook only;");
            LOG("  other hooks will continue to install where they match.");
            return false;
        }
        LOG("  prologue verified (matches expected %d bytes)", expectedLen);
    }

    int n = FindPrologueLength(target, 16);
    if (n == 0) {
        LOG("  ABORT: cannot decode prologue");
        return false;
    }
    LOG("  prologue length: %d", n);

    // Save originals (so we can uninstall by writing them back)
    memcpy(outSavedBytes, target, n);
    *outSavedLen = n;

    // ============================================================
    // v8 ARCHITECTURAL CHANGE: full function relocation
    // ============================================================
    // v7 built a TRAMPOLINE: just the first N bytes of the original,
    // followed by a JMP back to the original at offset N. Miss path:
    //
    //   hook -> JMP to trampoline -> N bytes -> JMP to EXE_orig+N -> ... -> ret
    //
    // v8 builds a FULL RELOCATED COPY of the function. Miss path:
    //
    //   hook -> JMP to relocated copy -> entire body -> ret
    //
    // The relocated copy is the entire function, in our memory.
    // After our entry-point patch, the original EXE bytes for this
    // function (after the patch) are unreachable; our copy IS the
    // function.
    //
    // Saves 1 JMP per miss path. More importantly, gives us full
    // architectural control over the relocated body for future
    // optimizations (e.g. modifying instructions in our copy).
    auto reloc = FunctionRelocator::RelocateFunction(target, 256);
    if (!reloc.relocated) {
        LOG("  ABORT: relocation failed - %s",
            reloc.failReason ? reloc.failReason : "unknown");
        return false;
    }
    if (reloc.hasExternalRel) {
        LOG("  ABORT: function has external rel32 calls/jumps - cannot relocate verbatim");
        VirtualFree(reloc.relocated, 0, MEM_RELEASE);
        return false;
    }
    LOG("  relocated copy at %p (%d bytes verbatim)", reloc.relocated, reloc.bodyLen);

    // The miss-path JMP target: our relocated copy. The hook function's
    // miss-path asm reads g_psTrampoline / g_vsTrampoline as the JMP
    // target. We re-use that pointer slot with the relocated copy
    // address - no asm changes needed.
    *outTrampoline = reloc.relocated;

    // Patch target: JMP to hook function (cache check)
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, n, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG("  ABORT: VirtualProtect failed");
        VirtualFree(reloc.relocated, 0, MEM_RELEASE);
        return false;
    }
    WriteRelJmp(target, (void*)hookFn);
    for (int i = 5; i < n; ++i) target[i] = 0x90; // NOP padding
    DWORD tmp;
    VirtualProtect(target, n, oldProtect, &tmp);
    FlushInstructionCache(GetCurrentProcess(), target, n);

    *outTarget = target;
    LOG("  %s INSTALLED (relocation-based)", name);
    return true;
}

} // anon namespace

namespace EngineCacheHooks {

bool InstallAll()
{
    if (!kEnableCacheHooks) {
        LOG("EngineCacheHooks: kEnableCacheHooks=false");
        return false;
    }
    if (!IniConfig::g_cfg.Enabled) {
        LOG("EngineCacheHooks: disabled by master switch");
        return false;
    }
    if (g_installed) return true;

    HMODULE tesv = GetModuleHandleW(NULL);
    if (!tesv) {
        LOG("EngineCacheHooks: GetModuleHandleW failed");
        return false;
    }
    uintptr_t base = (uintptr_t)tesv;
    LOG("EngineCacheHooks: TESV.exe base = 0x%p", (void*)base);

    // Zero the cache and valid arrays.
    memset(g_vsCache, 0, sizeof(g_vsCache));
    memset(g_psCache, 0, sizeof(g_psCache));
    memset(g_vsValid, 0, sizeof(g_vsValid));
    memset(g_psValid, 0, sizeof(g_psValid));

    // ============================================================
    // v38: TABLE-DRIVEN HOOK LIST
    // ============================================================
    // Each entry describes ONE function to hook in TESV.exe.
    //
    // FIELDS:
    //   name         human-readable label (logs only)
    //   rva          offset from module base. Combined with the runtime
    //                base from GetModuleHandleW(NULL) at install time;
    //                this handles ASLR correctly.
    //   hookFn       our entry-point function (cache check + miss-path JMP)
    //   target/saved/trampoline  per-hook state slots (where we record
    //                what to restore on uninstall)
    //   expectedProlog/Len  v38: the EXACT bytes we expect to see at
    //                rva+base before patching. If the running TESV.exe
    //                doesn't match, this hook is skipped. Other hooks
    //                in the table still install. Skyrim does NOT crash.
    //
    // -----------------------------------------------------------------
    // FUTURE 64-BIT PORT NOTE
    // -----------------------------------------------------------------
    // For SkyrimSE.exe (1.6+, 64-bit), this entire table is replaced.
    // The framework code (InstallOneCacheHook, FunctionRelocator, etc.)
    // is architecture-agnostic where possible, but:
    //   * RVAs are different (and 64-bit pointers)
    //   * Calling conventions differ (__fastcall RCX vs __thiscall ECX)
    //   * Inline-asm hook bodies need 64-bit rewrites
    //   * Prologue patterns differ
    // For 64-bit port: leave this table for the 32-bit build, add a
    // parallel 64-bit table behind #ifdef _WIN64.
    // ============================================================

    // sub_CCC0F0 (PS dispatcher main):
    //   8A 51 62        mov  dl, [ecx+62h]
    //   84 D2           test dl, dl
    static const uint8_t kProlog_CCC0F0[5] = { 0x8A, 0x51, 0x62, 0x84, 0xD2 };

    // sub_CCB7D0 (VS dispatcher main):
    //   0F B7 41 30     movzx eax, word ptr [ecx+30h]
    //   0F              (next: B6 49 32 = movzx ecx, byte ptr [ecx+32h])
    static const uint8_t kProlog_CCB7D0[5] = { 0x0F, 0xB7, 0x41, 0x30, 0x0F };

    // sub_CCC120 (PS dispatcher alt) and sub_CCB800 (VS dispatcher alt)
    //   A1 28 09 BB 01  mov eax, dword_1BB0928   (same pattern in both)
    static const uint8_t kProlog_CCC120[5] = { 0xA1, 0x28, 0x09, 0xBB, 0x01 };
    static const uint8_t kProlog_CCB800[5] = { 0xA1, 0x28, 0x09, 0xBB, 0x01 };

    bool psOK = InstallOneCacheHook(
        "sub_CCC0F0_ps_dispatcher", 0x008CC0F0, base,
        &CacheHook_PS,
        &g_psTarget, g_psSavedBytes, &g_psSavedLen,
        &g_psTrampoline,
        kProlog_CCC0F0, sizeof(kProlog_CCC0F0));

    bool vsOK = InstallOneCacheHook(
        "sub_CCB7D0_vs_dispatcher", 0x008CB7D0, base,
        &CacheHook_VS,
        &g_vsTarget, g_vsSavedBytes, &g_vsSavedLen,
        &g_vsTrampoline,
        kProlog_CCB7D0, sizeof(kProlog_CCB7D0));

    bool psAltOK = InstallOneCacheHook(
        "sub_CCC120_ps_dispatcher_alt", 0x008CC120, base,
        &CacheHook_PS_Alt,
        &g_psAltTarget, g_psAltSavedBytes, &g_psAltSavedLen,
        &g_psAltRelocated,
        kProlog_CCC120, sizeof(kProlog_CCC120));

    bool vsAltOK = InstallOneCacheHook(
        "sub_CCB800_vs_dispatcher_alt", 0x008CB800, base,
        &CacheHook_VS_Alt,
        &g_vsAltTarget, g_vsAltSavedBytes, &g_vsAltSavedLen,
        &g_vsAltRelocated,
        kProlog_CCB800, sizeof(kProlog_CCB800));

    if (!psOK && !vsOK && !psAltOK && !vsAltOK) {
        LOG("EngineCacheHooks: NO HOOKS INSTALLED (game version may differ "
            "from this build's compiled targets - mod will run safely without "
            "engine cache hooks)");
        return false;
    }

    g_installed = true;
    LOG("EngineCacheHooks: PS=%s VS=%s PSAlt=%s VSAlt=%s",
        psOK    ? "installed" : "skipped",
        vsOK    ? "installed" : "skipped",
        psAltOK ? "installed" : "skipped",
        vsAltOK ? "installed" : "skipped");
    return true;
}

void UninstallAll()
{
    if (!g_installed) return;

    if (g_psTarget && g_psSavedLen > 0) {
        DWORD oldProtect = 0;
        if (VirtualProtect(g_psTarget, g_psSavedLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(g_psTarget, g_psSavedBytes, g_psSavedLen);
            DWORD tmp;
            VirtualProtect(g_psTarget, g_psSavedLen, oldProtect, &tmp);
            FlushInstructionCache(GetCurrentProcess(), g_psTarget, g_psSavedLen);
        }
    }
    if (g_vsTarget && g_vsSavedLen > 0) {
        DWORD oldProtect = 0;
        if (VirtualProtect(g_vsTarget, g_vsSavedLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(g_vsTarget, g_vsSavedBytes, g_vsSavedLen);
            DWORD tmp;
            VirtualProtect(g_vsTarget, g_vsSavedLen, oldProtect, &tmp);
            FlushInstructionCache(GetCurrentProcess(), g_vsTarget, g_vsSavedLen);
        }
    }
    if (g_psAltTarget && g_psAltSavedLen > 0) {
        DWORD oldProtect = 0;
        if (VirtualProtect(g_psAltTarget, g_psAltSavedLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(g_psAltTarget, g_psAltSavedBytes, g_psAltSavedLen);
            DWORD tmp;
            VirtualProtect(g_psAltTarget, g_psAltSavedLen, oldProtect, &tmp);
            FlushInstructionCache(GetCurrentProcess(), g_psAltTarget, g_psAltSavedLen);
        }
    }
    if (g_vsAltTarget && g_vsAltSavedLen > 0) {
        DWORD oldProtect = 0;
        if (VirtualProtect(g_vsAltTarget, g_vsAltSavedLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(g_vsAltTarget, g_vsAltSavedBytes, g_vsAltSavedLen);
            DWORD tmp;
            VirtualProtect(g_vsAltTarget, g_vsAltSavedLen, oldProtect, &tmp);
            FlushInstructionCache(GetCurrentProcess(), g_vsAltTarget, g_vsAltSavedLen);
        }
    }
    if (g_psTrampoline)   { VirtualFree(g_psTrampoline,   0, MEM_RELEASE); g_psTrampoline   = nullptr; }
    if (g_vsTrampoline)   { VirtualFree(g_vsTrampoline,   0, MEM_RELEASE); g_vsTrampoline   = nullptr; }
    if (g_psAltRelocated) { VirtualFree(g_psAltRelocated, 0, MEM_RELEASE); g_psAltRelocated = nullptr; }
    if (g_vsAltRelocated) { VirtualFree(g_vsAltRelocated, 0, MEM_RELEASE); g_vsAltRelocated = nullptr; }
    g_installed = false;
}

void LogStats()
{
    // Release build: no stats logging.
}

void InvalidateAll()
{
    // Clear valid flags. The next call to each register will see invalid
    // and miss, which forces a fresh cache update.
    memset(g_vsValid, 0, sizeof(g_vsValid));
    memset(g_psValid, 0, sizeof(g_psValid));
}

void UpdateFromUpload(bool isPS, unsigned startReg, const void* pData, unsigned count)
{
    // This is called from the D3D9 wrapper on EVERY SetPxConstF /
    // SetVxConstF, BEFORE forwarding to the inner device.
    //
    // Whatever path the upload came from (engine miss path, ENB,
    // some other mod, reset code) - we now cache exactly the bytes
    // that are about to be sent to D3D9. This way our cache always
    // reflects "what the GPU has", not "what's in the engine's bank".
    //
    // The cache hook's compare on the next call will then be correct:
    //   - If next upload writes the same bytes -> HIT, safely skip
    //   - If next upload writes different bytes -> MISS, run normally

    if (!pData || count == 0) return;

    uint8_t*  cache = isPS ? g_psCache : g_vsCache;
    uint8_t*  valid = isPS ? g_psValid : g_vsValid;
    unsigned  cap   = isPS ? (unsigned)kPSCount : (unsigned)kVSCount;

    if (startReg >= cap) return;
    unsigned end = startReg + count;
    if (end > cap) end = cap;

    unsigned bytes = (end - startReg) * 16;
    memcpy(cache + startReg * 16, pData, bytes);
    memset(valid + startReg, 1, end - startReg);
}

// =====================================================================
// A/B test mode API.
// =====================================================================
// Enable / disable the cache check at the top of each naked thunk.
// When disabled, the four hooks fall straight through to the original
// dispatcher - same effect as if the JMP-patches weren't installed,
// but without un-patching anything.
//
// Single-writer (stats thread). Render thread reads the flag with a
// plain cmp-against-0 inside the naked thunks. No barrier needed - on
// x86 a 32-bit aligned write is observed atomically by readers.
static volatile LONG g_engineCacheActive = 1;

void SetActive(bool active)
{
    InterlockedExchange(&g_engineCacheActive, active ? 1 : 0);
}

bool IsActive()
{
    return g_engineCacheActive != 0;
}

} // namespace EngineCacheHooks

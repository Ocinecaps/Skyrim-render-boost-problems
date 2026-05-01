//
// function_caches.cpp - Cache layer for the three hottest profiled
// functions in TESV.exe.
//
// PATTERN
// =======
// For each target function we:
//   1. RelocateFunction() to get an executable copy of the original body.
//   2. Allocate a naked thunk that:
//        a. Hashes the call's inputs into a 32-bit FNV-1a digest.
//        b. Looks the digest up in a small per-function cache.
//        c. On HIT (and if SkipOnHit is enabled): increment hit counter,
//           do nothing (function's effect is already in the destination).
//        d. On MISS: increment miss counter, store digest, JMP to the
//           relocated copy. The relocated copy ends in a normal RETN and
//           returns to the caller as if nothing happened.
//   3. InstallRedirect() to patch the original entry point.
//
// SAFETY
// ======
// Only sub_B06250 has SkipOnHit enabled by default. It is pure arithmetic
// from input matrix to output struct - if inputs match a previous call,
// the output is bit-identical. Its 12-float output region is fully
// determined by its 4 args, no shared state.
//
// sub_CB7E80 and sub_CA2610 read mutable globals (current shader bind
// state at dword_1BABFB4 and current renderer state at dword_1BA76FC)
// before doing their matrix multiplies. We can't safely hash all that
// state without (a) bloating the cache key beyond the savings and
// (b) creating ABA bugs where two states hash the same. They run the
// original always; their hit counters tell us how often the IMMEDIATE
// inputs match previous calls, which is upper-bound information.
//
// CACHE STRUCTURE
// ===============
// Tiny direct-mapped table: 256 slots, indexed by low 8 bits of hash.
// Each slot stores the full hash. Collision = miss = function runs.
// Capacity intentionally tiny - we want to capture micro-redundancy
// (same inputs back-to-back), not session-wide deduplication.
//

#include "function_caches.h"
#include "Common.h"
#include "ini_config.h"
#include "function_relocator.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <intrin.h>

namespace FunctionCaches {

// ---------------------------------------------------------------------
// Tiny per-function hash cache.
// ---------------------------------------------------------------------
// 1024 entries × 4 bytes = 4 KB per cache. Three caches = 12 KB total,
// fits comfortably in L1. v1 used 256 entries but at 380k calls/sec
// every slot churned ~1500 times/sec, generating constant collision
// misses. Larger table dramatically reduces pseudo-misses without
// changing the algorithm.
static constexpr int kCacheSize = 1024;

struct HashCache {
    uint32_t entries[kCacheSize];   // last-seen hashes per slot

    void Reset() {
        // 0 is treated as "unset" - we OR the hash with 0x80000000 to
        // ensure no zero collision with empty slots.
        memset(entries, 0, sizeof(entries));
    }

    // Returns true if `hash` matches the current entry for that slot.
    // Always stores the new hash. (LRU within a slot, capacity 1 per slot.)
    inline bool TestAndStore(uint32_t hash) {
        uint32_t h = hash | 0x80000000u;
        uint32_t slot = h & (kCacheSize - 1);
        bool hit = (entries[slot] == h);
        entries[slot] = h;
        return hit;
    }
};

// FNV-1a over N bytes. Inlined for speed - this runs ~380k/sec.
static inline uint32_t Hash(const void* p, size_t bytes) {
    const uint8_t* b = (const uint8_t*)p;
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= b[i];
        h *= 0x01000193u;
    }
    return h;
}

// ---------------------------------------------------------------------
// Per-function state.
// ---------------------------------------------------------------------
struct CacheSite {
    const char*         name;
    uintptr_t           rva;          // RVA from imagebase
    void*               originalFn;   // resolved at install time
    void*               relocatedFn;  // RelocateFunction output
    int                 patchBytes;   // bytes overwritten by JMP at original entry
    bool                installed;
    bool                skipOnHit;    // false = report only, true = actually skip work
    HashCache           cache;
    volatile LONG       hits;
    volatile LONG       misses;
    volatile LONG       skips;        // hits where we actually skipped (== hits if skipOnHit)
};

// Three cache sites - addresses are RVAs (imagebase + this = absolute).
static CacheSite g_CB7E80 = { "sub_CB7E80",  0x008B7E80, nullptr, nullptr, 0, false, false, {}, 0, 0, 0 };
static CacheSite g_B06250 = { "sub_B06250",  0x00706250, nullptr, nullptr, 0, false, true,  {}, 0, 0, 0 };
static CacheSite g_CA2610 = { "sub_CA2610",  0x008A2610, nullptr, nullptr, 0, false, false, {}, 0, 0, 0 };

// ---------------------------------------------------------------------
// HASH-AND-CHECK helpers, called from the naked thunks below.
// Kept as __cdecl so inline asm doesn't have to set up __thiscall ECX.
//
// Each helper returns 1 if we should SKIP the original (hit + skipOnHit),
// or 0 if we should run it.
// ---------------------------------------------------------------------

// sub_CB7E80(this=ecx, arg_0)
//   Reads the matrix at *(arg_0+8) + 0x54 - this is the matrix passed
//   to sub_CDA170, which transforms it into the multiply inputs at
//   dword_1BA76FC+0x690 and +0x750. Same input matrix content -> same
//   transform output -> same multiply result.
//
//   We hash:
//     4 bytes: [ebx+0x454] flag word (controls whether 2nd multiply runs)
//    32 bytes: first half of matrix at *(arg_0+8) + 0x54
//
//   32 bytes (half a matrix) is enough for collision-free distinction
//   in practice; full 64 bytes would double the hash cost for no
//   measurable correctness gain on this workload.
//
//   Caveat: if another writer overwrites dword_1BAC080[slot] between
//   our two hits, skipping is unsafe. We accept this ABA risk because
//   the previous (pointer-only) hash had a worse one - it false-hit
//   even when the matrix CONTENT had changed. That was the cause of
//   the loading-screen visual glitches.
extern "C" int __cdecl Probe_CB7E80(uint32_t flagWord, uint32_t /*arg4*/, uint32_t arg8) {
    if (arg8 == 0) return 0;

    uint8_t buf[36];
    memcpy(buf, &flagWord, 4);
    // Original function does this same dereference (esi = [edi+8]; add esi, 0x54)
    // before any null check, so if arg8+0x54 is unreadable, the original would
    // crash too. We mirror its behavior.
    memcpy(buf + 4, (const uint8_t*)(uintptr_t)(arg8 + 0x54), 32);

    uint32_t h = Hash(buf, sizeof(buf));
    bool hit = g_CB7E80.cache.TestAndStore(h);
    if (hit) {
        ++g_CB7E80.hits;
        if (g_CB7E80.skipOnHit) {
            ++g_CB7E80.skips;
            return 1;
        }
    } else {
        ++g_CB7E80.misses;
    }
    return 0;
}

// sub_B06250(arg_0=src_matrix_ptr, arg_4=zero_check, arg_8=dst_struct_ptr)
//   Pure arithmetic from src_matrix contents to dst_struct contents.
//   We hash 16 floats (64 bytes) of src matrix contents + dst pointer.
//   If contents match a previous call AND dst pointer matches, the
//   destination already has the correct result.
//   This is the only one safe to skip.
extern "C" int __cdecl Probe_B06250(const void* srcMatrix, uint32_t zeroCheck, const void* dstStruct) {
    if (zeroCheck != 0) return 0;     // The original takes a fast path when arg_4 != 0.
    uint8_t buf[64 + 8];
    memcpy(buf,      srcMatrix, 64);  // 16 floats - the whole matrix
    memcpy(buf + 64, &dstStruct, 4);  // include dst ptr - same inputs to same dst = same result
    uint32_t z = 0;
    memcpy(buf + 68, &z, 4);
    uint32_t h = Hash(buf, sizeof(buf));
    bool hit = g_B06250.cache.TestAndStore(h);
    if (hit) {
        ++g_B06250.hits;
        if (g_B06250.skipOnHit) {
            ++g_B06250.skips;
            return 1;     // safe to skip - dst already contains the answer
        }
    } else {
        ++g_B06250.misses;
    }
    return 0;
}

// sub_CA2610(this=ecx, arg_0)
//   Same shape as CB7E80 but with a 19-case switch on bits from
//   [ecx+0xE4]. Different cases call different helpers (sub_CA1A70,
//   sub_CA0D20, sub_CA0BC0, sub_B06250) but ultimately produce a
//   matrix multiply written to dword_1BAC080[slot*4].
//
//   We hash:
//     4 bytes: dispatch bits (= which switch case fires)
//    32 bytes: first half of matrix at *(arg_0+8) + 0x54
//
//   Same caveat as CB7E80 about ABA risk on the destination slot.
extern "C" int __cdecl Probe_CA2610(uint32_t dispatchBits, uint32_t /*arg4*/, uint32_t arg8) {
    if (arg8 == 0) return 0;

    uint8_t buf[36];
    memcpy(buf, &dispatchBits, 4);
    memcpy(buf + 4, (const uint8_t*)(uintptr_t)(arg8 + 0x54), 32);

    uint32_t h = Hash(buf, sizeof(buf));
    bool hit = g_CA2610.cache.TestAndStore(h);
    if (hit) {
        ++g_CA2610.hits;
        if (g_CA2610.skipOnHit) {
            ++g_CA2610.skips;
            return 1;
        }
    } else {
        ++g_CA2610.misses;
    }
    return 0;
}

// ---------------------------------------------------------------------
// NAKED THUNKS - the actual hook entry points. Each must:
//   1. Save volatile state (eax, ecx, edx, flags) so the original's
//      register inputs are preserved if we end up calling it.
//   2. Push the cache-key inputs and call the appropriate Probe_*.
//   3. If probe returns 1: pop preserved state and emit the original's
//      epilogue (return to caller, balancing the stack as the original
//      would have).
//   4. If probe returns 0: pop preserved state and JMP to the
//      relocated original.
//
// Stack-balance note: sub_B06250 is __cdecl - caller cleans the stack -
// so to skip, we just RETN without touching args. sub_CB7E80 and
// sub_CA2610 are __thiscall with 1 stack arg - to skip, we'd RETN 4
// to balance. (We don't actually skip those, but the asm is set up
// correctly in case skipOnHit is later flipped on for them.)
// ---------------------------------------------------------------------

extern "C" void Hook_CB7E80();
extern "C" void Hook_B06250();
extern "C" void Hook_CA2610();

// Trampoline pointers - must be defined BEFORE the naked thunks that
// reference them via inline asm. Set by InstallOne() at install time.
extern "C" void* g_CB7E80_relocated = nullptr;
extern "C" void* g_B06250_relocated = nullptr;
extern "C" void* g_CA2610_relocated = nullptr;

__declspec(naked) void Hook_CB7E80() {
    __asm {
        // ECX = this, [esp+4] = arg_0
        push ebp
        mov  ebp, esp
        // Save volatile registers used by the call: only eax/ecx/edx are
        // caller-saved by __thiscall, but we also need to preserve ECX
        // because the original function expects it as 'this'.
        push ecx
        push edx
        push eax

        // Build cache-key inputs:
        //   flagWord = [ecx+0x454] (the bit field tested at 00CB7E99)
        //   arg4     = [ebp+8+arg_0+4]  - indirect through arg_0
        //   arg8     = [ebp+8+arg_0+8]  - indirect through arg_0
        mov  eax, [ecx + 0x454]
        mov  edx, [ebp + 8]            ; edx = arg_0
        push dword ptr [edx + 8]       ; arg8 input to probe
        push dword ptr [edx + 4]       ; arg4 input to probe
        push eax                       ; flagWord input to probe
        call Probe_CB7E80
        add  esp, 12

        test eax, eax
        jnz  skip_CB7E80

        // Cache miss - run the original.
        pop  eax
        pop  edx
        pop  ecx
        pop  ebp
        jmp  [g_CB7E80_relocated]

      skip_CB7E80:
        // Cache hit + skipOnHit. Return to caller. __thiscall with 1 arg
        // means callee cleans 4 bytes of stack args.
        pop  eax
        pop  edx
        pop  ecx
        pop  ebp
        ret  4
    }
}

__declspec(naked) void Hook_B06250() {
    __asm {
        // __cdecl: [esp+4] = src_matrix, [esp+8] = zero_check, [esp+12] = dst_struct
        push ebp
        mov  ebp, esp
        push ecx
        push edx
        push eax

        // Push args to probe in same order: src_matrix, zero_check, dst_struct
        push dword ptr [ebp + 16]      ; dst_struct
        push dword ptr [ebp + 12]      ; zero_check
        push dword ptr [ebp + 8]       ; src_matrix
        call Probe_B06250
        add  esp, 12

        test eax, eax
        jnz  skip_B06250

        pop  eax
        pop  edx
        pop  ecx
        pop  ebp
        jmp  [g_B06250_relocated]

      skip_B06250:
        // Cache hit + skipOnHit. __cdecl - caller cleans args - we just RETN.
        pop  eax
        pop  edx
        pop  ecx
        pop  ebp
        ret
    }
}

__declspec(naked) void Hook_CA2610() {
    __asm {
        // ECX = this, [esp+4] = arg_0
        push ebp
        mov  ebp, esp
        push ecx
        push edx
        push eax

        // Compute dispatch bits the same way the original does at 00CA2616:
        //   eax = [ecx+0xE4]
        //   eax >>= 0x15
        //   eax &= 0x3F
        // This gives us the switch index (the function dispatches 19 cases
        // off this).
        mov  eax, [ecx + 0xE4]
        shr  eax, 0x15
        and  eax, 0x3F
        mov  edx, [ebp + 8]            ; arg_0
        push dword ptr [edx + 8]
        push dword ptr [edx + 4]
        push eax
        call Probe_CA2610
        add  esp, 12

        test eax, eax
        jnz  skip_CA2610

        pop  eax
        pop  edx
        pop  ecx
        pop  ebp
        jmp  [g_CA2610_relocated]

      skip_CA2610:
        pop  eax
        pop  edx
        pop  ecx
        pop  ebp
        ret  4
    }
}

// ---------------------------------------------------------------------
// Install / uninstall.
// ---------------------------------------------------------------------
// Pattern: prologue-only trampoline.
//
//   At install time:
//     1. MeasurePrologue() finds N bytes (>=5) of decodable prologue.
//     2. We allocate a small RWX trampoline. Layout:
//          [0..N-1]  copy of original prologue bytes
//          [N..N+4]  JMP rel32 back to (original + N)
//     3. We patch original entry with JMP rel32 -> our naked thunk.
//        NOP-pad to N bytes.
//     4. The naked thunk's cache-miss path JMPs to the trampoline,
//        which executes the prologue and returns to the body.
//
//   This avoids the "whole function copy" trap. The function body
//   stays in TESV - external rel32 calls and FPU instructions inside
//   the body never need to be relocated.
//
//   Saved bytes for uninstall: copied prologue lives at trampoline[0..N-1].
//
static bool InstallOne(CacheSite& s, void (*hookFn)(), void** trampolineSlot, uintptr_t imageBase) {
    s.originalFn = (void*)(imageBase + s.rva);

    // Measure prologue. We need at least 5 bytes to fit JMP rel32.
    bool hasInternalRel = false;
    int prologueLen = FunctionRelocator::MeasurePrologue(s.originalFn, 5, &hasInternalRel);
    if (prologueLen == 0) {
        LOG("FunctionCaches: %s: MeasurePrologue failed", s.name);
        return false;
    }
    if (hasInternalRel) {
        // A rel32 call/jump within the first 5+ bytes - unusual for
        // a function prologue. We'd have to fix up the displacement
        // when copying. For our three target functions this won't fire,
        // but we refuse rather than silently corrupt.
        LOG("FunctionCaches: %s: prologue contains rel32 (rare), refusing", s.name);
        return false;
    }
    s.patchBytes = prologueLen;

    // Allocate trampoline: prologue bytes + 5-byte JMP back.
    size_t trampolineSize = prologueLen + 5;
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, (trampolineSize + 15) & ~15,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        LOG("FunctionCaches: %s: VirtualAlloc failed", s.name);
        return false;
    }

    // Copy prologue verbatim.
    memcpy(tramp, s.originalFn, prologueLen);

    // Append JMP rel32 -> (original + prologueLen).
    uint8_t* jmpAt = tramp + prologueLen;
    uint8_t* jmpTarget = (uint8_t*)s.originalFn + prologueLen;
    jmpAt[0] = 0xE9;
    int32_t rel = (int32_t)(jmpTarget - (jmpAt + 5));
    memcpy(jmpAt + 1, &rel, 4);

    FlushInstructionCache(GetCurrentProcess(), tramp, trampolineSize);

    s.relocatedFn = tramp;
    *trampolineSlot = tramp;

    // Install JMP rel32 -> hookFn at the original entry. This patches
    // the live executable memory in TESV.
    if (!FunctionRelocator::InstallRedirect(s.originalFn, (const void*)hookFn, prologueLen)) {
        LOG("FunctionCaches: %s: InstallRedirect failed", s.name);
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }

    s.cache.Reset();
    s.installed = true;
    LOG("FunctionCaches: %s INSTALLED at %p (prologue %d bytes, trampoline %p, skipOnHit=%s)",
        s.name, s.originalFn, prologueLen, tramp, s.skipOnHit ? "yes" : "no");
    return true;
}

int InstallAll() {
    if (!IniConfig::g_cfg.Enabled) {
        LOG("FunctionCaches: skipped (master switch off)");
        return 0;
    }

    HMODULE tesv = GetModuleHandleW(NULL);
    if (!tesv) {
        LOG("FunctionCaches: GetModuleHandleW(NULL) failed");
        return 0;
    }
    uintptr_t base = (uintptr_t)tesv;
    LOG("FunctionCaches: TESV.exe base = 0x%p", (void*)base);

    // Apply per-function INI overrides for skipOnHit.
    //
    // SAFETY OVERRIDE: regardless of what the INI says, NONE of the three
    // caches actually skip work in this build. They install, hash inputs,
    // and count hit/miss rates - that's it.
    //
    // All three functions write to memory locations (VS bank dword_1BAC080
    // for CB7E80/CA2610, the dst_struct passed by caller for B06250) that
    // can be overwritten between our two calls by other writers we don't
    // see. Our hash matches "same inputs as last time", but the destination
    // may have been clobbered by someone else's call in between, so the
    // previous output is no longer there. Skipping in that case leaves
    // stale data that the next reader sees as wrong - this manifests as
    // visual glitches.
    //
    // The fix is "ABA-aware" caching: store a snapshot of the destination
    // alongside the input hash, and verify it still matches before
    // skipping. That's a future change because it requires a different
    // thunk shape (must capture output AFTER the function runs, not
    // just before). Until then, we measure but don't skip.
    g_B06250.skipOnHit = false;  // SAFETY OVERRIDE - ABA risk on dst_struct
    g_CB7E80.skipOnHit = false;  // SAFETY OVERRIDE - ABA risk on VS bank slot
    g_CA2610.skipOnHit = false;  // SAFETY OVERRIDE - ABA risk on VS bank slot
    (void)IniConfig::g_cfg.CacheB06250;
    (void)IniConfig::g_cfg.CacheCB7E80;
    (void)IniConfig::g_cfg.CacheCA2610;

    int ok = 0;
    if (InstallOne(g_CB7E80, &Hook_CB7E80, &g_CB7E80_relocated, base)) ++ok;
    if (InstallOne(g_B06250, &Hook_B06250, &g_B06250_relocated, base)) ++ok;
    if (InstallOne(g_CA2610, &Hook_CA2610, &g_CA2610_relocated, base)) ++ok;
    return ok;
}

void UninstallAll() {
    // Best-effort restore. The relocator's Install patched a JMP into the
    // original entry; the saved copy is the relocated body. Recovering the
    // original prologue requires copying it back from `relocatedFn` (which
    // begins with the verbatim prologue bytes).
    CacheSite* sites[] = { &g_CB7E80, &g_B06250, &g_CA2610 };
    for (auto* s : sites) {
        if (!s->installed) continue;
        DWORD oldProtect = 0;
        if (VirtualProtect(s->originalFn, s->patchBytes, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(s->originalFn, s->relocatedFn, s->patchBytes);
            DWORD tmp;
            VirtualProtect(s->originalFn, s->patchBytes, oldProtect, &tmp);
            FlushInstructionCache(GetCurrentProcess(), s->originalFn, s->patchBytes);
        }
        s->installed = false;
        // Don't VirtualFree relocatedFn - other threads may be in the middle
        // of executing it. Tiny leak, no impact.
    }
}

void LogStats() {
    CacheSite* sites[] = { &g_CB7E80, &g_B06250, &g_CA2610 };
    LOG("--- Function caches ---");
    for (auto* s : sites) {
        if (!s->installed) {
            LOG("  %s: not installed", s->name);
            continue;
        }
        LONG hits   = s->hits;
        LONG misses = s->misses;
        LONG skips  = s->skips;
        LONG total  = hits + misses;
        double hitPct  = total > 0 ? (100.0 * hits / total)  : 0.0;
        double skipPct = total > 0 ? (100.0 * skips / total) : 0.0;
        LOG("  %s: %ld hits / %ld misses (%.1f%% hit), %ld skipped (%.1f%% skipped) [skipOnHit=%s]",
            s->name, hits, misses, hitPct, skips, skipPct,
            s->skipOnHit ? "yes" : "no");
    }
}

} // namespace FunctionCaches

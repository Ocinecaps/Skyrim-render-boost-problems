//
// function_relocator.cpp
//
// Verbatim function body copy with safety validation.
//
// We scan the source function instruction-by-instruction (using the
// same LDE pattern as engine_probes/engine_cache_hooks), and:
//   1. Determine the function's end (a top-level `ret` or `retn imm`)
//   2. Detect any rel32 calls (E8) or rel32 jumps (E9) that target
//      addresses OUTSIDE [src, src+bodyLen). These would need
//      per-instruction relocation, which we don't do yet.
//   3. Copy the body verbatim to allocated RWX memory.
//
// Internal rel8 / rel32 jumps that stay within the function ARE safe
// to copy verbatim because the relative offsets are preserved when we
// copy contiguously.
//

#include "function_relocator.h"
#include "Common.h"

#include <windows.h>
#include <cstring>
#include <cstdint>

namespace {

int ModRmExtra(const uint8_t* p, int op_size_imm)
{
    uint8_t modrm = p[0];
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

// Returns instruction length, plus output flags about the instruction.
// outIsTerminator: true if this instruction ends the function (ret/retn/jmp)
// outRel32CallOrJmp: nonzero = the rel32 displacement of this instruction
//                    (E8 or E9). 0 = not a rel32 call/jmp. Use to detect
//                    external calls/jumps.
int Decode(const uint8_t* p, bool& outIsTerminator, int32_t& outRel32Disp,
           uint8_t& outOpcode)
{
    outIsTerminator = false;
    outRel32Disp = 0;
    outOpcode = p[0];
    uint8_t op = p[0];

    // 0x66: operand-size override prefix. The instruction that follows
    // is decoded normally. For our purposes the only length impact is
    // on imm16-vs-imm32 forms (e.g. mov ax,imm16 = 66 B8 imm16 = 4
    // bytes vs mov eax,imm32 = B8 imm32 = 5 bytes). For modrm forms
    // without immediates, the 0x66 just prepends a byte.
    //
    // We handle this by recursing and adding 1.
    if (op == 0x66) {
        bool isTerm = false;
        int32_t r = 0;
        uint8_t op2 = 0;
        int sub = Decode(p + 1, isTerm, r, op2);
        if (sub == 0) return 0;
        // Adjust imm-bearing forms whose length changes with the prefix:
        //   B8-BF: mov r,imm32 -> mov r,imm16 (1 byte shorter)
        if (op2 >= 0xB8 && op2 <= 0xBF) sub -= 2;       // 5 -> 3
        else if (op2 == 0x68)            sub -= 2;       // push imm32 -> push imm16
        else if (op2 == 0x81)            sub -= 2;       // group with imm32 -> imm16
        // We don't bother with all imm-changing cases - they don't appear
        // in our target functions. Just guard: if recurse returned a
        // length that doesn't match for our common cases, the user can
        // add more here.
        outIsTerminator = isTerm;
        outRel32Disp = r;
        outOpcode = op2;
        return 1 + sub;
    }

    // ret variants
    if (op == 0xC3) { outIsTerminator = true; return 1; }       // ret
    if (op == 0xC2) { outIsTerminator = true; return 3; }       // retn imm16

    // jmp variants
    if (op == 0xEB) {                                            // jmp rel8
        outIsTerminator = true;
        return 2;
    }
    if (op == 0xE9) {                                            // jmp rel32
        outIsTerminator = true;
        outRel32Disp = *(const int32_t*)(p + 1);
        return 5;
    }
    if (op == 0xFF) {                                            // group: includes jmp r/m32
        uint8_t modrm = p[1];
        int reg = (modrm >> 3) & 7;
        int len = 1 + ModRmExtra(p + 1, 0);
        // /4 = jmp r/m32 (terminator)
        // /5 = jmp m16:32 (terminator, far)
        if (reg == 4 || reg == 5) outIsTerminator = true;
        return len;
    }

    // call rel32
    if (op == 0xE8) {
        outRel32Disp = *(const int32_t*)(p + 1);
        return 5;
    }

    // Conditional rel8 jumps (70-7F)
    if (op >= 0x70 && op <= 0x7F) return 2;

    // Conditional rel32 jumps (0F 80 - 0F 8F)
    if (op == 0x0F) {
        uint8_t op2 = p[1];
        if (op2 >= 0x80 && op2 <= 0x8F) return 6;
        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF || op2 == 0xAF) {
            return 2 + ModRmExtra(p + 2, 0);
        }
        return 0;
    }

    // PUSH/POP reg (50-5F)
    if (op >= 0x50 && op <= 0x5F) return 1;

    // ---- ARITHMETIC GROUPS WITH MODRM (00-3B except 0x0F) ----
    // Pattern: opcodes ending in 0,1,2,3 are r/m,r and r,r/m forms
    // for the eight arithmetic ops:
    //   00-03 ADD, 08-0B OR, 10-13 ADC, 18-1B SBB,
    //   20-23 AND, 28-2B SUB, 30-33 XOR, 38-3B CMP
    // The (op & 0x04) == 0 filter excludes the AL,imm8 / EAX,imm32
    // immediate-only forms (04, 05, 0C, 0D, ...) and segment ops
    // (06, 07, 0E, 16, 17, ...) which all have bit 0x04 set.
    if (op <= 0x3B && (op & 0x04) == 0) {
        return 1 + ModRmExtra(p + 1, 0);
    }
    // AL,imm8 forms (04, 0C, 14, 1C, 24, 2C, 34, 3C): 2 bytes each
    if (op <= 0x3F && (op & 0x07) == 0x04) return 2;
    // EAX,imm32 forms (05, 0D, 15, 1D, 25, 2D, 35, 3D): 5 bytes each
    if (op <= 0x3F && (op & 0x07) == 0x05) return 5;

    // MOV r/m,r and r,r/m (88-8B)
    if (op >= 0x88 && op <= 0x8B) return 1 + ModRmExtra(p + 1, 0);
    // LEA r,m
    if (op == 0x8D)               return 1 + ModRmExtra(p + 1, 0);

    // TEST r/m,r (84/85)
    if (op == 0x84 || op == 0x85) return 1 + ModRmExtra(p + 1, 0);

    // MOV with absolute address (A0-A3): 5 bytes (1 op + 4-byte address)
    if (op >= 0xA0 && op <= 0xA3) return 5;

    // Group 1 with imm8 (80, 83) and imm32 (81)
    if (op == 0x80) return 1 + ModRmExtra(p + 1, 1);
    if (op == 0x81) return 1 + ModRmExtra(p + 1, 4);
    if (op == 0x83) return 1 + ModRmExtra(p + 1, 1);

    // Push immediate
    if (op == 0x6A) return 2;
    if (op == 0x68) return 5;

    // MOV reg, imm32 (B8-BF)
    if (op >= 0xB8 && op <= 0xBF) return 5;

    // C0/C1: shift/rotate r/m, imm8 (e.g. shl edx, 4)
    //   C0 r/m8, imm8; C1 r/m32, imm8
    if (op == 0xC0 || op == 0xC1) return 1 + ModRmExtra(p + 1, 1);

    // C6 r/m8, imm8;  C7 r/m32, imm32
    if (op == 0xC6) return 1 + ModRmExtra(p + 1, 1);
    if (op == 0xC7) return 1 + ModRmExtra(p + 1, 4);

    // D0/D1: shift/rotate r/m by 1
    // D2/D3: shift/rotate r/m by CL
    if (op >= 0xD0 && op <= 0xD3) return 1 + ModRmExtra(p + 1, 0);

    // ---- Group F6/F7: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV r/m ----
    //   F6 /0 ib = TEST r/m8, imm8     (length: 1 + modrm + 1)
    //   F6 /1 ib = TEST r/m8, imm8     (alias)
    //   F6 /2..7 = NOT/NEG/MUL/IMUL/DIV/IDIV r/m8  (length: 1 + modrm)
    //   F7 /0 id = TEST r/m32, imm32   (length: 1 + modrm + 4)
    //   F7 /1 id = TEST r/m32, imm32   (alias)
    //   F7 /2..7 = NOT/NEG/MUL/IMUL/DIV/IDIV r/m32 (length: 1 + modrm)
    // The reg field of ModR/M selects the operation. /0 and /1 are TEST
    // and carry an immediate; /2-/7 do not.
    if (op == 0xF6) {
        uint8_t modrm = p[1];
        int reg = (modrm >> 3) & 7;
        int extra = ModRmExtra(p + 1, 0);
        int immLen = (reg <= 1) ? 1 : 0;
        return 1 + extra + immLen;
    }
    if (op == 0xF7) {
        uint8_t modrm = p[1];
        int reg = (modrm >> 3) & 7;
        int extra = ModRmExtra(p + 1, 0);
        int immLen = (reg <= 1) ? 4 : 0;
        return 1 + extra + immLen;
    }

    // ---- FPU instructions D8-DF ----
    // All FPU instructions are exactly 1 byte opcode + ModR/M + memory disp.
    // The ModR/M decoding is identical to integer ModR/M. None of them
    // are control flow.
    if (op >= 0xD8 && op <= 0xDF) {
        return 1 + ModRmExtra(p + 1, 0);
    }

    // ---- Misc single-byte instructions ----
    if (op == 0x90) return 1;       // NOP
    if (op == 0x9C) return 1;       // PUSHFD
    if (op == 0x9D) return 1;       // POPFD
    if (op == 0xC9) return 1;       // LEAVE
    if (op == 0xCC) return 1;       // INT 3
    if (op == 0xF5) return 1;       // CMC
    if (op == 0xF8) return 1;       // CLC
    if (op == 0xF9) return 1;       // STC
    if (op == 0xFC) return 1;       // CLD
    if (op == 0xFD) return 1;       // STD

    // ---- INC/DEC reg (40-4F) ----
    if (op >= 0x40 && op <= 0x4F) return 1;

    // ---- XCHG reg with EAX (90 is NOP, but 91-97 are real XCHG) ----
    if (op >= 0x91 && op <= 0x97) return 1;

    // ---- CDQ / CWD ----
    if (op == 0x99) return 1;       // CDQ
    if (op == 0x98) return 1;       // CWDE

    // Unknown - bail
    return 0;
}

} // anon

namespace FunctionRelocator {

RelocationResult RelocateFunction(const void* src, int maxScan)
{
    RelocationResult r{};
    const uint8_t* p = (const uint8_t*)src;

    // Walk forward, decoding instructions, until we hit a terminator.
    int n = 0;
    bool foundTerminator = false;
    bool hasExternal = false;
    const char* failReason = nullptr;

    while (n < maxScan) {
        bool isTerm = false;
        int32_t rel32 = 0;
        uint8_t op = 0;
        int len = Decode(p + n, isTerm, rel32, op);

        if (len == 0) {
            failReason = "unknown instruction encountered";
            break;
        }

        // Check rel32 calls/jumps for external targets.
        // If the displacement points outside [0, maxScan) from current
        // position, it's external.
        if (rel32 != 0) {
            int targetOffsetInFunc = n + len + rel32;
            if (targetOffsetInFunc < 0 || targetOffsetInFunc >= maxScan) {
                hasExternal = true;
            }
        }

        n += len;

        if (isTerm) {
            foundTerminator = true;
            break;
        }
    }

    if (!foundTerminator) {
        r.failReason = failReason ? failReason : "no terminator within scan range";
        return r;
    }

    r.bodyLen = n;
    r.hasExternalRel = hasExternal;

    // Allocate executable memory and copy.
    uint8_t* copy = (uint8_t*)VirtualAlloc(nullptr, (n + 15) & ~15,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!copy) {
        r.failReason = "VirtualAlloc failed";
        return r;
    }
    memcpy(copy, p, n);
    FlushInstructionCache(GetCurrentProcess(), copy, n);

    r.relocated = copy;
    LOG("FunctionRelocator: src=%p len=%d external=%d -> %p",
        src, n, hasExternal ? 1 : 0, copy);

    // If there are external rel32 calls in our copy, those calls now
    // point to wrong addresses. We'd need to fix them up (rewrite
    // each E8/E9 with a corrected rel32). For now we just flag it -
    // the caller can decide whether to use the relocated copy or fall
    // back to prologue patching.
    if (hasExternal) {
        LOG("  WARN: function has external rel32 calls/jumps - relocated copy unsafe to JMP into");
    }

    return r;
}

bool InstallRedirect(void* src, const void* relocated, int patchBytes)
{
    if (patchBytes < 5) return false;
    uint8_t* at = (uint8_t*)src;

    DWORD oldProtect = 0;
    if (!VirtualProtect(at, patchBytes, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    // E9 rel32
    at[0] = 0xE9;
    int32_t rel = (int32_t)((const uint8_t*)relocated - (at + 5));
    memcpy(at + 1, &rel, 4);
    for (int i = 5; i < patchBytes; ++i) at[i] = 0x90;

    DWORD tmp;
    VirtualProtect(at, patchBytes, oldProtect, &tmp);
    FlushInstructionCache(GetCurrentProcess(), at, patchBytes);
    return true;
}

// Measure the prologue length needed to fit a 5-byte JMP patch.
// Walks instructions from `src` until the cumulative byte count reaches
// at least `minBytes` (5 for a JMP rel32). Refuses to split an
// instruction across the patch boundary.
//
// Returns the byte count, or 0 on failure.
//
// Unlike RelocateFunction, this does NOT require finding a terminator
// and does NOT care about external rel32 calls past the prologue.
// The caller will only COPY these prologue bytes - the rest of the
// function body remains in its original location, so external calls
// in the body fix themselves automatically.
//
// One caveat: if a rel32 call/jump appears WITHIN the prologue itself
// (rare - typical prologues are sub esp / push regs), the caller must
// fix up the displacement when copying. We return failReason set in
// that case via the out param.
int MeasurePrologue(const void* src, int minBytes, bool* outHasInternalRel)
{
    if (outHasInternalRel) *outHasInternalRel = false;
    const uint8_t* p = (const uint8_t*)src;
    int n = 0;
    while (n < minBytes) {
        bool isTerm = false;
        int32_t rel32 = 0;
        uint8_t op = 0;
        int len = Decode(p + n, isTerm, rel32, op);
        if (len == 0) return 0;
        if (rel32 != 0 && outHasInternalRel) *outHasInternalRel = true;
        n += len;
        // If we hit a function terminator inside the prologue, the function
        // is shorter than 5 bytes - nothing we can do.
        if (isTerm && n < minBytes) return 0;
    }
    return n;
}

} // namespace FunctionRelocator

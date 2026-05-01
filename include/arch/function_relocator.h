#pragma once
//
// function_relocator.h - copy entire function bodies into our own
// allocated memory, where we can modify them as we wish.
//
// THE BIG IDEA (per user's request)
// =================================
// Instead of patching a function's prologue with a JMP and trying to
// thunk-then-fall-through, we copy the entire function body to memory
// we control, then patch the original entry to JMP to our copy.
//
// Why this is more powerful than prologue patches:
//   - We can rewrite the body (inject cache checks at any point, not
//     just the entry)
//   - We can intercept internal control flow (replace a `call edx`
//     with our own implementation)
//   - The original function is preserved untouched in EXE memory; we
//     work entirely in our memory pages
//
// CONSTRAINTS
// ===========
// Functions with rel32 calls or rel32 jumps to OUTSIDE the function
// can't be naively memcpy'd - their relative offsets would point to
// the wrong place. We detect such functions and refuse to relocate
// them (they need per-instruction relocation, which is a real LDE).
//
// The hot functions we care about (sub_CCB7D0, sub_CCC0F0, sub_CD39C0)
// are all relocatable: their internal jumps stay relative to the copy,
// and their external calls go through registers (`call edx`, etc).
//

#include <cstdint>

namespace FunctionRelocator {

struct RelocationResult {
    uint8_t* relocated;       // Pointer to the copy. Executable. Or nullptr if failed.
    int      bodyLen;         // Bytes copied.
    bool     hasExternalRel;  // True if function has rel32 calls/jumps - then we can't safely modify yet.
    const char* failReason;   // If relocated == nullptr.
};

// Copy a function from `src` for at most `maxScan` bytes (until we find
// the function's end via a `ret` or `jmp` at top level). Returns a
// pointer to the relocated copy plus metadata.
//
// The relocated copy is in memory we VirtualAlloc'd as RWX. The caller
// can modify it before installing the redirect.
RelocationResult RelocateFunction(const void* src, int maxScan = 256);

// After modification, install the redirect: patch `src` (in EXE memory)
// with a JMP rel32 -> `relocated`. NOP-pad as needed. Returns true on
// success.
bool InstallRedirect(void* src, const void* relocated, int patchBytes);

// Measure how many bytes of the function's prologue we need to copy
// to fit a 5-byte JMP patch. Walks instructions from `src` until the
// cumulative byte count reaches `minBytes` (typically 5). Refuses to
// split an instruction across the patch boundary.
//
// Returns the byte count, or 0 if the function is too short or starts
// with an instruction we can't decode. Sets *outHasInternalRel = true
// if a rel32 call/jump exists within the measured prologue (rare for
// standard prologues - caller must fix up rel32 displacements when
// copying if this is set).
int MeasurePrologue(const void* src, int minBytes, bool* outHasInternalRel);

} // namespace FunctionRelocator

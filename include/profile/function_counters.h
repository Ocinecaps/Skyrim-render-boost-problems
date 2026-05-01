#pragma once
//
// function_counters.h - lightweight call-frequency profiling for a
// fixed list of TESV.exe subroutines.
//
// PURPOSE
// =======
// Pure measurement, not optimization. Each entry installs a 5-byte
// JMP at the function entry point that lands in a tiny per-function
// thunk. The thunk does:
//
//     lock inc dword ptr [counter]
//     <copy of the original prologue bytes>
//     jmp <original_entry + prologue_len>
//
// The function executes normally; we just observe how often it's
// called. Overhead per call is ~5-8 ns (one locked increment +
// the unconditional taken jump). For functions called <100k/sec
// the overhead is invisible. For million+/sec callees it shows up
// as 1-3% on the frame budget - acceptable for profiling builds.
//
// OUTPUT
// ======
// When [Profiling] HotspotProfiling=true, every 30 seconds the stats
// thread writes a sorted "top callers" section to a separate log
// file (skyrim_render_clean_hotspots.log). The main frame-timing
// log is unaffected.
//
// FAILURE MODE
// ============
// Best-effort install: each function attempts independently. Any
// function whose prologue we can't safely relocate is skipped and
// logged. The counters for skipped functions remain zero, which
// makes "skipped" indistinguishable from "never called" in the
// output - the install log is the source of truth on what actually
// got hooked.
//
// SAFETY
// ======
// Disabled by default. Profiling builds use this module to identify
// hot functions that are worth optimizing in subsequent iterations.
// Production builds ship with HotspotProfiling=false.
//

#include <cstdint>

namespace FunctionCounters {

// Install all counters listed in the slot table. Returns the number of
// successful installs. Per-function failures are logged.
int InstallAll();

// Reverse the patches at module shutdown. Best-effort.
void UninstallAll();

// Called by the stats thread every 30 seconds when HotspotProfiling
// is true. Writes a sorted top-callers section to the hotspots log
// and resets all counters for the next window.
void DumpAndReset();

} // namespace FunctionCounters

#pragma once
//
// function_caches.h - Caches the three hottest functions identified by
// the v1 profiling pass:
//
//   sub_CB7E80  - 162k calls/sec - upload world*viewProj to VS bank
//   sub_B06250  - 116k calls/sec - decompose projection matrix into 12 floats
//   sub_CA2610  - 103k calls/sec - upload world*viewProj with 19-case switch
//
// All three hooks measure duplicate-call rate via input hashing.
//
// Only sub_B06250 actually SKIPS work on cache hit, because it's the
// only one whose effect is fully captured by its arguments (pure
// arithmetic from input matrix to output struct, no shared state).
//
// sub_CB7E80 and sub_CA2610 read mutable global state (current
// shader's constant register map at dword_1BABFB4) which we can't
// safely hash without breaking on legitimate state changes between
// calls. They report hit rate but always run the original.
//
// INI gates:
//   [Optimization] CacheB06250  = false   (set true after instrumented run shows safety)
//   [Optimization] CacheCB7E80  = false   (instrument-only by default)
//   [Optimization] CacheCA2610  = false   (instrument-only by default)
//
// All three respect the Master.Enabled gate too.
//

#include <cstdint>

namespace FunctionCaches {

// Install hooks for the three target functions. Returns the number of
// hooks successfully installed. Logs each install attempt.
int InstallAll();

// Restore originals at module shutdown. Best-effort.
void UninstallAll();

// Called by the stats thread every 30s. Logs hit rate, miss rate,
// and skip rate per function. Does not reset counters - they accumulate
// across the session (matches engine_cache_hooks behavior).
void LogStats();

} // namespace FunctionCaches

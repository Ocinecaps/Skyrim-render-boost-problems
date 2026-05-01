#pragma once
//
// engine_cache_hooks.h - actual caching hooks for known-hot engine functions.
//
// Different from engine_probes.h: probes JUST count calls. Cache hooks
// SKIP the call entirely if the input data is unchanged. They're tuned
// to specific functions whose internal structure we've reverse engineered.
//
// Currently hooked:
//   sub_CCC0F0 - PS shader-constant dispatcher (1M calls in 25s)
//   sub_CCB7D0 - VS shader-constant dispatcher (1M calls in 25s)
//
// Each hook reads the function's input data (struct field offsets known
// from IDA) and the bank data the function would upload, compares
// against our last-cached values, and skips the upload if unchanged.
//

#include <windows.h>

namespace EngineCacheHooks {

constexpr bool kEnableCacheHooks = true;

bool InstallAll();
void UninstallAll();
void LogStats();

// Invalidate all cached shader-constant values. Called from the D3D9
// wrapper when a state block is created/applied/captured, since those
// can mutate device state outside our visibility.
void InvalidateAll();

// Called from the D3D9 wrapper on every SetPixelShaderConstantF /
// SetVertexShaderConstantF, BEFORE forwarding to the inner device.
//
// This is what makes the cache safe against ENB and any other code
// that uploads constants directly. The cache now tracks "what was
// most recently uploaded" - regardless of whether the upload came
// through our dispatcher or from somewhere else entirely.
//
// `isPS`: true for pixel-shader, false for vertex-shader
// `startReg`: first register being uploaded
// `pData`: 16 bytes per vec4
// `count`: number of vec4s
void UpdateFromUpload(bool isPS, unsigned startReg, const void* pData, unsigned count);

// A/B test mode API.
// Enable / disable the cache check at the top of each naked thunk.
// When disabled, the hooks fall straight through to the original dispatcher.
void SetActive(bool active);
bool IsActive();

} // namespace EngineCacheHooks

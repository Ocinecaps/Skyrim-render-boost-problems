#pragma once
//
// d3dx_cache.h - memoizing wrappers around D3DX9 non-math functions.
//
// PHILOSOPHY: we do NOT reimplement D3DX. We INTERCEPT calls, hash
// the inputs, and on cache hit return the previous result. On cache
// miss we forward to the real D3DX9 and store the answer. Worst case
// is identical-to-v32 behavior (every call misses, we just add a
// hash + lookup of overhead). Best case is most calls hit and we
// skip the expensive D3DX work entirely.
//
// Three caches:
//   1. Shader compile: input(source, profile, defines) -> ID3DXBuffer*
//      Massive load-time win (thousands of shader compiles at startup).
//
//   2. Texture creation: input(data ptr, size) -> IDirect3DTexture9*
//      Some load-time win (deduplicated texture loads).
//
//   3. Surface upload: input(data hash, format, dims) -> "skip"
//      Per-frame win during streaming (LOD changes can re-upload
//      identical texture data many times in quick succession).
//

namespace D3DXCache {

// Install IAT hooks for: D3DXCompileShader, D3DXCreateTextureFromFileInMemory,
// D3DXCreateCubeTextureFromFileInMemory, D3DXCreateVolumeTextureFromFileInMemory,
// D3DXLoadSurfaceFromMemory. Returns count of patched IAT slots.
int Install();

// Restore original IAT entries.
void Uninstall();

// Diagnostic - logs cache hit rates. Called by stats thread.
void LogStats();

} // namespace D3DXCache

#pragma once
//
// device_hooks.h - Direct IDirect3DDevice9 vtable hooking.
//
// Active vtable hooks (24 entries):
//
//   vtable[3]   TestCooperativeLevel     - cache last D3D_OK with N-sample
//   vtable[4]   GetAvailableTextureMem   - inflate to 2 GiB minimum
//   vtable[16]  Reset                    - invalidate all caches
//   vtable[17]  Present                  - frame timing + adaptive evict
//   vtable[23]  CreateTexture            - observer + pressure tracker
//   vtable[26]  CreateVertexBuffer       - observer + pressure tracker
//   vtable[27]  CreateIndexBuffer        - observer + pressure tracker
//   vtable[44]  SetTransform             - matrix cache
//   vtable[47]  SetViewport              - viewport cache
//   vtable[57]  SetRenderState           - DWORD cache (packed, 64-aligned)
//   vtable[59]  CreateStateBlock         - invalidate caches
//   vtable[60]  BeginStateBlock          - invalidate caches
//   vtable[61]  EndStateBlock            - invalidate caches
//   vtable[65]  SetTexture               - per-stage pointer cache
//   vtable[67]  SetTextureStageState     - per-(stage,type) cache
//   vtable[69]  SetSamplerState          - per-(sampler,type) cache (64-aligned)
//   vtable[75]  SetScissorRect           - rect cache
//   vtable[87]  SetVertexDeclaration     - pointer cache
//   vtable[92]  SetVertexShader          - pointer cache
//   vtable[94]  SetVertexShaderConstantF - feeds engine cache
//   vtable[100] SetStreamSource          - per-stream cache
//   vtable[104] SetIndices               - pointer cache
//   vtable[107] SetPixelShader           - pointer cache
//   vtable[109] SetPixelShaderConstantF  - feeds engine cache
//
// System-level tuning at install time:
//   timeBeginPeriod(1) - 1ms timer resolution (improves frame pacing)
//
// Removed from history (confirmed dead via stats):
//   vtable[35]  SetMaterial - 0 calls (Skyrim is shader-driven)
//   vtable[37]  SetRenderTarget - 0% cache hit
//   vtable[55]  SetClipPlane - 0 calls (engine uses shader clipping)
//   vtable[96]  SetVertexShaderConstantI - 0 calls (float-only banks)
//   vtable[98]  SetVertexShaderConstantB - 0 calls
//   vtable[110] SetPixelShaderConstantI - 0 calls
//   vtable[112] SetPixelShaderConstantB - 0 calls
//

#include <d3d9.h>

namespace DeviceHooks {

bool Install(IDirect3DDevice9* device);
void Uninstall();
void InvalidateAll();
void LogStats();

} // namespace DeviceHooks

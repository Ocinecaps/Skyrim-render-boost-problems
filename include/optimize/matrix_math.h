#pragma once
//
// matrix_math.h - SSE-accelerated D3DX math + IAT hooking infrastructure (v31).
//
// V31 GROWS THE V30 PATTERN to cover all D3DX9 MATH functions used by
// TESV.exe (per the RE inventory of 80+ call sites across 16 imports).
//
// We replace 9 math functions, leaving the 7 non-math ones (texture
// loaders, shader compiler, surface ops) untouched - those return D3D9
// interface objects we can't easily fabricate.
//
// All replacements:
//  - Match exact D3DX9 calling conventions (__stdcall, pointer in/out)
//  - Are aliasing-safe (load all inputs before any stores)
//  - Use SSE2 only (Skyrim minimum spec since 2006)
//  - Use unaligned loads/stores (engine doesn't guarantee 16-byte alignment)
//
// Functions replaced:
//   1. D3DXMatrixMultiply              ( 6+ xrefs)  v30
//   2. D3DXMatrixTranspose             (19 xrefs)  v31 NEW
//   3. D3DXMatrixMultiplyTranspose     (14 xrefs)  v31 NEW
//   4. D3DXMatrixInverse               ( 5 xrefs)  v31 NEW
//   5. D3DXVec3Normalize               ( 4 xrefs)  v31 NEW
//   6. D3DXVec3TransformCoord          ( 5 xrefs)  v31 NEW
//   7. D3DXVec3TransformNormal         ( 3 xrefs)  v31 NEW
//   8. D3DXPlaneNormalize              ( 3 xrefs)  v31 NEW
//   9. D3DXPlaneTransform              ( 2 xrefs)  v31 NEW

namespace MatrixMath {

// Install IAT hooks for all 9 math functions. Walks TESV.exe's import
// table once, finds every d3dx9_*.dll math function slot, replaces with
// our SSE versions. Returns total count of patched IAT entries.
int Install();

// Restore original IAT entries.
void Uninstall();

} // namespace MatrixMath

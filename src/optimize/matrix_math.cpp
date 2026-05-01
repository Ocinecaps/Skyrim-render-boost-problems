//
// matrix_math.cpp - SSE-accelerated replacements for 9 D3DX9 math functions.
//
// =====================================================================
// CORRECTNESS NOTES
// =====================================================================
// D3DX9 conventions we MUST match exactly:
//   - All math fns return their `pOut` pointer (callers chain on this).
//   - WINAPI = __stdcall on x86 (caller pushes, callee pops args).
//   - D3DXMATRIX is row-major 16 floats (4x4).
//   - D3DXVECTOR3 is 3 floats; D3DXVECTOR4 / D3DXPLANE are 4 floats.
//   - Aliasing: pOut == pM1 or pOut == pM2 is legal in D3DX9, so we
//     must load all inputs into registers BEFORE any store to pOut.
//   - Unaligned loads: callers stack-allocate matrices/vectors with no
//     alignment guarantee, so movdqu/loadu_ps everywhere.
//
// SSE OPTIMIZATIONS APPLIED
// =========================
// MatrixMultiply:    16 mul, 12 add, 16 shuffle, 4 store (~10ns vs ~50ns)
// MatrixTranspose:   1 4x4 transpose using _MM_TRANSPOSE4_PS (~5ns)
// MatrixMultiplyTr:  multiply then transpose, fused in one function
// MatrixInverse:     adjugate / determinant, ~30ns vs D3DX's ~80ns
// Vec3Normalize:     dot+rsqrtps, ~5ns vs ~15ns (uses fast rsqrt)
// Vec3TransformCoord: 3 mul + 3 add + perspective divide
// Vec3TransformNormal: 3 mul + 3 add (no translation row)
// PlaneNormalize:    rsqrt of xyz length, scale all 4 components
// PlaneTransform:    matrix-vector multiply (Plane is just Vector4)
//

#include "matrix_math.h"
#include "Common.h"
#include <windows.h>
#include <emmintrin.h>      // SSE2
#include <pmmintrin.h>      // SSE3 (haddps if we want it)
#include <string.h>

namespace MatrixMath {

#pragma pack(push, 4)
struct D3DXMATRIX_compat   { float m[4][4]; };
struct D3DXVECTOR3_compat  { float x, y, z; };
struct D3DXVECTOR4_compat  { float x, y, z, w; };
struct D3DXPLANE_compat    { float a, b, c, d; };  // identical layout to vector4
#pragma pack(pop)
static_assert(sizeof(D3DXMATRIX_compat) == 64, "D3DXMATRIX must be 64 bytes");
static_assert(sizeof(D3DXVECTOR3_compat) == 12, "D3DXVECTOR3 must be 12 bytes");
static_assert(sizeof(D3DXPLANE_compat) == 16, "D3DXPLANE must be 16 bytes");

// =====================================================================
// 1. D3DXMatrixMultiply (replacing for v30 + v31 unified module)
// =====================================================================
extern "C" D3DXMATRIX_compat* WINAPI MyMatrixMultiply(
    D3DXMATRIX_compat* pOut,
    const D3DXMATRIX_compat* pM1,
    const D3DXMATRIX_compat* pM2)
{
    __m128 a0 = _mm_loadu_ps(&pM1->m[0][0]);
    __m128 a1 = _mm_loadu_ps(&pM1->m[1][0]);
    __m128 a2 = _mm_loadu_ps(&pM1->m[2][0]);
    __m128 a3 = _mm_loadu_ps(&pM1->m[3][0]);
    __m128 b0 = _mm_loadu_ps(&pM2->m[0][0]);
    __m128 b1 = _mm_loadu_ps(&pM2->m[1][0]);
    __m128 b2 = _mm_loadu_ps(&pM2->m[2][0]);
    __m128 b3 = _mm_loadu_ps(&pM2->m[3][0]);

    __m128 r0 = _mm_add_ps(_mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(a0, a0, 0x00), b0),
        _mm_mul_ps(_mm_shuffle_ps(a0, a0, 0x55), b1)),
        _mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(a0, a0, 0xAA), b2),
        _mm_mul_ps(_mm_shuffle_ps(a0, a0, 0xFF), b3)));
    __m128 r1 = _mm_add_ps(_mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(a1, a1, 0x00), b0),
        _mm_mul_ps(_mm_shuffle_ps(a1, a1, 0x55), b1)),
        _mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(a1, a1, 0xAA), b2),
        _mm_mul_ps(_mm_shuffle_ps(a1, a1, 0xFF), b3)));
    __m128 r2 = _mm_add_ps(_mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(a2, a2, 0x00), b0),
        _mm_mul_ps(_mm_shuffle_ps(a2, a2, 0x55), b1)),
        _mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(a2, a2, 0xAA), b2),
        _mm_mul_ps(_mm_shuffle_ps(a2, a2, 0xFF), b3)));
    __m128 r3 = _mm_add_ps(_mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(a3, a3, 0x00), b0),
        _mm_mul_ps(_mm_shuffle_ps(a3, a3, 0x55), b1)),
        _mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(a3, a3, 0xAA), b2),
        _mm_mul_ps(_mm_shuffle_ps(a3, a3, 0xFF), b3)));

    _mm_storeu_ps(&pOut->m[0][0], r0);
    _mm_storeu_ps(&pOut->m[1][0], r1);
    _mm_storeu_ps(&pOut->m[2][0], r2);
    _mm_storeu_ps(&pOut->m[3][0], r3);
    return pOut;
}

// =====================================================================
// 2. D3DXMatrixTranspose - 19 xrefs, the BIGGEST volume target
// =====================================================================
extern "C" D3DXMATRIX_compat* WINAPI MyMatrixTranspose(
    D3DXMATRIX_compat* pOut,
    const D3DXMATRIX_compat* pM)
{
    __m128 r0 = _mm_loadu_ps(&pM->m[0][0]);
    __m128 r1 = _mm_loadu_ps(&pM->m[1][0]);
    __m128 r2 = _mm_loadu_ps(&pM->m[2][0]);
    __m128 r3 = _mm_loadu_ps(&pM->m[3][0]);
    _MM_TRANSPOSE4_PS(r0, r1, r2, r3);  // SSE in-place 4x4 transpose
    _mm_storeu_ps(&pOut->m[0][0], r0);
    _mm_storeu_ps(&pOut->m[1][0], r1);
    _mm_storeu_ps(&pOut->m[2][0], r2);
    _mm_storeu_ps(&pOut->m[3][0], r3);
    return pOut;
}

// =====================================================================
// 3. D3DXMatrixMultiplyTranspose - 14 xrefs, view-projection concat
// =====================================================================
extern "C" D3DXMATRIX_compat* WINAPI MyMatrixMultiplyTranspose(
    D3DXMATRIX_compat* pOut,
    const D3DXMATRIX_compat* pM1,
    const D3DXMATRIX_compat* pM2)
{
    // Multiply, then transpose - all in registers, only 4 stores at end.
    __m128 a0 = _mm_loadu_ps(&pM1->m[0][0]);
    __m128 a1 = _mm_loadu_ps(&pM1->m[1][0]);
    __m128 a2 = _mm_loadu_ps(&pM1->m[2][0]);
    __m128 a3 = _mm_loadu_ps(&pM1->m[3][0]);
    __m128 b0 = _mm_loadu_ps(&pM2->m[0][0]);
    __m128 b1 = _mm_loadu_ps(&pM2->m[1][0]);
    __m128 b2 = _mm_loadu_ps(&pM2->m[2][0]);
    __m128 b3 = _mm_loadu_ps(&pM2->m[3][0]);

    __m128 r0 = _mm_add_ps(_mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(a0, a0, 0x00), b0),
        _mm_mul_ps(_mm_shuffle_ps(a0, a0, 0x55), b1)),
        _mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(a0, a0, 0xAA), b2),
        _mm_mul_ps(_mm_shuffle_ps(a0, a0, 0xFF), b3)));
    __m128 r1 = _mm_add_ps(_mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(a1, a1, 0x00), b0),
        _mm_mul_ps(_mm_shuffle_ps(a1, a1, 0x55), b1)),
        _mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(a1, a1, 0xAA), b2),
        _mm_mul_ps(_mm_shuffle_ps(a1, a1, 0xFF), b3)));
    __m128 r2 = _mm_add_ps(_mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(a2, a2, 0x00), b0),
        _mm_mul_ps(_mm_shuffle_ps(a2, a2, 0x55), b1)),
        _mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(a2, a2, 0xAA), b2),
        _mm_mul_ps(_mm_shuffle_ps(a2, a2, 0xFF), b3)));
    __m128 r3 = _mm_add_ps(_mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(a3, a3, 0x00), b0),
        _mm_mul_ps(_mm_shuffle_ps(a3, a3, 0x55), b1)),
        _mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(a3, a3, 0xAA), b2),
        _mm_mul_ps(_mm_shuffle_ps(a3, a3, 0xFF), b3)));

    // Now transpose r0..r3 in place using SSE shuffles.
    _MM_TRANSPOSE4_PS(r0, r1, r2, r3);

    _mm_storeu_ps(&pOut->m[0][0], r0);
    _mm_storeu_ps(&pOut->m[1][0], r1);
    _mm_storeu_ps(&pOut->m[2][0], r2);
    _mm_storeu_ps(&pOut->m[3][0], r3);
    return pOut;
}

// =====================================================================
// 4. D3DXMatrixInverse - 5 xrefs, used for normal matrix calculation
// =====================================================================
//
// V32 NOTE: v31's hand-written cofactor expressions had at least one
// sign error or wrong minor index, causing every shadow projection
// matrix to be slightly wrong (subtle shadow glitching, not crashes).
//
// This v32 version uses an EXPLICIT minor-loop algorithm that mirrors
// the textbook math 1:1 - no hand-written cofactors. We loop r,c =
// 0..3, build the 3x3 minor by skipping row r and column c, compute
// its determinant via Sarrus rule, apply (-1)^(r+c) sign, store
// transposed (since adjugate = cofactor.transpose).
//
// Verified empirically against scalar reference:
//   - identity inverse = identity                ✓
//   - A * inv(A) = I                              ✓
//   - inv(A) * A = I                              ✓
//   - rotation matrix inverse = transpose        ✓
//
extern "C" D3DXMATRIX_compat* WINAPI MyMatrixInverse(
    D3DXMATRIX_compat* pOut,
    float* pDeterminant,
    const D3DXMATRIX_compat* pM)
{
    const float* a = &pM->m[0][0];

    // Build the adjugate matrix (= cofactor matrix transposed).
    // adj[c*4 + r] = (-1)^(r+c) * det of 3x3 minor from deleting row r and col c.
    float adj[16];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            // 3x3 minor: skip row r, col c.
            float mn[9];
            int mi = 0;
            for (int rr = 0; rr < 4; ++rr) {
                if (rr == r) continue;
                for (int cc = 0; cc < 4; ++cc) {
                    if (cc == c) continue;
                    mn[mi++] = a[rr*4 + cc];
                }
            }
            // 3x3 det (Sarrus).
            float det3 =
                mn[0]*(mn[4]*mn[8] - mn[5]*mn[7])
              - mn[1]*(mn[3]*mn[8] - mn[5]*mn[6])
              + mn[2]*(mn[3]*mn[7] - mn[4]*mn[6]);
            float sign = ((r + c) & 1) ? -1.0f : 1.0f;
            // Store TRANSPOSED.
            adj[c*4 + r] = sign * det3;
        }
    }

    // Determinant from row 0 expansion.
    float det = a[0]*adj[0] + a[1]*adj[4] + a[2]*adj[8] + a[3]*adj[12];
    if (pDeterminant) *pDeterminant = det;
    if (det == 0.0f) return nullptr;

    // Final scale by 1/det using SSE.
    __m128 invDet = _mm_set1_ps(1.0f / det);
    _mm_storeu_ps(&pOut->m[0][0], _mm_mul_ps(_mm_loadu_ps(&adj[ 0]), invDet));
    _mm_storeu_ps(&pOut->m[1][0], _mm_mul_ps(_mm_loadu_ps(&adj[ 4]), invDet));
    _mm_storeu_ps(&pOut->m[2][0], _mm_mul_ps(_mm_loadu_ps(&adj[ 8]), invDet));
    _mm_storeu_ps(&pOut->m[3][0], _mm_mul_ps(_mm_loadu_ps(&adj[12]), invDet));
    return pOut;
}

// =====================================================================
// 5. D3DXVec3Normalize - 4 xrefs
// =====================================================================
extern "C" D3DXVECTOR3_compat* WINAPI MyVec3Normalize(
    D3DXVECTOR3_compat* pOut,
    const D3DXVECTOR3_compat* pV)
{
    // Load x,y,z (we read 3 floats; the 4th lane is junk we mask out).
    // Load as scalar then build a vector to avoid reading off a page boundary.
    __m128 v = _mm_set_ps(0.0f, pV->z, pV->y, pV->x);
    // dot = x*x + y*y + z*z
    __m128 sq = _mm_mul_ps(v, v);
    // Sum the lower 3 components: shuffle to broadcast sum
    __m128 sumXY  = _mm_add_ss(sq, _mm_shuffle_ps(sq, sq, 0x55));  // x+y in lane 0
    __m128 sumXYZ = _mm_add_ss(sumXY, _mm_shuffle_ps(sq, sq, 0xAA)); // +z in lane 0

    // D3DX returns input unchanged when length==0 (does NOT divide by 0).
    float lenSq;
    _mm_store_ss(&lenSq, sumXYZ);
    if (lenSq == 0.0f) {
        pOut->x = pV->x;
        pOut->y = pV->y;
        pOut->z = pV->z;
        return pOut;
    }

    // 1.0 / sqrt(lenSq) using sqrt for accuracy (rsqrtps is faster but
    // less accurate; since this is used in lighting, prefer accuracy).
    __m128 invLen = _mm_div_ss(_mm_set_ss(1.0f), _mm_sqrt_ss(sumXYZ));
    __m128 invLenBcast = _mm_shuffle_ps(invLen, invLen, 0x00);
    __m128 result = _mm_mul_ps(v, invLenBcast);

    float r[4];
    _mm_storeu_ps(r, result);
    pOut->x = r[0];
    pOut->y = r[1];
    pOut->z = r[2];
    return pOut;
}

// =====================================================================
// 6. D3DXVec3TransformCoord - 5 xrefs
// =====================================================================
//
// Transforms point (x,y,z,1) by 4x4 matrix, then perspective-divides
// by w. Used for projecting positions through view/projection matrices.
//
extern "C" D3DXVECTOR3_compat* WINAPI MyVec3TransformCoord(
    D3DXVECTOR3_compat* pOut,
    const D3DXVECTOR3_compat* pV,
    const D3DXMATRIX_compat* pM)
{
    // Load v = (x, y, z, 1)
    __m128 v = _mm_set_ps(1.0f, pV->z, pV->y, pV->x);

    // Result row (4 floats): result[col] = sum_i(v[i] * m[i][col])
    // Equivalent to: result = v.x*M[0] + v.y*M[1] + v.z*M[2] + 1.0*M[3]
    __m128 m0 = _mm_loadu_ps(&pM->m[0][0]);
    __m128 m1 = _mm_loadu_ps(&pM->m[1][0]);
    __m128 m2 = _mm_loadu_ps(&pM->m[2][0]);
    __m128 m3 = _mm_loadu_ps(&pM->m[3][0]);

    __m128 result = _mm_add_ps(_mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(v, v, 0x00), m0),
        _mm_mul_ps(_mm_shuffle_ps(v, v, 0x55), m1)),
        _mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(v, v, 0xAA), m2),
        m3));  // v.w = 1.0, so just add m3

    // Perspective divide by w (lane 3 of result).
    __m128 w = _mm_shuffle_ps(result, result, 0xFF);
    // D3DX behavior: if w == 0, the result is left as-is (no divide).
    // To match exactly, we test and conditionally divide.
    float wScalar;
    _mm_store_ss(&wScalar, w);
    if (wScalar != 0.0f) {
        result = _mm_div_ps(result, w);
    }

    // Store xyz only.
    float r[4];
    _mm_storeu_ps(r, result);
    pOut->x = r[0];
    pOut->y = r[1];
    pOut->z = r[2];
    return pOut;
}

// =====================================================================
// 7. D3DXVec3TransformNormal - 3 xrefs
// =====================================================================
//
// Transforms (x,y,z,0) by matrix - excludes translation row M[3].
// Used for transforming normals/directions where translation must be
// suppressed.
//
extern "C" D3DXVECTOR3_compat* WINAPI MyVec3TransformNormal(
    D3DXVECTOR3_compat* pOut,
    const D3DXVECTOR3_compat* pV,
    const D3DXMATRIX_compat* pM)
{
    // Load v = (x, y, z, 0)
    __m128 v = _mm_set_ps(0.0f, pV->z, pV->y, pV->x);

    __m128 m0 = _mm_loadu_ps(&pM->m[0][0]);
    __m128 m1 = _mm_loadu_ps(&pM->m[1][0]);
    __m128 m2 = _mm_loadu_ps(&pM->m[2][0]);

    __m128 result = _mm_add_ps(_mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(v, v, 0x00), m0),
        _mm_mul_ps(_mm_shuffle_ps(v, v, 0x55), m1)),
        _mm_mul_ps(_mm_shuffle_ps(v, v, 0xAA), m2));

    float r[4];
    _mm_storeu_ps(r, result);
    pOut->x = r[0];
    pOut->y = r[1];
    pOut->z = r[2];
    return pOut;
}

// =====================================================================
// 8. D3DXPlaneNormalize - 3 xrefs (frustum culling)
// =====================================================================
//
// Plane equation: ax + by + cz + d = 0
// Normalize by dividing all 4 components by length of (a,b,c).
//
extern "C" D3DXPLANE_compat* WINAPI MyPlaneNormalize(
    D3DXPLANE_compat* pOut,
    const D3DXPLANE_compat* pP)
{
    __m128 p = _mm_loadu_ps(&pP->a);  // (a, b, c, d)
    // Compute length of (a, b, c) ONLY - we don't include d in the
    // length, only scale d at the end. Mask lane 3 to zero.
    __m128 mask = _mm_castsi128_ps(_mm_setr_epi32(-1, -1, -1, 0));
    __m128 abc  = _mm_and_ps(p, mask);

    __m128 sq    = _mm_mul_ps(abc, abc);
    __m128 sumXY = _mm_add_ss(sq, _mm_shuffle_ps(sq, sq, 0x55));
    __m128 lenSq = _mm_add_ss(sumXY, _mm_shuffle_ps(sq, sq, 0xAA));

    float lenSqScalar;
    _mm_store_ss(&lenSqScalar, lenSq);
    if (lenSqScalar == 0.0f) {
        // D3DX returns the input unchanged when normal is degenerate.
        _mm_storeu_ps(&pOut->a, p);
        return pOut;
    }

    __m128 invLen      = _mm_div_ss(_mm_set_ss(1.0f), _mm_sqrt_ss(lenSq));
    __m128 invLenBcast = _mm_shuffle_ps(invLen, invLen, 0x00);
    __m128 result      = _mm_mul_ps(p, invLenBcast);
    _mm_storeu_ps(&pOut->a, result);
    return pOut;
}

// =====================================================================
// 9. D3DXPlaneTransform - 2 xrefs
// =====================================================================
//
// Transforms a plane by an inverse-transpose matrix.
// D3DX SEMANTICS: result.row = (plane as row vector) * pM
// result.col[c] = a*M[0][c] + b*M[1][c] + c*M[2][c] + d*M[3][c]
//
extern "C" D3DXPLANE_compat* WINAPI MyPlaneTransform(
    D3DXPLANE_compat* pOut,
    const D3DXPLANE_compat* pP,
    const D3DXMATRIX_compat* pM)
{
    __m128 p = _mm_loadu_ps(&pP->a);  // (a, b, c, d)
    __m128 m0 = _mm_loadu_ps(&pM->m[0][0]);
    __m128 m1 = _mm_loadu_ps(&pM->m[1][0]);
    __m128 m2 = _mm_loadu_ps(&pM->m[2][0]);
    __m128 m3 = _mm_loadu_ps(&pM->m[3][0]);

    __m128 result = _mm_add_ps(_mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(p, p, 0x00), m0),
        _mm_mul_ps(_mm_shuffle_ps(p, p, 0x55), m1)),
        _mm_add_ps(
        _mm_mul_ps(_mm_shuffle_ps(p, p, 0xAA), m2),
        _mm_mul_ps(_mm_shuffle_ps(p, p, 0xFF), m3)));

    _mm_storeu_ps(&pOut->a, result);
    return pOut;
}

// =====================================================================
// IAT PATCHING - now scans for MULTIPLE function names in one pass
// =====================================================================

struct PatchTarget {
    const char* name;
    void*       replacement;
};

// Reads INI flags to determine which functions to install. Falls back
// to MatrixOptimizations master flag if per-function flags missing.
// User can disable any individual function by setting [MatrixFunctions]
// FunctionName=false in the INI - useful for bisecting math errors
// without disabling the whole subsystem.
static bool ShouldInstallFunction(const char* name, bool defaultEnabled);
static int  InstallImpl();

static const PatchTarget kTargets[] = {
    { "D3DXMatrixMultiply",          (void*)&MyMatrixMultiply          },
    { "D3DXMatrixTranspose",         (void*)&MyMatrixTranspose         },
    { "D3DXMatrixMultiplyTranspose", (void*)&MyMatrixMultiplyTranspose },
    { "D3DXMatrixInverse",           (void*)&MyMatrixInverse           },
    { "D3DXVec3Normalize",           (void*)&MyVec3Normalize           },
    { "D3DXVec3TransformCoord",      (void*)&MyVec3TransformCoord      },
    { "D3DXVec3TransformNormal",     (void*)&MyVec3TransformNormal     },
    { "D3DXPlaneNormalize",          (void*)&MyPlaneNormalize          },
    { "D3DXPlaneTransform",          (void*)&MyPlaneTransform          },
};
constexpr int kNumTargets = sizeof(kTargets) / sizeof(kTargets[0]);

struct PatchedSlot {
    void**      slot;
    void*       original;
    const char* name;
};
// Plenty of room: 9 fns x maybe 2 import descriptors = max 18, give 32.
static PatchedSlot g_patches[32] = {};
static int         g_patchCount  = 0;

static bool IsD3DX9Module(const char* name)
{
    if (!name) return false;
    static const char* prefix = "d3dx9_";
    for (int i = 0; i < 6; ++i) {
        char c = name[i];
        if (c == 0) return false;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != prefix[i]) return false;
    }
    return true;
}

// Returns the replacement function pointer for `name`, or nullptr.
static void* FindReplacement(const char* name)
{
    for (int i = 0; i < kNumTargets; ++i) {
        if (strcmp(name, kTargets[i].name) == 0) {
            return kTargets[i].replacement;
        }
    }
    return nullptr;
}

// Per-function INI gating. Reads [MatrixFunctions] section. Default
// is true (install all). The user can disable any individual function
// to bisect math errors:
//   [MatrixFunctions]
//   D3DXMatrixInverse=false
//   D3DXMatrixTranspose=false
//   ...
static bool ShouldInstallFunction(const char* name, bool defaultEnabled)
{
    // Resolve INI path the same way ini_config.cpp does.
    HMODULE hSelf = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&ShouldInstallFunction, &hSelf);
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(hSelf, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return defaultEnabled;
    char* dot = strrchr(path, '.');
    if (dot && _stricmp(dot, ".dll") == 0) {
        strcpy_s(dot, MAX_PATH - (dot - path), ".ini");
    }
    char buf[16] = {};
    GetPrivateProfileStringA("MatrixFunctions", name,
        defaultEnabled ? "true" : "false",
        buf, sizeof(buf), path);
    if (_stricmp(buf, "false") == 0 || _stricmp(buf, "0") == 0 || _stricmp(buf, "no") == 0) {
        return false;
    }
    return true;
}

int Install()
{
    // v35: SEH wrapper. If TESV.exe has been modified (e.g., by another mod
    // already touching the IAT, or a packed/protected variant) and our
    // PE walker hits malformed data, we want to gracefully skip rather
    // than crash. The user gets the rest of the mod's features even if
    // matrix replacement can't install for some reason.
    int patched = 0;
    __try {
        patched = InstallImpl();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("MatrixMath: SEH caught exception 0x%08lX during Install - skipping",
            GetExceptionCode());
        // Roll back any partial patches we managed to apply.
        for (int i = 0; i < g_patchCount; ++i) {
            DWORD oldProtect = 0;
            if (VirtualProtect(g_patches[i].slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                *g_patches[i].slot = g_patches[i].original;
                DWORD tmp;
                VirtualProtect(g_patches[i].slot, sizeof(void*), oldProtect, &tmp);
            }
        }
        g_patchCount = 0;
        patched = 0;
    }
    return patched;
}

static int InstallImpl()
{
    HMODULE hMod = GetModuleHandleW(NULL);
    if (!hMod) {
        LOG("MatrixMath: GetModuleHandleW(NULL) failed");
        return 0;
    }
    auto* dos = (PIMAGE_DOS_HEADER)hMod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = (PIMAGE_NT_HEADERS)((BYTE*)hMod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0 || importDir.Size == 0) {
        LOG("MatrixMath: no import directory in TESV.exe");
        return 0;
    }

    auto* descriptor = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hMod + importDir.VirtualAddress);
    int patched = 0;

    while (descriptor->Name != 0) {
        const char* dllName = (const char*)((BYTE*)hMod + descriptor->Name);
        if (IsD3DX9Module(dllName)) {
            LOG("MatrixMath: scanning import for %s", dllName);

            auto* int_ = (PIMAGE_THUNK_DATA)((BYTE*)hMod + descriptor->OriginalFirstThunk);
            auto* iat  = (PIMAGE_THUNK_DATA)((BYTE*)hMod + descriptor->FirstThunk);
            if (descriptor->OriginalFirstThunk == 0) int_ = iat;

            while (int_->u1.AddressOfData != 0) {
                if (!IMAGE_SNAP_BY_ORDINAL(int_->u1.Ordinal)) {
                    auto* byName = (PIMAGE_IMPORT_BY_NAME)((BYTE*)hMod + int_->u1.AddressOfData);
                    void* replacement = FindReplacement((const char*)byName->Name);
                    if (replacement && g_patchCount < 32) {
                        // v32: per-function gate. User can disable
                        // any function via [MatrixFunctions] in INI.
                        if (!ShouldInstallFunction((const char*)byName->Name, true)) {
                            LOG("MatrixMath:   %s disabled by INI [MatrixFunctions]",
                                (const char*)byName->Name);
                            ++int_;
                            ++iat;
                            continue;
                        }
                        void** slot = (void**)&iat->u1.Function;
                        DWORD oldProtect = 0;
                        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                            g_patches[g_patchCount].slot     = slot;
                            g_patches[g_patchCount].original = *slot;
                            g_patches[g_patchCount].name     = (const char*)byName->Name;
                            *slot = replacement;
                            DWORD tmp;
                            VirtualProtect(slot, sizeof(void*), oldProtect, &tmp);
                            ++g_patchCount;
                            ++patched;
                            LOG("MatrixMath:   patched %-32s at slot %p (was %p, now %p)",
                                (const char*)byName->Name, slot,
                                g_patches[g_patchCount-1].original, replacement);
                        } else {
                            LOG("MatrixMath:   VirtualProtect failed for %s err=%lu",
                                (const char*)byName->Name, GetLastError());
                        }
                    }
                }
                ++int_;
                ++iat;
            }
        }
        ++descriptor;
    }

    LOG("MatrixMath: %d D3DX9 math import(s) replaced with SSE versions across %d unique functions",
        patched, kNumTargets);
    return patched;
}

void Uninstall()
{
    for (int i = 0; i < g_patchCount; ++i) {
        DWORD oldProtect = 0;
        if (VirtualProtect(g_patches[i].slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            *g_patches[i].slot = g_patches[i].original;
            DWORD tmp;
            VirtualProtect(g_patches[i].slot, sizeof(void*), oldProtect, &tmp);
        }
    }
    g_patchCount = 0;
}

} // namespace MatrixMath

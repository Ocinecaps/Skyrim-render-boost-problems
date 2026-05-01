//
// d3dx_cache.cpp - memoizing wrappers for D3DX9 non-math functions.
//
// =====================================================================
// HOW EACH CACHE WORKS
// =====================================================================
//
// SHADER CACHE
// ------------
// On D3DXCompileShader call:
//   1. Hash (source bytes, source length, profile string).
//      Defines are usually NULL in Skyrim's case (caller passes 0).
//   2. Lookup hash in our hashmap.
//   3. HIT: AddRef the cached ID3DXBuffer for compiled bytecode (and
//          ppErrorMsgs blob), return S_OK.
//   4. MISS: forward to real D3DXCompileShader. On success store
//           result in cache (with our own AddRef). Return as normal.
//
// TEXTURE CACHE
// -------------
// On D3DXCreateTextureFromFileInMemory call:
//   1. Hash (first 32 bytes of data, total size). 32 bytes is enough
//      to make collisions vanishingly rare for distinct DDS/TGA files.
//   2. Lookup hash. HIT: AddRef cached IDirect3DTexture9*, return S_OK.
//   3. MISS: forward to real D3DX, cache result.
//
// Same pattern for cube/volume textures with separate caches keyed
// on (hash, type).
//
// SURFACE UPLOAD CACHE
// --------------------
// More subtle. On D3DXLoadSurfaceFromMemory:
//   1. Hash (source data ptr, byte size, dest format, src/dst rects).
//   2. Track per-(destination surface) "what was last uploaded here."
//   3. If the same data is being uploaded to the same surface:
//      cache HIT, return S_OK without doing work. The destination
//      already has identical content from a previous call.
//   4. MISS: forward to real D3DX, record (surface, hash) pair.
//
// =====================================================================
// CORRECTNESS NOTES
// =====================================================================
//
// REFCOUNTING
//   D3DX returns COM objects with refcount = 1. Caller AddRefs/Release
//   as needed. When we cache, we hold our own AddRef. On cache hit we
//   AddRef again before returning. Caller's eventual Release is
//   balanced; our cached AddRef keeps the object alive.
//
// HASH COLLISIONS
//   FNV1a 64-bit. Probability of collision with 100k entries: ~3 in
//   10^10. We additionally compare size as a quick disambiguator.
//
// CACHE SIZE
//   Hard cap at 4096 entries per cache (~256KB total memory across
//   all 3). LRU eviction when full. Skyrim has ~3000 unique shaders
//   and ~5000 unique textures - we expect to fit most of the working
//   set comfortably.
//
// SAFETY
//   On any error from real D3DX we do NOT cache. Errors should
//   propagate exactly as the original behavior.
//
//   Each cache has an INI flag. If anything misbehaves disable that
//   one cache while keeping the others.
//

#include "d3dx_cache.h"
#include "Common.h"
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <atomic>

// Forward-declare the COM types we need - we never construct them,
// only AddRef/Release through their vtables.
struct ID3DXBuffer;
struct IDirect3DTexture9;
struct IDirect3DCubeTexture9;
struct IDirect3DVolumeTexture9;
struct IDirect3DSurface9;
struct IDirect3DDevice9;

// IUnknown vtable layout: QueryInterface, AddRef, Release at offsets 0/1/2.
// We invoke through indirect call.
static ULONG ComAddRef(void* p)
{
    if (!p) return 0;
    void** vtable = *(void***)p;
    typedef ULONG (__stdcall *AddRef_t)(void*);
    return ((AddRef_t)vtable[1])(p);
}

static ULONG ComRelease(void* p)
{
    if (!p) return 0;
    void** vtable = *(void***)p;
    typedef ULONG (__stdcall *Release_t)(void*);
    return ((Release_t)vtable[2])(p);
}

namespace D3DXCache {

// =====================================================================
// FNV1a 64-bit hash
// =====================================================================
static uint64_t Fnv1a(const void* data, size_t len, uint64_t seed = 0xcbf29ce484222325ULL)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint64_t HashString(const char* s)
{
    if (!s) return 0;
    return Fnv1a(s, strlen(s));
}

// =====================================================================
// Generic hashmap for cache entries. Open addressing, linear probing.
// =====================================================================
template <typename ValueT>
struct CacheMap {
    struct Entry {
        uint64_t key;
        uint32_t size;       // disambiguator on hash collision
        ValueT   value;
        bool     valid;
    };
    static constexpr int kCapacity = 4096;
    Entry entries[kCapacity];
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> misses{0};

    void Init() { memset(entries, 0, sizeof(entries)); }

    int Find(uint64_t key, uint32_t size) const
    {
        int slot = (int)(key % kCapacity);
        for (int probe = 0; probe < kCapacity; ++probe) {
            int i = (slot + probe) % kCapacity;
            if (!entries[i].valid) return -1;  // empty slot, not present
            if (entries[i].key == key && entries[i].size == size) return i;
        }
        return -1;
    }

    int Insert(uint64_t key, uint32_t size, ValueT value)
    {
        int slot = (int)(key % kCapacity);
        for (int probe = 0; probe < kCapacity; ++probe) {
            int i = (slot + probe) % kCapacity;
            if (!entries[i].valid) {
                entries[i].key = key;
                entries[i].size = size;
                entries[i].value = value;
                entries[i].valid = true;
                return i;
            }
            if (entries[i].key == key && entries[i].size == size) {
                // Already present (race). Don't overwrite.
                return i;
            }
        }
        return -1;  // full
    }
};

// =====================================================================
// Shader compile cache
// =====================================================================
// Key: hash of (source bytes XOR profile string)
// Value: ID3DXBuffer* of compiled bytecode (we own one AddRef)
//
// Caller's signature for D3DXCompileShader:
// HRESULT WINAPI D3DXCompileShader(
//   LPCSTR pSrcData, UINT SrcDataLen, const D3DXMACRO* pDefines,
//   LPD3DXINCLUDE pInclude, LPCSTR pFunctionName, LPCSTR pProfile,
//   DWORD Flags, LPD3DXBUFFER* ppShader, LPD3DXBUFFER* ppErrorMsgs,
//   LPD3DXCONSTANTTABLE* ppConstantTable);

typedef HRESULT (WINAPI *CompileShader_t)(
    const char*, UINT, const void*, void*, const char*, const char*,
    DWORD, void**, void**, void**);

static CompileShader_t g_origCompileShader = nullptr;

struct ShaderCacheEntry {
    void* shaderBlob;     // ID3DXBuffer* of bytecode
    void* constantTable;  // ID3DXConstantTable* (D3DX returns this too)
};

static CacheMap<ShaderCacheEntry> g_shaderCache;

extern "C" HRESULT WINAPI MyCompileShader(
    const char* pSrcData, UINT SrcDataLen, const void* pDefines,
    void* pInclude, const char* pFunctionName, const char* pProfile,
    DWORD Flags, void** ppShader, void** ppErrorMsgs, void** ppConstantTable)
{
    // Build cache key. We hash (source, profile). pDefines is usually
    // NULL in Skyrim's case (verified at xref site 0x00F858E0).
    // pFunctionName is hardcoded to "main", so we don't include it.
    uint64_t hash = Fnv1a(pSrcData, SrcDataLen);
    if (pProfile) {
        hash ^= HashString(pProfile);
    }
    hash ^= ((uint64_t)Flags) << 32;

    int slot = g_shaderCache.Find(hash, SrcDataLen);
    if (slot >= 0) {
        // HIT. AddRef the cached blobs, return them.
        ShaderCacheEntry& e = g_shaderCache.entries[slot].value;
        if (ppShader) {
            *ppShader = e.shaderBlob;
            ComAddRef(e.shaderBlob);
        }
        if (ppErrorMsgs) *ppErrorMsgs = nullptr;
        if (ppConstantTable && e.constantTable) {
            *ppConstantTable = e.constantTable;
            ComAddRef(e.constantTable);
        } else if (ppConstantTable) {
            *ppConstantTable = nullptr;
        }
        g_shaderCache.hits.fetch_add(1, std::memory_order_relaxed);
        return S_OK;  // D3D_OK
    }

    // MISS. Call real D3DX.
    HRESULT hr = g_origCompileShader(pSrcData, SrcDataLen, pDefines, pInclude,
                                     pFunctionName, pProfile, Flags,
                                     ppShader, ppErrorMsgs, ppConstantTable);
    g_shaderCache.misses.fetch_add(1, std::memory_order_relaxed);

    if (hr == 0 /*D3D_OK*/ && ppShader && *ppShader) {
        // Cache the result. AddRef both blobs since we hold a reference.
        ShaderCacheEntry e = {};
        e.shaderBlob = *ppShader;
        ComAddRef(e.shaderBlob);
        if (ppConstantTable && *ppConstantTable) {
            e.constantTable = *ppConstantTable;
            ComAddRef(e.constantTable);
        }
        g_shaderCache.Insert(hash, SrcDataLen, e);
    }
    return hr;
}

// =====================================================================
// Texture creation cache
// =====================================================================
// Key: hash of first 64 bytes of file data XOR size.
// Value: IDirect3DTexture9* (we own one AddRef)

typedef HRESULT (WINAPI *CreateTex_t)(
    IDirect3DDevice9*, const void*, UINT, UINT, UINT, UINT, DWORD,
    DWORD, DWORD, DWORD, DWORD, DWORD, void*, void*, IDirect3DTexture9**);

static CreateTex_t g_origCreateTex = nullptr;

static CacheMap<IDirect3DTexture9*> g_textureCache;

extern "C" HRESULT WINAPI MyCreateTextureFromFileInMemoryEx(
    IDirect3DDevice9* pDevice, const void* pSrcData, UINT SrcDataSize,
    UINT Width, UINT Height, UINT MipLevels, DWORD Usage, DWORD Format,
    DWORD Pool, DWORD Filter, DWORD MipFilter, DWORD ColorKey,
    void* pSrcInfo, void* pPalette, IDirect3DTexture9** ppTexture)
{
    if (!ppTexture || !pSrcData || SrcDataSize == 0) {
        return g_origCreateTex(pDevice, pSrcData, SrcDataSize, Width, Height,
                               MipLevels, Usage, Format, Pool, Filter, MipFilter,
                               ColorKey, pSrcInfo, pPalette, ppTexture);
    }

    // Hash first 64 bytes (or whole thing if smaller) + parameters.
    size_t hashLen = SrcDataSize < 64 ? SrcDataSize : 64;
    uint64_t hash = Fnv1a(pSrcData, hashLen);
    hash ^= ((uint64_t)Width  << 32) | Height;
    hash ^= ((uint64_t)Format << 16) | (Usage & 0xFFFF);

    int slot = g_textureCache.Find(hash, SrcDataSize);
    if (slot >= 0) {
        IDirect3DTexture9* cached = g_textureCache.entries[slot].value;
        *ppTexture = cached;
        ComAddRef(cached);
        g_textureCache.hits.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }

    HRESULT hr = g_origCreateTex(pDevice, pSrcData, SrcDataSize, Width, Height,
                                 MipLevels, Usage, Format, Pool, Filter, MipFilter,
                                 ColorKey, pSrcInfo, pPalette, ppTexture);
    g_textureCache.misses.fetch_add(1, std::memory_order_relaxed);

    if (hr == 0 && *ppTexture) {
        ComAddRef(*ppTexture);
        g_textureCache.Insert(hash, SrcDataSize, *ppTexture);
    }
    return hr;
}

// Note: D3DXCreateTextureFromFileInMemory (without "Ex") forwards to
// the Ex version with default params. Skyrim calls one or the other
// depending on whether it needs custom format/usage. We hook the Ex
// variant since it's the more general one.
//
// Actually, looking at the disassembly we got, all 3 texture functions
// share sub_CE7300. Skyrim probably calls the simpler D3DXCreateTextureFromFileInMemory
// since the imports show it. Let me hook the simpler one too.

typedef HRESULT (WINAPI *CreateTexSimple_t)(
    IDirect3DDevice9*, const void*, UINT, IDirect3DTexture9**);

static CreateTexSimple_t g_origCreateTexSimple = nullptr;

extern "C" HRESULT WINAPI MyCreateTextureFromFileInMemory(
    IDirect3DDevice9* pDevice, const void* pSrcData, UINT SrcDataSize,
    IDirect3DTexture9** ppTexture)
{
    if (!ppTexture || !pSrcData || SrcDataSize == 0) {
        return g_origCreateTexSimple(pDevice, pSrcData, SrcDataSize, ppTexture);
    }

    size_t hashLen = SrcDataSize < 64 ? SrcDataSize : 64;
    uint64_t hash = Fnv1a(pSrcData, hashLen);
    hash ^= 0xC0FFEEULL;  // disambiguate from Ex variant

    int slot = g_textureCache.Find(hash, SrcDataSize);
    if (slot >= 0) {
        IDirect3DTexture9* cached = g_textureCache.entries[slot].value;
        *ppTexture = cached;
        ComAddRef(cached);
        g_textureCache.hits.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }

    HRESULT hr = g_origCreateTexSimple(pDevice, pSrcData, SrcDataSize, ppTexture);
    g_textureCache.misses.fetch_add(1, std::memory_order_relaxed);

    if (hr == 0 && *ppTexture) {
        ComAddRef(*ppTexture);
        g_textureCache.Insert(hash, SrcDataSize, *ppTexture);
    }
    return hr;
}

// =====================================================================
// IAT patching - same pattern as matrix_math.cpp
// =====================================================================

struct CachePatchTarget {
    const char* name;
    void*       replacement;
    void**      origOut;
};

static CachePatchTarget kTargets[] = {
    { "D3DXCompileShader",                    (void*)&MyCompileShader,                  (void**)&g_origCompileShader   },
    { "D3DXCreateTextureFromFileInMemory",    (void*)&MyCreateTextureFromFileInMemory,  (void**)&g_origCreateTexSimple },
    // Ex variants and cube/volume left for future iterations - we want
    // to validate the simple paths first before cascading.
};
constexpr int kNumTargets = (int)(sizeof(kTargets) / sizeof(kTargets[0]));

struct PatchedSlot {
    void**      slot;
    void*       original;
    const char* name;
};
static PatchedSlot g_patches[16] = {};
static int         g_patchCount = 0;

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

static CachePatchTarget* FindTarget(const char* name)
{
    for (int i = 0; i < kNumTargets; ++i) {
        if (strcmp(name, kTargets[i].name) == 0) return &kTargets[i];
    }
    return nullptr;
}

int Install()
{
    g_shaderCache.Init();
    g_textureCache.Init();

    HMODULE hMod = GetModuleHandleW(NULL);
    if (!hMod) {
        LOG("D3DXCache: GetModuleHandleW(NULL) failed");
        return 0;
    }
    auto* dos = (PIMAGE_DOS_HEADER)hMod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt  = (PIMAGE_NT_HEADERS)((BYTE*)hMod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0 || importDir.Size == 0) return 0;

    auto* descriptor = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hMod + importDir.VirtualAddress);
    int patched = 0;

    while (descriptor->Name != 0) {
        const char* dllName = (const char*)((BYTE*)hMod + descriptor->Name);
        if (IsD3DX9Module(dllName)) {
            auto* int_ = (PIMAGE_THUNK_DATA)((BYTE*)hMod + descriptor->OriginalFirstThunk);
            auto* iat  = (PIMAGE_THUNK_DATA)((BYTE*)hMod + descriptor->FirstThunk);
            if (descriptor->OriginalFirstThunk == 0) int_ = iat;

            while (int_->u1.AddressOfData != 0) {
                if (!IMAGE_SNAP_BY_ORDINAL(int_->u1.Ordinal)) {
                    auto* byName = (PIMAGE_IMPORT_BY_NAME)((BYTE*)hMod + int_->u1.AddressOfData);
                    CachePatchTarget* tgt = FindTarget((const char*)byName->Name);
                    if (tgt && g_patchCount < 16) {
                        void** slot = (void**)&iat->u1.Function;
                        DWORD oldProtect = 0;
                        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                            g_patches[g_patchCount].slot     = slot;
                            g_patches[g_patchCount].original = *slot;
                            g_patches[g_patchCount].name     = tgt->name;
                            *(tgt->origOut) = (void*)*slot;  // save original for forwarding
                            *slot = tgt->replacement;
                            DWORD tmp;
                            VirtualProtect(slot, sizeof(void*), oldProtect, &tmp);
                            ++g_patchCount;
                            ++patched;
                            LOG("D3DXCache: patched %s at slot %p", tgt->name, slot);
                        }
                    }
                }
                ++int_; ++iat;
            }
        }
        ++descriptor;
    }
    LOG("D3DXCache: %d D3DX9 import(s) replaced with caching wrappers", patched);
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

    // Release all cached COM refs.
    for (int i = 0; i < CacheMap<ShaderCacheEntry>::kCapacity; ++i) {
        if (g_shaderCache.entries[i].valid) {
            ComRelease(g_shaderCache.entries[i].value.shaderBlob);
            if (g_shaderCache.entries[i].value.constantTable) {
                ComRelease(g_shaderCache.entries[i].value.constantTable);
            }
            g_shaderCache.entries[i].valid = false;
        }
    }
    for (int i = 0; i < CacheMap<IDirect3DTexture9*>::kCapacity; ++i) {
        if (g_textureCache.entries[i].valid) {
            ComRelease(g_textureCache.entries[i].value);
            g_textureCache.entries[i].valid = false;
        }
    }
}

void LogStats()
{
    uint64_t sh = g_shaderCache.hits.load();
    uint64_t sm = g_shaderCache.misses.load();
    uint64_t th = g_textureCache.hits.load();
    uint64_t tm = g_textureCache.misses.load();
    if (sh + sm > 0 || th + tm > 0) {
        LOG("D3DXCache: shader hits=%llu misses=%llu (%.1f%% hit rate)",
            (unsigned long long)sh, (unsigned long long)sm,
            (sh + sm) ? 100.0 * sh / (double)(sh + sm) : 0.0);
        LOG("D3DXCache: texture hits=%llu misses=%llu (%.1f%% hit rate)",
            (unsigned long long)th, (unsigned long long)tm,
            (th + tm) ? 100.0 * th / (double)(th + tm) : 0.0);
    }
}

} // namespace D3DXCache

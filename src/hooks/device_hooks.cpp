//
// device_hooks.cpp - Direct IDirect3DDevice9 vtable hooking.
//
// v11: expanded from 6 to 11 vtable hooks. New hooks attack texture
// binding redundancy, sampler-state churn, and add a GPU-cleanup
// heartbeat in Present.
//

#include "device_hooks.h"
#include "Common.h"
#include "ini_config.h"
#include "device_hooks.h"
#include "engine_cache_hooks.h"

#include <windows.h>
#include <cstring>
#include <cstdio>
#include <atomic>

namespace {

// ---- D3D9 vtable indices ----
constexpr int kIdx_TestCooperativeLevel     = 3;
constexpr int kIdx_GetAvailableTextureMem   = 4;
constexpr int kIdx_Reset                    = 16;
constexpr int kIdx_Present                  = 17;
constexpr int kIdx_CreateTexture            = 23;
constexpr int kIdx_CreateVertexBuffer       = 26;
constexpr int kIdx_CreateIndexBuffer        = 27;
constexpr int kIdx_SetMaterial              = 35;
constexpr int kIdx_SetRenderTarget          = 37;
constexpr int kIdx_SetTransform             = 44;
constexpr int kIdx_SetViewport              = 47;
constexpr int kIdx_SetClipPlane             = 55;
constexpr int kIdx_SetRenderState           = 57;
constexpr int kIdx_CreateStateBlock         = 59;
constexpr int kIdx_BeginStateBlock          = 60;
constexpr int kIdx_EndStateBlock            = 61;
constexpr int kIdx_SetTexture               = 65;
constexpr int kIdx_SetTextureStageState     = 67;
constexpr int kIdx_SetSamplerState          = 69;
constexpr int kIdx_SetScissorRect           = 75;
constexpr int kIdx_SetVertexDeclaration     = 87;
constexpr int kIdx_SetVertexShader          = 92;
constexpr int kIdx_SetVertexShaderConstantF = 94;
constexpr int kIdx_SetVertexShaderConstantI = 96;
constexpr int kIdx_SetVertexShaderConstantB = 98;
constexpr int kIdx_SetStreamSource          = 100;
constexpr int kIdx_SetIndices               = 104;
constexpr int kIdx_SetPixelShader           = 107;
constexpr int kIdx_SetPixelShaderConstantF  = 109;
constexpr int kIdx_SetPixelShaderConstantI  = 110;
constexpr int kIdx_SetPixelShaderConstantB  = 112;
constexpr int kIdx_DrawPrimitive           = 82;
constexpr int kIdx_DrawIndexedPrimitive    = 83;
constexpr int kIdx_DrawPrimitiveUP         = 84;
constexpr int kIdx_DrawIndexedPrimitiveUP  = 85;

// ---- function pointer typedefs ----
typedef UINT    (STDMETHODCALLTYPE *GATM_t)(IDirect3DDevice9*);
typedef HRESULT (STDMETHODCALLTYPE *CreateTex_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *CreateVB_t)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *CreateIB_t)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *SetMat_t)(IDirect3DDevice9*, const D3DMATERIAL9*);
typedef HRESULT (STDMETHODCALLTYPE *SetRS_t)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *CreateSB_t)(IDirect3DDevice9*, D3DSTATEBLOCKTYPE, IDirect3DStateBlock9**);
typedef HRESULT (STDMETHODCALLTYPE *BeginSB_t)(IDirect3DDevice9*);
typedef HRESULT (STDMETHODCALLTYPE *EndSB_t)(IDirect3DDevice9*, IDirect3DStateBlock9**);
typedef HRESULT (STDMETHODCALLTYPE *SetRT_t)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT (STDMETHODCALLTYPE *SetXfm_t)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
typedef HRESULT (STDMETHODCALLTYPE *SetVP_t)(IDirect3DDevice9*, const D3DVIEWPORT9*);
typedef HRESULT (STDMETHODCALLTYPE *SetClipP_t)(IDirect3DDevice9*, DWORD, const float*);
typedef HRESULT (STDMETHODCALLTYPE *SetTex_t)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
typedef HRESULT (STDMETHODCALLTYPE *SetTSS_t)(IDirect3DDevice9*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *SetSS_t)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *SetScissor_t)(IDirect3DDevice9*, const RECT*);
typedef HRESULT (STDMETHODCALLTYPE *SetVDecl_t)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
typedef HRESULT (STDMETHODCALLTYPE *SetVS_t)(IDirect3DDevice9*, IDirect3DVertexShader9*);
typedef HRESULT (STDMETHODCALLTYPE *SetVxConstF_t)(IDirect3DDevice9*, UINT, const float*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *SetVxConstI_t)(IDirect3DDevice9*, UINT, const int*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *SetVxConstB_t)(IDirect3DDevice9*, UINT, const BOOL*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *SetStrm_t)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *SetIdx_t)(IDirect3DDevice9*, IDirect3DIndexBuffer9*);
typedef HRESULT (STDMETHODCALLTYPE *SetPS_t)(IDirect3DDevice9*, IDirect3DPixelShader9*);
typedef HRESULT (STDMETHODCALLTYPE *DrawPrim_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawIndexedPrim_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawPrimUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawIndexedPrimUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT, const void*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *SetPxConstF_t)(IDirect3DDevice9*, UINT, const float*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *SetPxConstI_t)(IDirect3DDevice9*, UINT, const int*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *SetPxConstB_t)(IDirect3DDevice9*, UINT, const BOOL*, UINT);
typedef UINT (STDMETHODCALLTYPE *GATM_t)(IDirect3DDevice9*);
typedef HRESULT (STDMETHODCALLTYPE *TCL_t)(IDirect3DDevice9*);
typedef HRESULT (STDMETHODCALLTYPE *Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE *Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

// ---- saved originals + state ----
static IDirect3DDevice9* g_device   = nullptr;
static void**            g_vtable   = nullptr;

static GATM_t            g_orig_GATM    = nullptr;
static TCL_t             g_orig_TCL     = nullptr;
static Reset_t           g_orig_Reset   = nullptr;
static Present_t         g_orig_Present = nullptr;
static CreateTex_t       g_orig_CreateTex = nullptr;
static CreateVB_t        g_orig_CreateVB  = nullptr;
static CreateIB_t        g_orig_CreateIB  = nullptr;
static SetMat_t          g_orig_SetMat    = nullptr;
static SetRS_t           g_orig_SRS     = nullptr;
static CreateSB_t        g_orig_CSB     = nullptr;
static BeginSB_t         g_orig_BSB     = nullptr;
static EndSB_t           g_orig_ESB     = nullptr;
static SetRT_t           g_orig_RT      = nullptr;
static SetXfm_t          g_orig_Xfm     = nullptr;
static SetVP_t           g_orig_VP      = nullptr;
static SetClipP_t        g_orig_ClipP   = nullptr;
static SetTex_t          g_orig_ST      = nullptr;
static SetTSS_t          g_orig_TSS     = nullptr;
static SetSS_t           g_orig_SS      = nullptr;
static SetScissor_t      g_orig_Scissor = nullptr;
static SetVDecl_t        g_orig_VDecl   = nullptr;
static SetVS_t           g_orig_VShader = nullptr;
static SetVxConstF_t     g_orig_SVS     = nullptr;
static SetVxConstI_t     g_orig_SVSI    = nullptr;
static SetVxConstB_t     g_orig_SVSB    = nullptr;
static SetStrm_t         g_orig_Strm    = nullptr;
static SetIdx_t          g_orig_Idx     = nullptr;
static SetPS_t           g_orig_PShader = nullptr;
static SetPxConstF_t     g_orig_SPS     = nullptr;
static SetPxConstI_t     g_orig_SPSI    = nullptr;
static SetPxConstB_t     g_orig_SPSB    = nullptr;

// SetRenderState DWORD cache - packed into single 8-byte entries.
// v13: combined separate g_rsCache+g_rsValid arrays for locality.
// v15: alignas(64) guarantees cache-line aligned start regardless
// of linker decisions. With 256 entries × 8 bytes = 2KB, this fits
// in 32 cache lines. First entry now starts at a line boundary.
struct RSEntry {
    DWORD   value;
    uint8_t valid;
    uint8_t pad[3];
};
static_assert(sizeof(RSEntry) == 8, "RSEntry must be 8 bytes for cache alignment");
alignas(64) static RSEntry g_rsCache[256];

// SetTexture cache: 16 PS texture stages (0-15) + 4 VS texture
// stages (D3DVERTEXTEXTURESAMPLER0-3 = 257-260, mapped to slots
// 16-19). Cache holds the last bound IDirect3DBaseTexture9*.
//
// Memory-aliasing safety: the device AddRefs a bound texture
// internally, so a cached pointer stays valid until rebound.
static IDirect3DBaseTexture9* g_textureCache[20] = {};

// SetTextureStageState cache: 8 stages × 33 state types
static DWORD   g_tssCache[8][33];
static uint8_t g_tssValid[8][33];

// SetSamplerState cache: 20 samplers × 14 state types, packed.
// v13 SAFE WIN: same logic as RSEntry. With ~217k calls/sec this is
// the single hottest hook in the system - the locality improvement
// matters most here.
// v15: alignas(64) guarantees first entry sits at a cache-line boundary.
struct SSEntry {
    DWORD   value;
    uint8_t valid;
    uint8_t pad[3];
};
static_assert(sizeof(SSEntry) == 8, "SSEntry must be 8 bytes for cache alignment");
alignas(64) static SSEntry g_ssCache[20][14];

// SetRenderTarget cache (v14 EXPERIMENT). D3D9 supports up to 4
// MRT slots (most engines use 1-2). Engines defensively re-bind
// the same RT before each pass, so this should cache well.
static IDirect3DSurface9* g_rtCache[4] = {};

static std::atomic<uint32_t> g_stateChanges{0}, g_stateChangesSkipped{0};

static std::atomic<uint32_t> g_textureCacheHits{0}, g_textureCacheMisses{0};


// SetVertexDeclaration / SetVertexShader / SetPixelShader / SetIndices:
// single-pointer caches.
static IDirect3DVertexDeclaration9* g_vdeclLast = nullptr;
static IDirect3DVertexShader9*      g_vshLast   = nullptr;
static IDirect3DPixelShader9*       g_pshLast   = nullptr;
static IDirect3DIndexBuffer9*       g_idxLast   = nullptr;

// SetStreamSource cache: 16 streams × (vb, offset, stride).
struct StreamSlot {
    IDirect3DVertexBuffer9* vb;
    UINT offset;
    UINT stride;
    uint8_t valid;
};
static StreamSlot g_streamCache[16] = {};

// =====================================================================
// v16: FRAME TIMING INSTRUMENTATION
// =====================================================================
//
// FPS averages hide what users actually feel. A 50fps average can be
// silky smooth (consistent 20ms frames) or jittery hell (alternating
// 5ms and 35ms). We need to measure frame-time *variance*, not just
// the mean.
//
// Strategy: QueryPerformanceCounter on every Present. Maintain a
// histogram of frame times in 32 buckets covering 0-100ms (3.125ms
// each), plus running min/max/sum/count and a spike counter (frames
// > 2x running average).
//
// At log time we compute average, p50, p95, p99 from the histogram.
// Reset stats per logging interval to get fresh data each window.
//
// Cost: QueryPerformanceCounter is ~10-30 cycles on modern hardware.
// Histogram update is one division + array increment. Total overhead
// per frame: ~50 cycles = effectively free at 50-75 fps.
//
// All access from a single thread (the render thread, where Present
// is called) - no locking needed for the hot path. Stats reader (other
// thread) uses snapshot copies via simple loads.

constexpr int      kFrameHistBuckets   = 32;
constexpr uint64_t kFrameHistRangeUs   = 100000;  // 0..100ms
constexpr uint64_t kFrameHistBucketUs  = kFrameHistRangeUs / kFrameHistBuckets;  // 3.125ms

// Pack the frame timing state into one struct for cache locality.
// __declspec(align(64)) keeps it in its own cache line.
struct __declspec(align(64)) FrameTimingState {
    LARGE_INTEGER qpcFreq;        // QPC ticks per second (constant after init)
    LARGE_INTEGER lastFrameQPC;   // QPC at the previous Present
    uint64_t      minUs;
    uint64_t      maxUs;
    uint64_t      sumUs;
    uint32_t      sampleCount;
    uint32_t      spikeCount;     // frames > 2x running avg (aggregate)
    uint32_t      spikesLoggedThisWindow;  // v37: cap on per-spike LOG lines
    uint32_t      histogram[kFrameHistBuckets];
};
static FrameTimingState g_ft = {};

// Reset frame timing state at the start of each logging window.
// Called only from the stats thread immediately AFTER reading the
// snapshot for logging. Safe because: the render thread only writes,
// the stats thread reads then resets. A few samples might race during
// the reset window but that's tolerable for an aggregate metric.
static void ResetFrameTiming()
{
    g_ft.minUs       = 0;
    g_ft.maxUs       = 0;
    g_ft.sumUs       = 0;
    g_ft.sampleCount = 0;
    g_ft.spikeCount  = 0;
    g_ft.spikesLoggedThisWindow = 0;  // v37
    memset(g_ft.histogram, 0, sizeof(g_ft.histogram));
    // Don't reset lastFrameQPC - we want continuity across windows.
}

// Compute percentile from the histogram. p in [0..100].
// Returns microseconds. If no samples, returns 0.
static uint64_t FrameTimePercentile(const uint32_t* hist, uint32_t total, int p)
{
    if (total == 0) return 0;
    uint64_t target = ((uint64_t)total * p + 50) / 100;
    if (target == 0) target = 1;
    uint64_t cumulative = 0;
    for (int i = 0; i < kFrameHistBuckets; ++i) {
        cumulative += hist[i];
        if (cumulative >= target) {
            // Return the upper bound of this bucket.
            return (uint64_t)(i + 1) * kFrameHistBucketUs;
        }
    }
    return kFrameHistRangeUs;  // saturated; frame > 100ms
}

// =====================================================================
// v16: RESOURCE CREATION OBSERVERS
// =====================================================================
//
// Counter-only hooks on CreateTexture / CreateVertexBuffer /
// CreateIndexBuffer. These DO NOT intercept any logic - they just
// count and forward. The point is to empirically measure the
// "flooding when turning" phenomenon by exposing per-window resource
// creation rates.
//
// Byte-size estimate is rough: actual GPU memory cost is more than
// w*h*bpp due to alignment and mip storage, but the relative
// comparison across windows still tells us whether streaming is
// quiet or busy.

static volatile uint32_t g_createTexCalls   = 0;
static volatile uint64_t g_createTexBytes   = 0;
static volatile uint32_t g_createVBCalls    = 0;
static volatile uint64_t g_createVBBytes    = 0;
static volatile uint32_t g_createIBCalls    = 0;
static volatile uint64_t g_createIBBytes    = 0;

// v24: anti-spike timing infrastructure. The "frame droppings" the user
// reported are concentrated in resource creation bursts (~280/sec during
// loading, 56/sec stable). We need to know:
//   1) Are individual creates slow, or just numerous?
//   2) Do spikes correlate with allocation density per frame?
//   3) Which create type (Tex/VB/IB) is the worst offender?
static volatile uint64_t g_createTexUs      = 0; // cumulative microseconds in CreateTexture
static volatile uint64_t g_createVBUs       = 0;
static volatile uint64_t g_createIBUs       = 0;
static volatile uint32_t g_createTexSlow    = 0; // count of CreateTex calls > 2ms
static volatile uint32_t g_createVBSlow     = 0;
static volatile uint32_t g_createIBSlow     = 0;
static volatile uint32_t g_createTexMaxUs   = 0; // single slowest call
static volatile uint32_t g_createVBMaxUs    = 0;
static volatile uint32_t g_createIBMaxUs    = 0;

// Per-frame creation counter. Reset at top of Hook_Present after we've
// read the previous frame's value. Lets us correlate spikes with
// allocation bursts in the SAME frame.
static volatile uint32_t g_createsThisFrame = 0;

// Spike correlation log: when a frame is > kSpikeThresholdUs, we log
// the create count for that frame. Capped to avoid log spam during
// loading bursts where every other frame would qualify.
constexpr uint64_t kSpikeThresholdUs = 25000; // 25 ms = 40 fps frame
constexpr uint32_t kMaxSpikeLogsPerWindow = 10;
static volatile uint32_t g_spikeLogsThisWindow = 0;
static volatile uint32_t g_spikesObserved      = 0;
static volatile uint32_t g_spikesWithCreates   = 0; // spikes that had creates this frame
static volatile uint32_t g_maxCreatesInSpike   = 0; // largest create burst in any spike

// Estimate bytes per pixel for D3D9 formats. Conservative (slightly
// under-estimates for compressed formats but they're the common case
// so it's fine for relative measurement).
static uint32_t EstimateBpp(D3DFORMAT fmt)
{
    switch (fmt) {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_A8B8G8R8:
        case D3DFMT_X8B8G8R8:
        case D3DFMT_A2B10G10R10:
        case D3DFMT_A2R10G10B10:
        case D3DFMT_G16R16:
        case D3DFMT_D24S8:
        case D3DFMT_D24X8:
        case D3DFMT_D32:
        case D3DFMT_R32F:           return 4;
        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4:
        case D3DFMT_A8L8:
        case D3DFMT_V8U8:
        case D3DFMT_L16:
        case D3DFMT_D16:
        case D3DFMT_R16F:
        case D3DFMT_G16R16F:        return 2;
        case D3DFMT_L8:
        case D3DFMT_A8:
        case D3DFMT_P8:             return 1;
        case D3DFMT_A16B16G16R16:
        case D3DFMT_A16B16G16R16F:
        case D3DFMT_G32R32F:        return 8;
        case D3DFMT_A32B32G32R32F:  return 16;
        // Compressed formats: DXT1=4bpp, DXT3/5=8bpp. Return ~1 byte/px
        // approximation since we use uint32_t bpp.
        case D3DFMT_DXT1:           return 1;  // actually 0.5 bpp but min 1
        case D3DFMT_DXT2:
        case D3DFMT_DXT3:
        case D3DFMT_DXT4:
        case D3DFMT_DXT5:           return 1;  // actually 1 bpp
        default:                    return 4;  // default assume 32bpp
    }
}

// SetTransform cache (v13 EXPERIMENT - kept in v14, hit rate 50%).
// D3D9 fixed-function transform states 0-8 are well-known (World,
// View, Projection, etc). Texture transforms 256-263 are also common.
// We cache all 512 possible states. Each entry is 68 bytes (64 data + 4 pad)
// for a total of ~34KB - fits comfortably in L2.
struct XfmEntry {
    D3DMATRIX m;
    uint8_t   valid;
    uint8_t   pad[3];
};
static_assert(sizeof(XfmEntry) == 68, "XfmEntry layout check");
alignas(64) static XfmEntry g_xfmCache[512];


// SetViewport cache (v14 EXPERIMENT). Viewport is usually
// full-screen for the main pass plus small viewports for shadow
// maps / post-process. Total unique viewports per frame is small,
// re-binding the same one should cache at high rate.
// D3DVIEWPORT9 = 6 DWORDs = 24 bytes. memcmp is cheap.
static D3DVIEWPORT9 g_vpCache = {};
static uint8_t      g_vpValid = 0;

// CreateTexture observer (v15 DIAGNOSTIC). vtable[23]. Counter-only:
// hook simply increments a counter and forwards. This lets us
// empirically measure the texture creation rate, which is what
// causes the spike-when-turning behavior. We're NOT trying to
// cache or skip these - just measure.
//
// The pipeline that triggers a CreateTexture call:
//   sub_AB1430 (NIF parser) reads .nif file
//     -> NetImmerse texture cache miss
//        -> sub_CE7300 (D3DXCreateTextureFromFileInMemory wrapper)
//           -> device->CreateTexture  (THIS IS WHAT WE COUNT)
//              -> DXVK -> vkCreateImage + vkAllocateMemory
//
// At steady state during normal gameplay this should be near zero.
// During cell transitions and view-frustum churn (turning) we
// expect bursts of dozens per frame.

// v14: SetVertexDeclaration trace REMOVED. v13 diagnostic showed
// the engine cycles 8 declarations interleaved across draws (e.g.
// decl 0x31244760 used 90k times but never adjacent to itself).
// Pointer-equality cache cannot help; trace overhead removed to
// speed up the hot path slightly.

// GPU cleanup heartbeat
static ULONGLONG g_lastEvictTick = 0;
static volatile uint32_t g_evictCount = 0;
static volatile uint32_t g_evictDeferred = 0;

// v16: Creation pressure - rises when textures/buffers are being created,
// decays each frame. Used to defer eviction during streaming bursts
// (camera in/out, cell transitions). Evicting while the engine is
// actively loading new resources makes the stutter worse - we'd be
// freeing things it's about to need.
//
// Each CreateXxx call adds 4 to pressure. Each frame decays by 1.
// At pressure >= 8 (~2 creates/frame steady-state), eviction is deferred.
// Once pressure drops, eviction resumes on the next 20s tick.
static volatile uint32_t g_creationPressure = 0;
constexpr uint32_t kCreationPressureThreshold = 8;
// v17: cap pressure to prevent unbounded growth from loading bursts.
// Without a cap, a startup burst of ~2000 textures pushes pressure
// to ~8000 which takes 8000 frames (>2 minutes) to drain at -1/frame.
// v16's data showed this caused eviction to never fire after startup.
// With cap=64, a single max burst drains in 64 frames (~1 second).
constexpr uint32_t kCreationPressureCap = 64;
// v17: safety valve. If we've deferred eviction for too long, force
// it through regardless of pressure. Prevents indefinite deferral
// and keeps the GPU memory cleanup contract honored.
constexpr ULONGLONG kForceEvictAfterMs = 60000;  // 60 seconds

// =====================================================================
// v17: SHADER CONSTANT INTEGER / BOOLEAN CACHES
// =====================================================================
// D3D9 has separate banks for float (SetVertexShaderConstantF, hooked
// via engine cache hooks) and integer / boolean constants. Integers
// and booleans are sparser (~16 slots typical) and used for branching
// hints, sampler indices, etc. Cache them with simple per-slot
// tracking - the values are tiny so memcmp is overkill, just
// element-by-element compare.
//
// Per D3D9 spec: max 16 integer constants, max 16 boolean constants
// per shader stage. Each integer constant is a 4-int vector; booleans
// are a single BOOL each.
//
// v18: DISABLED - confirmed 0 calls in v17. Skyrim's shaders use only
// float constants (banks at 0x01BAC080) - integer and boolean constant
// banks are untouched by the engine. Keeping the cache structures for
// potential future use but removing the hooks to avoid overhead.

static int     g_vsiCache[16][4];   // VS integer constants (16 vec4s)
static uint8_t g_vsiValid[16];
static int     g_psiCache[16][4];   // PS integer constants
static uint8_t g_psiValid[16];
static BOOL    g_vsbCache[16];      // VS boolean constants (BOOL each)
static uint8_t g_vsbValid[16];
static BOOL    g_psbCache[16];      // PS boolean constants
static uint8_t g_psbValid[16];

// SetMaterial cache (vtable[35]). Fixed-function lighting material.
// D3DMATERIAL9 is 17 floats = 68 bytes. Used in legacy paths only -
// shader-driven engines bypass it.
//
// v18: DISABLED - confirmed 0 calls in v17. Skyrim is fully shader-driven
// and never uses fixed-function lighting. Keeping the cache structure for
// potential future use but removing the hook to avoid overhead.
static D3DMATERIAL9 g_matCache = {};
static uint8_t      g_matValid = 0;

// =====================================================================
// SYSTEM TIMER STATE
// =====================================================================
// timeBeginPeriod(1) is called once at install. The flag prevents
// double-application if Install() is invoked more than once for any
// reason (e.g. device reset paths).
static bool   g_timerSet = false;
static volatile uint32_t g_presentCount = 0;

static inline int TextureStageIndex(DWORD stage)
{
    if (stage < 16) return (int)stage;
    if (stage >= 257 && stage <= 260) return 16 + (int)(stage - 257);
    return -1;
}

// ---- hook functions ----

UINT STDMETHODCALLTYPE Hook_GetAvailableTextureMem(IDirect3DDevice9* dev)
{
    UINT real = g_orig_GATM(dev);
    // Skyrim is 32-bit and tops out around 3GB of address space.
    // Reporting a generous VRAM number tells the engine "feel free
    // to keep textures resident" so it streams less from disk.
    constexpr UINT kMin = 0x80000000u;  // 2 GiB
    return (real < kMin) ? kMin : real;
}

HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* dev,
                                       const RECT* src, const RECT* dst,
                                       HWND hwndOverride, const RGNDATA* dirty)
{
    // v37: per-frame timing PLUS per-spike event logging.
    // When off: this entire block compiles to a single load+test+jump,
    // ~1ns total. When on: ~50ns of QPC + histogram, plus a LOG line
    // only when a spike actually happens (rare).
    if (IniConfig::g_cfg.ProfilingEnabled) {
        LARGE_INTEGER nowQPC;
        QueryPerformanceCounter(&nowQPC);
        if (g_ft.lastFrameQPC.QuadPart != 0 && g_ft.qpcFreq.QuadPart != 0) {
            uint64_t deltaTicks = (uint64_t)(nowQPC.QuadPart - g_ft.lastFrameQPC.QuadPart);
            uint64_t deltaUs = (deltaTicks * 1000000ULL) / (uint64_t)g_ft.qpcFreq.QuadPart;
            if (deltaUs < kFrameHistRangeUs) {
                if (g_ft.sampleCount == 0 || deltaUs < g_ft.minUs) g_ft.minUs = deltaUs;
                if (deltaUs > g_ft.maxUs) g_ft.maxUs = deltaUs;
                g_ft.sumUs += deltaUs;
                if (g_ft.sampleCount > 30) {
                    uint64_t avgUs = g_ft.sumUs / g_ft.sampleCount;
                    if (deltaUs > 2 * avgUs) g_ft.spikeCount++;
                }
                g_ft.sampleCount++;
                uint32_t bucket = (uint32_t)(deltaUs / kFrameHistBucketUs);
                if (bucket >= kFrameHistBuckets) bucket = kFrameHistBuckets - 1;
                g_ft.histogram[bucket]++;

                // v37: Per-spike event log. Fires when this individual
                // frame exceeded StutterThresholdMs. Capped per window
                // so loading screens don't spam the log.
                int thresholdMs = IniConfig::g_cfg.StutterThresholdMs;
                if (thresholdMs > 0) {
                    uint64_t thresholdUs = (uint64_t)thresholdMs * 1000ULL;
                    if (deltaUs >= thresholdUs &&
                        g_ft.spikesLoggedThisWindow <
                            (uint32_t)IniConfig::g_cfg.MaxSpikesPerWindow)
                    {
                        // Snapshot context for the spike line.
                        uint32_t allocs = g_createsThisFrame;
                        uint32_t pressure = g_creationPressure;
                        // sample counter so user can tell which frame
                        // (relative to start of window) the spike was.
                        LOG("SPIKE frame=%u delta=%llu us (%.1f ms) "
                            "allocs_this_frame=%u creation_pressure=%u",
                            g_ft.sampleCount,
                            (unsigned long long)deltaUs,
                            deltaUs / 1000.0,
                            allocs, pressure);
                        g_ft.spikesLoggedThisWindow++;
                    }
                }
            }
        }
        g_ft.lastFrameQPC = nowQPC;

        // Reset the per-frame allocation counter AFTER reading it for
        // the spike line above. This keeps allocs_this_frame attached
        // to the correct frame.
        g_createsThisFrame = 0;
    }

    // Decay streaming pressure each frame. When pressure is low, the
    // periodic EvictManagedResources call below is allowed to run.
    if (g_creationPressure > 0) g_creationPressure--;

    // GPU cleanup heartbeat: every ~20s of game time, ask the device
    // to evict managed-pool resources. v17 OVERHAUL:
    //   - pressure cap at 64 ensures bursts drain in ~1 sec
    //   - safety valve forces eviction after 60s regardless of pressure
    //     (guarantees the cleanup contract is honored)
    ULONGLONG now = GetTickCount64();
    if (g_lastEvictTick == 0) {
        g_lastEvictTick = now;
    } else if (now - g_lastEvictTick > 20000) {
        bool forceEvict = (now - g_lastEvictTick > kForceEvictAfterMs);
        if (forceEvict || g_creationPressure < kCreationPressureThreshold) {
            dev->EvictManagedResources();
            g_lastEvictTick = now;
        } else {
            // Defer: still under streaming pressure. Don't reset the
            // tick - we'll try again on the next Present.
        }
    }

    return g_orig_Present(dev, src, dst, hwndOverride, dirty);
}

HRESULT STDMETHODCALLTYPE Hook_SetRenderState(
    IDirect3DDevice9* dev, D3DRENDERSTATETYPE state, DWORD value)
{
    if ((unsigned)state < 256) {
        RSEntry& e = g_rsCache[state];
        if (e.valid && e.value == value) {
            return D3D_OK;
        }
        e.value = value;
        e.valid = 1;
    }
    return g_orig_SRS(dev, state, value);
}

HRESULT STDMETHODCALLTYPE Hook_CreateStateBlock(
    IDirect3DDevice9* dev, D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9** sb)
{
    DeviceHooks::InvalidateAll();
    return g_orig_CSB(dev, t, sb);
}
HRESULT STDMETHODCALLTYPE Hook_BeginStateBlock(IDirect3DDevice9* dev)
{
    // NOTE: BeginStateBlock only RECORDS subsequent state changes into a
    // capture buffer - it does NOT apply them to the device. Invalidating
    // the cache here was overly aggressive and caused a burst of cache
    // misses for every state change during the recording period.
    // Only EndStateBlock (which captures current device state) and
    // CreateStateBlock need invalidation.
    return g_orig_BSB(dev);
}
HRESULT STDMETHODCALLTYPE Hook_EndStateBlock(
    IDirect3DDevice9* dev, IDirect3DStateBlock9** sb)
{
    DeviceHooks::InvalidateAll();
    return g_orig_ESB(dev, sb);
}

HRESULT STDMETHODCALLTYPE Hook_SetTexture(
    IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex)
{
    int idx = TextureStageIndex(stage);
    if (idx >= 0) {
        if (g_textureCache[idx] == tex) {
            g_textureCacheHits.fetch_add(1, std::memory_order_relaxed);
            return D3D_OK;
        }
        g_textureCache[idx] = tex;
        g_textureCacheMisses.fetch_add(1, std::memory_order_relaxed);
    }
    return g_orig_ST(dev, stage, tex);
}

HRESULT STDMETHODCALLTYPE Hook_SetTextureStageState(
    IDirect3DDevice9* dev, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
    if (stage < 8 && (unsigned)type < 33) {
        if (g_tssValid[stage][type] && g_tssCache[stage][type] == value) {
            return D3D_OK;
        }
        g_tssCache[stage][type] = value;
        g_tssValid[stage][type] = 1;
    }
    return g_orig_TSS(dev, stage, type, value);
}

HRESULT STDMETHODCALLTYPE Hook_SetSamplerState(
    IDirect3DDevice9* dev, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
{
    if (sampler < 20 && (unsigned)type < 14) {
        SSEntry& e = g_ssCache[sampler][type];
        if (e.valid && e.value == value) {
            return D3D_OK;
        }
        e.value = value;
        e.valid = 1;
    }
    return g_orig_SS(dev, sampler, type, value);
}

// ---- v12 NEW HOOKS ----

HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    LOG("Hook_Reset called - invalidating all caches");
    HRESULT hr = g_orig_Reset(dev, pp);
    // After Reset, all device state is clobbered. Invalidate every cache
    // we maintain so we don't return stale "skip this" answers.
    DeviceHooks::InvalidateAll();
    return hr;
}

// v13 EXPERIMENT: cache fixed-function transform matrices (View, Projection,
// etc are very stable across draws if used).
HRESULT STDMETHODCALLTYPE Hook_SetTransform(
    IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m)
{
    if ((unsigned)state < 512) {
        XfmEntry& e = g_xfmCache[state];
        if (e.valid && memcmp(&e.m, m, sizeof(D3DMATRIX)) == 0) {
            return D3D_OK;
        }
        e.m = *m;
        e.valid = 1;
    }
    return g_orig_Xfm(dev, state, m);
}


HRESULT STDMETHODCALLTYPE Hook_SetVertexDeclaration(
    IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl)
{
    if (g_vdeclLast == decl) {
        return D3D_OK;
    }
    g_vdeclLast = decl;
    return g_orig_VDecl(dev, decl);
}

// v14 EXPERIMENT: cache render target binding per slot. Engines often
// re-bind the same RT before each draw pass for safety; should cache.
HRESULT STDMETHODCALLTYPE Hook_SetRenderTarget(
    IDirect3DDevice9* dev, DWORD idx, IDirect3DSurface9* rt)
{
    if (idx < 4) {
        if (g_rtCache[idx] == rt) {
            return D3D_OK;
        }
        g_rtCache[idx] = rt;
    }
    return g_orig_RT(dev, idx, rt);
}

// v14 EXPERIMENT: cache viewport. Most frames have one main viewport
// plus a few for shadow maps / post-process. Engines re-set defensively
// before each pass.
HRESULT STDMETHODCALLTYPE Hook_SetViewport(
    IDirect3DDevice9* dev, const D3DVIEWPORT9* vp)
{
    if (g_vpValid && 
        g_vpCache.X == vp->X && g_vpCache.Y == vp->Y &&
        g_vpCache.Width == vp->Width && g_vpCache.Height == vp->Height &&
        g_vpCache.MinZ == vp->MinZ && g_vpCache.MaxZ == vp->MaxZ) {
        return D3D_OK;
    }
    g_vpCache = *vp;
    g_vpValid = 1;
    return g_orig_VP(dev, vp);
}

// v15 SetClipPlane hook removed in v16: confirmed 0 calls in entire
// v15 session. Skyrim does clipping in shaders, never via D3D9 user
// clip planes. Keeping the typedef and orig pointer for code minimum
// disruption but the hook function and PatchOne are gone.

// =====================================================================
// v16 NEW HOOKS
// =====================================================================

// TestCooperativeLevel: called every frame by Skyrim's render loop
// to detect device loss (alt-tab during fullscreen). 99%+ of the time
// it returns D3D_OK. Cache the last result; if it was D3D_OK, return
// D3D_OK directly without forwarding.
//
// SAFETY: We DO need to forward the call when the result might have
// changed - but D3D_OK is a stable steady-state. If the GPU does
// glitch (alt-tab, driver reset), DXVK will eventually trigger a
// different code path. To be safe, we sample - forward 1 in N calls
// to keep the cache fresh.
constexpr uint32_t kTCLSampleEveryN = 60;  // ~once/sec at 60fps
static volatile uint32_t g_tclCalls = 0;
static volatile uint32_t g_tclSkipped = 0;
static volatile uint32_t g_tclSampleCounter = 0;
static HRESULT g_tclLastResult = D3D_OK;

HRESULT STDMETHODCALLTYPE Hook_TestCooperativeLevel(IDirect3DDevice9* dev)
{
    g_tclSampleCounter++;
    // Force-refresh every N calls in case the device state changes.
    if (g_tclSampleCounter >= kTCLSampleEveryN) {
        g_tclSampleCounter = 0;
        g_tclLastResult = g_orig_TCL(dev);
        return g_tclLastResult;
    }
    // If last known state was OK, return OK without going through
    // DXVK (saves the call entirely).
    if (g_tclLastResult == D3D_OK) {
        return D3D_OK;
    }
    // Otherwise forward and update cache - lost device states need
    // accurate reporting.
    g_tclLastResult = g_orig_TCL(dev);
    return g_tclLastResult;
}

// CreateTexture observer: counter only. The "flooding when turning"
// suspect. If we see 30+ calls per frame during streaming bursts,
// that's the spike source.
HRESULT STDMETHODCALLTYPE Hook_CreateTexture(
    IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage,
    D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared)
{
    // Bump streaming pressure so adaptive eviction holds off during bursts.
    if (g_creationPressure < kCreationPressureCap) g_creationPressure += 4;
    else g_creationPressure = kCreationPressureCap;
    g_createsThisFrame++;
    return g_orig_CreateTex(dev, w, h, levels, usage, fmt, pool, out, shared);
}

HRESULT STDMETHODCALLTYPE Hook_CreateVertexBuffer(
    IDirect3DDevice9* dev, UINT length, DWORD usage, DWORD fvf,
    D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE* shared)
{
    if (g_creationPressure < kCreationPressureCap) g_creationPressure += 2;
    else g_creationPressure = kCreationPressureCap;
    g_createsThisFrame++;
    return g_orig_CreateVB(dev, length, usage, fvf, pool, out, shared);
}

HRESULT STDMETHODCALLTYPE Hook_CreateIndexBuffer(
    IDirect3DDevice9* dev, UINT length, DWORD usage, D3DFORMAT fmt,
    D3DPOOL pool, IDirect3DIndexBuffer9** out, HANDLE* shared)
{
    if (g_creationPressure < kCreationPressureCap) g_creationPressure += 2;
    else g_creationPressure = kCreationPressureCap;
    g_createsThisFrame++;
    return g_orig_CreateIB(dev, length, usage, fmt, pool, out, shared);
}

// SetScissorRect cache. Most engines either disable scissor or set
// the full viewport. Same rect re-asserted before each draw is common
// when scissor IS used. RECT = 4 LONG = 16 bytes; cheap to compare.
static RECT    g_scissorCache = {};
static uint8_t g_scissorValid = 0;

HRESULT STDMETHODCALLTYPE Hook_SetScissorRect(
    IDirect3DDevice9* dev, const RECT* rect)
{
    if (rect && g_scissorValid &&
        rect->left   == g_scissorCache.left   &&
        rect->top    == g_scissorCache.top    &&
        rect->right  == g_scissorCache.right  &&
        rect->bottom == g_scissorCache.bottom)
    {
        return D3D_OK;
    }
    if (rect) {
        g_scissorCache = *rect;
        g_scissorValid = 1;
    }
    return g_orig_Scissor(dev, rect);
}

HRESULT STDMETHODCALLTYPE Hook_SetVertexShader(
    IDirect3DDevice9* dev, IDirect3DVertexShader9* shader)
{
    if (g_vshLast == shader) {
        return D3D_OK;
    }
    g_vshLast = shader;
    return g_orig_VShader(dev, shader);
}

HRESULT STDMETHODCALLTYPE Hook_SetPixelShader(
    IDirect3DDevice9* dev, IDirect3DPixelShader9* shader)
{
    if (g_pshLast == shader) {
        return D3D_OK;
    }
    g_pshLast = shader;
    return g_orig_PShader(dev, shader);
}


HRESULT STDMETHODCALLTYPE Hook_SetStreamSource(
    IDirect3DDevice9* dev, UINT stream, IDirect3DVertexBuffer9* vb,
    UINT offset, UINT stride)
{
    if (stream < 16) {
        StreamSlot& s = g_streamCache[stream];
        if (s.valid && s.vb == vb && s.offset == offset && s.stride == stride) {
            g_stateChangesSkipped.fetch_add(1, std::memory_order_relaxed);
            return D3D_OK;
        }
        s.vb = vb;
        s.offset = offset;
        s.stride = stride;
        s.valid = 1;
    }
    g_stateChanges.fetch_add(1, std::memory_order_relaxed);
    return g_orig_Strm(dev, stream, vb, offset, stride);
}

HRESULT STDMETHODCALLTYPE Hook_SetIndices(
    IDirect3DDevice9* dev, IDirect3DIndexBuffer9* ib)
{
    if (g_idxLast == ib) {
        return D3D_OK;
    }
    g_idxLast = ib;
    return g_orig_Idx(dev, ib);
}

// ---- end v12 NEW HOOKS ----

HRESULT STDMETHODCALLTYPE Hook_SetVertexShaderConstantF(
    IDirect3DDevice9* dev, UINT sr, const float* d, UINT cnt)
{
    EngineCacheHooks::UpdateFromUpload(false, sr, d, cnt);
    return g_orig_SVS(dev, sr, d, cnt);
}

HRESULT STDMETHODCALLTYPE Hook_SetPixelShaderConstantF(
    IDirect3DDevice9* dev, UINT sr, const float* d, UINT cnt)
{
    EngineCacheHooks::UpdateFromUpload(true, sr, d, cnt);
    return g_orig_SPS(dev, sr, d, cnt);
}

// =====================================================================
// v17 NEW HOOKS
// =====================================================================

// Helper for integer-vec4 constant comparison.
static inline bool IntVec4Equal(const int* a, const int* b)
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

// VS integer constants. Most engines set 1 vec4 of ints at a time
// (sampler indices, branching counts). Cache 16 slots, skip exact
// repeats. Only handle single-vec4 calls for simplicity; multi-vec4
// uploads pass through.
HRESULT STDMETHODCALLTYPE Hook_SetVertexShaderConstantI(
    IDirect3DDevice9* dev, UINT sr, const int* data, UINT cnt)
{
    if (cnt == 1 && sr < 16 && data) {
        if (g_vsiValid[sr] && IntVec4Equal(g_vsiCache[sr], data)) {
            return D3D_OK;
        }
        g_vsiCache[sr][0] = data[0];
        g_vsiCache[sr][1] = data[1];
        g_vsiCache[sr][2] = data[2];
        g_vsiCache[sr][3] = data[3];
        g_vsiValid[sr] = 1;
    }
    return g_orig_SVSI(dev, sr, data, cnt);
}

// VS boolean constants. Each is a single BOOL. Engines flip these for
// shader feature toggles (skin / no skin, alpha / opaque, etc).
HRESULT STDMETHODCALLTYPE Hook_SetVertexShaderConstantB(
    IDirect3DDevice9* dev, UINT sr, const BOOL* data, UINT cnt)
{
    if (cnt == 1 && sr < 16 && data) {
        if (g_vsbValid[sr] && g_vsbCache[sr] == *data) {
            return D3D_OK;
        }
        g_vsbCache[sr] = *data;
        g_vsbValid[sr] = 1;
    }
    return g_orig_SVSB(dev, sr, data, cnt);
}

HRESULT STDMETHODCALLTYPE Hook_SetPixelShaderConstantI(
    IDirect3DDevice9* dev, UINT sr, const int* data, UINT cnt)
{
    if (cnt == 1 && sr < 16 && data) {
        if (g_psiValid[sr] && IntVec4Equal(g_psiCache[sr], data)) {
            return D3D_OK;
        }
        g_psiCache[sr][0] = data[0];
        g_psiCache[sr][1] = data[1];
        g_psiCache[sr][2] = data[2];
        g_psiCache[sr][3] = data[3];
        g_psiValid[sr] = 1;
    }
    return g_orig_SPSI(dev, sr, data, cnt);
}

HRESULT STDMETHODCALLTYPE Hook_SetPixelShaderConstantB(
    IDirect3DDevice9* dev, UINT sr, const BOOL* data, UINT cnt)
{
    if (cnt == 1 && sr < 16 && data) {
        if (g_psbValid[sr] && g_psbCache[sr] == *data) {
            return D3D_OK;
        }
        g_psbCache[sr] = *data;
        g_psbValid[sr] = 1;
    }
    return g_orig_SPSB(dev, sr, data, cnt);
}


template<typename FnT>
bool PatchOne(int idx, FnT hook, FnT* origOut, const char* name)
{
    void* orig = g_vtable[idx];
    *origOut = (FnT)orig;
    g_vtable[idx] = (void*)hook;
    LOG("  vtable[%3d] %-26s: %p -> %p", idx, name, orig, hook);
    return true;
}

} // anon namespace

namespace DeviceHooks {

bool Install(IDirect3DDevice9* device)
{
    if (!device) return false;
    if (g_device) {
        LOG("DeviceHooks::Install: already installed");
        return false;
    }

    g_device = device;
    g_vtable = *(void***)device;
    LOG("DeviceHooks::Install: device=%p vtable=%p", device, g_vtable);

    // Patches go up to index 109. 110 entries * 4 bytes = 440 bytes;
    // round to 512 for VirtualProtect.
    constexpr SIZE_T kVtableBytes = 512;
    DWORD oldProtect = 0;
    if (!VirtualProtect(g_vtable, kVtableBytes, PAGE_READWRITE, &oldProtect)) {
        LOG("DeviceHooks::Install: VirtualProtect failed err=%lu", GetLastError());
        g_device = nullptr;
        g_vtable = nullptr;
        return false;
    }

    PatchOne<TCL_t>        (kIdx_TestCooperativeLevel,     &Hook_TestCooperativeLevel,     &g_orig_TCL,     "TestCooperativeLevel");
    PatchOne<GATM_t>       (kIdx_GetAvailableTextureMem,   &Hook_GetAvailableTextureMem,   &g_orig_GATM,    "GetAvailableTextureMem");
    PatchOne<Reset_t>      (kIdx_Reset,                    &Hook_Reset,                    &g_orig_Reset,   "Reset");
    PatchOne<Present_t>    (kIdx_Present,                  &Hook_Present,                  &g_orig_Present, "Present");
    PatchOne<CreateTex_t>  (kIdx_CreateTexture,            &Hook_CreateTexture,            &g_orig_CreateTex, "CreateTexture");
    PatchOne<CreateVB_t>   (kIdx_CreateVertexBuffer,       &Hook_CreateVertexBuffer,       &g_orig_CreateVB,  "CreateVertexBuffer");
    PatchOne<CreateIB_t>   (kIdx_CreateIndexBuffer,        &Hook_CreateIndexBuffer,        &g_orig_CreateIB,  "CreateIndexBuffer");

    // v37: Cache hooks gated on master switch. When the master switch
    // is OFF, the SetXxx vtable entries are NOT patched and Skyrim's
    // calls go straight to the D3D9 driver - that's the vanilla path.
    // The hooks above (TestCooperativeLevel, GATM, Reset, Present,
    // CreateXxx) are always installed because:
    //   - Present is needed for frame timing / spike logging
    //   - Reset/GATM are tiny stubs that don't change behavior
    //   - CreateXxx are needed for resource tracking counters
    //
    // When the master switch is ON but [Features] CacheHooks=false,
    // the SetXxx hooks are also skipped.
    const bool installCacheHooks = IniConfig::g_cfg.Enabled;
    if (installCacheHooks) {
    // v18: SetMaterial DISABLED. v17 confirmed 0 calls in 60s session.
    // Skyrim is fully shader-driven and never uses fixed-function lighting.
    // PatchOne<SetMat_t>     (kIdx_SetMaterial,              &Hook_SetMaterial,              &g_orig_SetMat,    "SetMaterial");
    PatchOne<SetRS_t>      (kIdx_SetRenderState,           &Hook_SetRenderState,           &g_orig_SRS,     "SetRenderState");
    PatchOne<CreateSB_t>   (kIdx_CreateStateBlock,         &Hook_CreateStateBlock,         &g_orig_CSB,     "CreateStateBlock");
    PatchOne<BeginSB_t>    (kIdx_BeginStateBlock,          &Hook_BeginStateBlock,          &g_orig_BSB,     "BeginStateBlock");
    PatchOne<EndSB_t>      (kIdx_EndStateBlock,            &Hook_EndStateBlock,            &g_orig_ESB,     "EndStateBlock");
    // v15: SetRenderTarget hook NOT INSTALLED (0% hit rate confirmed).
    // PatchOne<SetRT_t>      (kIdx_SetRenderTarget,          &Hook_SetRenderTarget,          &g_orig_RT,      "SetRenderTarget");
    PatchOne<SetXfm_t>     (kIdx_SetTransform,             &Hook_SetTransform,             &g_orig_Xfm,     "SetTransform");
    PatchOne<SetVP_t>      (kIdx_SetViewport,              &Hook_SetViewport,              &g_orig_VP,      "SetViewport");
    // v16: SetClipPlane removed (0 calls in v15).
    PatchOne<SetTex_t>     (kIdx_SetTexture,               &Hook_SetTexture,               &g_orig_ST,      "SetTexture");
    PatchOne<SetTSS_t>     (kIdx_SetTextureStageState,     &Hook_SetTextureStageState,     &g_orig_TSS,     "SetTextureStageState");
    PatchOne<SetSS_t>      (kIdx_SetSamplerState,          &Hook_SetSamplerState,          &g_orig_SS,      "SetSamplerState");
    PatchOne<SetScissor_t> (kIdx_SetScissorRect,           &Hook_SetScissorRect,           &g_orig_Scissor, "SetScissorRect");
    PatchOne<SetVDecl_t>   (kIdx_SetVertexDeclaration,     &Hook_SetVertexDeclaration,     &g_orig_VDecl,   "SetVertexDeclaration");
    PatchOne<SetVS_t>      (kIdx_SetVertexShader,          &Hook_SetVertexShader,          &g_orig_VShader, "SetVertexShader");
    PatchOne<SetVxConstF_t>(kIdx_SetVertexShaderConstantF, &Hook_SetVertexShaderConstantF, &g_orig_SVS,     "SetVertexShaderConstantF");
    PatchOne<SetStrm_t>    (kIdx_SetStreamSource,          &Hook_SetStreamSource,          &g_orig_Strm,    "SetStreamSource");
    PatchOne<SetIdx_t>     (kIdx_SetIndices,               &Hook_SetIndices,               &g_orig_Idx,     "SetIndices");
    PatchOne<SetPS_t>      (kIdx_SetPixelShader,           &Hook_SetPixelShader,           &g_orig_PShader, "SetPixelShader");
    PatchOne<SetPxConstF_t>(kIdx_SetPixelShaderConstantF,  &Hook_SetPixelShaderConstantF,  &g_orig_SPS,     "SetPixelShaderConstantF");
    } // installCacheHooks

    DWORD tmp;
    VirtualProtect(g_vtable, kVtableBytes, oldProtect, &tmp);

    memset(g_rsCache,      0, sizeof(g_rsCache));
    memset(g_textureCache, 0, sizeof(g_textureCache));
    memset(g_tssValid,     0, sizeof(g_tssValid));
    memset(g_ssCache,      0, sizeof(g_ssCache));
    memset(g_streamCache,  0, sizeof(g_streamCache));
    memset(g_xfmCache,     0, sizeof(g_xfmCache));
    memset(g_rtCache,      0, sizeof(g_rtCache));
    memset(&g_vpCache,     0, sizeof(g_vpCache));
    memset(&g_scissorCache, 0, sizeof(g_scissorCache));
    memset(g_vsiCache,     0, sizeof(g_vsiCache));
    memset(g_vsiValid,     0, sizeof(g_vsiValid));
    memset(g_psiCache,     0, sizeof(g_psiCache));
    memset(g_psiValid,     0, sizeof(g_psiValid));
    memset(g_vsbCache,     0, sizeof(g_vsbCache));
    memset(g_vsbValid,     0, sizeof(g_vsbValid));
    memset(g_psbCache,     0, sizeof(g_psbCache));
    memset(g_psbValid,     0, sizeof(g_psbValid));
    memset(&g_matCache,    0, sizeof(g_matCache));
    g_matValid      = 0;
    g_vpValid       = 0;
    g_scissorValid  = 0;
    g_vdeclLast     = nullptr;
    g_vshLast       = nullptr;
    g_pshLast       = nullptr;
    g_idxLast       = nullptr;

    // v16: Initialize frame timing - capture QPC frequency once.
    QueryPerformanceFrequency(&g_ft.qpcFreq);
    g_ft.lastFrameQPC.QuadPart = 0;
    ResetFrameTiming();

    // System timer resolution: 1ms instead of default 15.625ms.
    // ----------------------------------------------------------
    // Skyrim's engine, audio sync, and worker-thread Sleep() calls all
    // rely on the system timer. At default 15.6ms granularity, Sleep(1)
    // actually waits ~15ms. Bumping resolution to 1ms makes those waits
    // accurate to the millisecond, smoothing frame pacing and reducing
    // micro-stuttering.
    //
    // Available since Windows 95 - LoadLibrary'd anyway for hygiene.
    // The handle is intentionally leaked: timeBeginPeriod must persist
    // for process lifetime; FreeLibrary would restore the default.
    if (!g_timerSet) {
        g_timerSet = true;
        typedef MMRESULT (WINAPI *timeBeginPeriod_t)(UINT);
        HMODULE hWinmm = LoadLibraryW(L"winmm.dll");
        if (hWinmm) {
            auto pBegin = (timeBeginPeriod_t)GetProcAddress(hWinmm, "timeBeginPeriod");
            if (pBegin) {
                MMRESULT r = pBegin(1);
                if (r == 0) {
                    LOG("Timer resolution -> 1ms");
                } else {
                    LOG("timeBeginPeriod(1) failed code=%u", r);
                }
            }
        }
    }

    LOG("DeviceHooks::Install: vtable patched (cache hooks=%s)",
        IniConfig::g_cfg.Enabled ? "ON" : "OFF");
    LOG("  Frame timing: QPC freq = %lld ticks/sec", g_ft.qpcFreq.QuadPart);
    return true;
}

void Uninstall()
{
    if (!g_vtable) return;

    constexpr SIZE_T kVtableBytes = 512;
    DWORD oldProtect = 0;
    if (VirtualProtect(g_vtable, kVtableBytes, PAGE_READWRITE, &oldProtect)) {
        if (g_orig_TCL)       g_vtable[kIdx_TestCooperativeLevel]     = (void*)g_orig_TCL;
        if (g_orig_GATM)      g_vtable[kIdx_GetAvailableTextureMem]   = (void*)g_orig_GATM;
        if (g_orig_Reset)     g_vtable[kIdx_Reset]                    = (void*)g_orig_Reset;
        if (g_orig_Present)   g_vtable[kIdx_Present]                  = (void*)g_orig_Present;
        if (g_orig_CreateTex) g_vtable[kIdx_CreateTexture]            = (void*)g_orig_CreateTex;
        if (g_orig_CreateVB)  g_vtable[kIdx_CreateVertexBuffer]       = (void*)g_orig_CreateVB;
        if (g_orig_CreateIB)  g_vtable[kIdx_CreateIndexBuffer]        = (void*)g_orig_CreateIB;
        if (g_orig_SetMat)    g_vtable[kIdx_SetMaterial]              = (void*)g_orig_SetMat;
        if (g_orig_SRS)       g_vtable[kIdx_SetRenderState]           = (void*)g_orig_SRS;
        if (g_orig_CSB)       g_vtable[kIdx_CreateStateBlock]         = (void*)g_orig_CSB;
        if (g_orig_BSB)       g_vtable[kIdx_BeginStateBlock]          = (void*)g_orig_BSB;
        if (g_orig_ESB)       g_vtable[kIdx_EndStateBlock]            = (void*)g_orig_ESB;
        // SetRenderTarget never installed - skip
        if (g_orig_Xfm)       g_vtable[kIdx_SetTransform]             = (void*)g_orig_Xfm;
        if (g_orig_VP)        g_vtable[kIdx_SetViewport]              = (void*)g_orig_VP;
        // SetClipPlane never installed in v16 - skip
        if (g_orig_ST)        g_vtable[kIdx_SetTexture]               = (void*)g_orig_ST;
        if (g_orig_TSS)       g_vtable[kIdx_SetTextureStageState]     = (void*)g_orig_TSS;
        if (g_orig_SS)        g_vtable[kIdx_SetSamplerState]          = (void*)g_orig_SS;
        if (g_orig_Scissor)   g_vtable[kIdx_SetScissorRect]           = (void*)g_orig_Scissor;
        if (g_orig_VDecl)     g_vtable[kIdx_SetVertexDeclaration]     = (void*)g_orig_VDecl;
        if (g_orig_VShader)   g_vtable[kIdx_SetVertexShader]          = (void*)g_orig_VShader;
        if (g_orig_SVS)       g_vtable[kIdx_SetVertexShaderConstantF] = (void*)g_orig_SVS;
        if (g_orig_SVSI)      g_vtable[kIdx_SetVertexShaderConstantI] = (void*)g_orig_SVSI;
        if (g_orig_SVSB)      g_vtable[kIdx_SetVertexShaderConstantB] = (void*)g_orig_SVSB;
        if (g_orig_Strm)      g_vtable[kIdx_SetStreamSource]          = (void*)g_orig_Strm;
        if (g_orig_Idx)       g_vtable[kIdx_SetIndices]               = (void*)g_orig_Idx;
        if (g_orig_PShader)   g_vtable[kIdx_SetPixelShader]           = (void*)g_orig_PShader;
        if (g_orig_SPS)       g_vtable[kIdx_SetPixelShaderConstantF]  = (void*)g_orig_SPS;
        if (g_orig_SPSI)      g_vtable[kIdx_SetPixelShaderConstantI]  = (void*)g_orig_SPSI;
        if (g_orig_SPSB)      g_vtable[kIdx_SetPixelShaderConstantB]  = (void*)g_orig_SPSB;
        DWORD tmp;
        VirtualProtect(g_vtable, kVtableBytes, oldProtect, &tmp);
    }
    g_device       = nullptr;
    g_vtable       = nullptr;
    g_orig_TCL       = nullptr;
    g_orig_GATM      = nullptr;
    g_orig_Reset     = nullptr;
    g_orig_Present   = nullptr;
    g_orig_CreateTex = nullptr;
    g_orig_CreateVB  = nullptr;
    g_orig_CreateIB  = nullptr;
    g_orig_SetMat    = nullptr;
    g_orig_SRS       = nullptr;
    g_orig_CSB       = nullptr;
    g_orig_BSB       = nullptr;
    g_orig_ESB       = nullptr;
    g_orig_RT        = nullptr;
    g_orig_Xfm       = nullptr;
    g_orig_VP        = nullptr;
    g_orig_ClipP     = nullptr;
    g_orig_ST        = nullptr;
    g_orig_TSS       = nullptr;
    g_orig_SS        = nullptr;
    g_orig_Scissor   = nullptr;
    g_orig_VDecl     = nullptr;
    g_orig_VShader   = nullptr;
    g_orig_SVS       = nullptr;
    g_orig_SVSI      = nullptr;
    g_orig_SVSB      = nullptr;
    g_orig_Strm      = nullptr;
    g_orig_Idx       = nullptr;
    g_orig_PShader   = nullptr;
    g_orig_SPS       = nullptr;
    g_orig_SPSI      = nullptr;
    g_orig_SPSB      = nullptr;
}

void InvalidateAll()
{
    memset(g_rsCache,      0, sizeof(g_rsCache));
    memset(g_textureCache, 0, sizeof(g_textureCache));
    memset(g_tssValid,     0, sizeof(g_tssValid));
    memset(g_ssCache,      0, sizeof(g_ssCache));
    memset(g_streamCache,  0, sizeof(g_streamCache));
    memset(g_xfmCache,     0, sizeof(g_xfmCache));
    memset(g_rtCache,      0, sizeof(g_rtCache));
    memset(&g_vpCache,     0, sizeof(g_vpCache));
    memset(&g_scissorCache, 0, sizeof(g_scissorCache));
    memset(g_vsiValid,     0, sizeof(g_vsiValid));
    memset(g_psiValid,     0, sizeof(g_psiValid));
    memset(g_vsbValid,     0, sizeof(g_vsbValid));
    memset(g_psbValid,     0, sizeof(g_psbValid));
    g_matValid      = 0;
    
    EngineCacheHooks::InvalidateAll();
}

void LogStats()
{
    // Snapshot then reset (race-tolerant - we lose at most one frame
    // sample if a Present runs between snapshot and reset).
    uint32_t  samples   = g_ft.sampleCount;
    if (samples == 0) return;

    uint64_t  sumUs     = g_ft.sumUs;
    uint64_t  minUs     = g_ft.minUs;
    uint64_t  maxUs     = g_ft.maxUs;
    uint32_t  spikes    = g_ft.spikeCount;
    uint32_t  spikesLogged = g_ft.spikesLoggedThisWindow;
    uint32_t  hist[kFrameHistBuckets];
    for (uint32_t i = 0; i < kFrameHistBuckets; ++i) hist[i] = g_ft.histogram[i];

    // Reset for next window
    g_ft.sumUs       = 0;
    g_ft.minUs       = 0;
    g_ft.maxUs       = 0;
    g_ft.sampleCount = 0;
    g_ft.spikeCount  = 0;
    g_ft.spikesLoggedThisWindow = 0;
    for (uint32_t i = 0; i < kFrameHistBuckets; ++i) g_ft.histogram[i] = 0;

    // Tag every window with the master switch state. Makes mod-on vs
    // mod-off comparison unambiguous when reading the log later.
    const char* mode = IniConfig::g_cfg.Enabled
                       ? "MOD-ON" : "MOD-OFF";

    // v39: per-window position tracking. Skyrim is known to have
    // session-wide performance decay - a cold boot run gives ~110 fps
    // on the test scene, but the same scene after multiple save reloads
    // can drop to ~85 fps. The decay lives in subsystems (save loader,
    // Papyrus VM, possibly the engine's own allocator) that our hooks
    // don't touch, so we can't fix it - but we can MAKE IT VISIBLE.
    //
    // The window number plus uptime tells you, when comparing two
    // logs, whether the comparison is fair:
    //   Window 1 of one log compared to window 1 of another  = fair
    //   Window 1 of one log compared to window 5 of another  = unfair
    //                                                          (decay
    //                                                          would
    //                                                          dominate)
    //
    // Within a single log, if avg fps drops noticeably from window 1
    // to window 5, that is the decay symptom showing up directly.
    static uint32_t  s_windowCount = 0;
    static ULONGLONG s_firstWindowMs = 0;
    ULONGLONG nowMs = GetTickCount64();
    if (s_firstWindowMs == 0) s_firstWindowMs = nowMs;
    ++s_windowCount;
    uint64_t uptimeSec = (nowMs - s_firstWindowMs) / 1000;

    uint64_t avgUs = sumUs / samples;
    LOG("--- Frame timing [%s] (window #%u, uptime %llus) ---",
        mode, s_windowCount, (unsigned long long)uptimeSec);
    LOG("  Samples: %u  Avg: %llu us (%llu fps)  Min: %llu us  Max: %llu us",
        samples, (unsigned long long)avgUs,
        avgUs ? 1000000ULL / avgUs : 0,
        (unsigned long long)minUs, (unsigned long long)maxUs);

    // Compute percentiles from histogram
    uint64_t cum = 0;
    uint64_t target50 = samples / 2;
    uint64_t target95 = (samples * 95) / 100;
    uint64_t target99 = (samples * 99) / 100;
    uint64_t p50 = 0, p95 = 0, p99 = 0;
    for (uint32_t i = 0; i < kFrameHistBuckets; ++i) {
        cum += hist[i];
        if (p50 == 0 && cum >= target50) p50 = (uint64_t)i * kFrameHistBucketUs;
        if (p95 == 0 && cum >= target95) p95 = (uint64_t)i * kFrameHistBucketUs;
        if (p99 == 0 && cum >= target99) p99 = (uint64_t)i * kFrameHistBucketUs;
    }
    LOG("  p50: %llu us (%llu fps)  p95: %llu us (%llu fps)  p99: %llu us (%llu fps)",
        (unsigned long long)p50, p50 ? 1000000ULL / p50 : 0,
        (unsigned long long)p95, p95 ? 1000000ULL / p95 : 0,
        (unsigned long long)p99, p99 ? 1000000ULL / p99 : 0);
    LOG("  Spikes (>2x avg): %u  (%.2f%% of frames)",
        spikes, samples ? 100.0 * spikes / samples : 0.0);

    // v37: report SPIKE event log cap usage so user knows if they
    // need to bump MaxSpikesPerWindow for full visibility.
    if (IniConfig::g_cfg.StutterThresholdMs > 0) {
        bool capHit = spikesLogged >= (uint32_t)IniConfig::g_cfg.MaxSpikesPerWindow;
        LOG("  Spike events logged: %u%s (threshold=%dms)",
            spikesLogged,
            capHit ? " (CAP HIT - increase [Profiling] MaxSpikesPerWindow)" : "",
            IniConfig::g_cfg.StutterThresholdMs);
    }

    // v39: dump the frame-time histogram so anomalies in the
    // percentiles are diagnosable from the log alone. The histogram is
    // 32 buckets x 3.125 ms each. When most frames are sub-3.125ms,
    // bucket 0 dominates and the percentile calculator above returns
    // 0 - which we display as "p50: 0 us / 0 fps" and which can also
    // collapse to "1 * bucketWidth" for slightly higher percentiles
    // (the source of the suspicious "320 fps p50" we saw in earlier logs).
    //
    // Format: "bucket_index: count" only for buckets with count > 0,
    // joined into one line. Plus an explicit warning if bucket 0 is
    // huge - that's our cue that the histogram is under-resolved.
    {
        char line[1024] = {};
        int  off = 0;
        int  written = _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE,
                                   "  Histogram (bucket=us-range:count):");
        if (written > 0) off += written;
        for (uint32_t i = 0; i < kFrameHistBuckets; ++i) {
            if (hist[i] == 0) continue;
            uint64_t lo = (uint64_t)i * kFrameHistBucketUs;
            uint64_t hi = lo + kFrameHistBucketUs - 1;
            int w = _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE,
                                " [%llu-%llu]:%u",
                                (unsigned long long)lo,
                                (unsigned long long)hi,
                                hist[i]);
            if (w <= 0) break;
            off += w;
            if (off >= (int)sizeof(line) - 32) break;
        }
        LOG("%s", line);

        // Diagnostic flag: if more than half the frames fall in
        // bucket 0 (i.e. < 3.125 ms each), p50 reads as 0 us and
        // appears as "infinity fps" or as the bucket-1 boundary
        // depending on rounding. This is the artifact that made
        // mod-off look like 320 fps median in earlier tests.
        if (samples > 0 && hist[0] * 2 > samples) {
            LOG("  WARNING: %u/%u samples (%.1f%%) fell in histogram bucket 0 "
                "(< %llu us). Percentile values for this window are unreliable - "
                "the histogram cannot resolve frame times below %llu us.",
                hist[0], samples, 100.0 * hist[0] / samples,
                (unsigned long long)kFrameHistBucketUs,
                (unsigned long long)kFrameHistBucketUs);
        }
    }

    // State change cache statistics (snapshot and reset per window)
    uint32_t sc = g_stateChanges.load(std::memory_order_relaxed);
    uint32_t ss = g_stateChangesSkipped.load(std::memory_order_relaxed);
    g_stateChanges.store(0, std::memory_order_relaxed);
    g_stateChangesSkipped.store(0, std::memory_order_relaxed);
    if (sc > 0 || ss > 0) {
        uint32_t total = sc + ss;
        LOG("  State changes: %u total, %u skipped (%.1f%% cache hit rate)",
            total, ss, total ? 100.0 * ss / total : 0.0);
    }

    // Texture cache statistics (snapshot and reset per window)
    uint32_t tHits = g_textureCacheHits.load(std::memory_order_relaxed);
    uint32_t tMisses = g_textureCacheMisses.load(std::memory_order_relaxed);
    g_textureCacheHits.store(0, std::memory_order_relaxed);
    g_textureCacheMisses.store(0, std::memory_order_relaxed);
    if (tHits > 0 || tMisses > 0) {
        uint32_t total = tHits + tMisses;
        LOG("  Texture cache: %u hits, %u misses (%.1f%% hit rate)",
            tHits, tMisses, total ? 100.0 * tHits / total : 0.0);
    }
}

} // namespace DeviceHooks

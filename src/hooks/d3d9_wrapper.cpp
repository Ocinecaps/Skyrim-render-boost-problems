//
// d3d9_wrapper.cpp - Minimal IDirect3D9 wrapper.
//
// All real action happens elsewhere now:
//   - device_hooks.cpp: vtable patches on the real IDirect3DDevice9
//   - engine_cache_hooks.cpp: engine-side dispatcher caching
//
// This file just intercepts CreateDevice so we know when the device
// exists, then installs vtable hooks on it.
//

#include "d3d9_wrapper.h"
#include "Common.h"
#include "device_hooks.h"
#include "ini_config.h"

// Initialize IID_IDirect3D9 to fix linker error
const IID IID_IDirect3D9 = {0x0D022269,0x637D,0x11D2,{0xB9,0x72,0x00,0x00,0xF8,0x75,0x24,0x32}};

HookedD3D9::~HookedD3D9()
{
    LOG("HookedD3D9: destroyed");
    if (m_inner) m_inner->Release();
}

HRESULT STDMETHODCALLTYPE HookedD3D9::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3D9) {
        *ppv = static_cast<IDirect3D9*>(this);
        AddRef();
        return S_OK;
    }
    return m_inner->QueryInterface(riid, ppv);
}

ULONG STDMETHODCALLTYPE HookedD3D9::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_refCount);
}

ULONG STDMETHODCALLTYPE HookedD3D9::Release()
{
    LONG n = InterlockedDecrement(&m_refCount);
    if (n == 0) { delete this; return 0; }
    return (ULONG)n;
}

HRESULT STDMETHODCALLTYPE HookedD3D9::RegisterSoftwareDevice(void* p)                                  { return m_inner->RegisterSoftwareDevice(p); }
UINT    STDMETHODCALLTYPE HookedD3D9::GetAdapterCount()                                                { return m_inner->GetAdapterCount(); }
HRESULT STDMETHODCALLTYPE HookedD3D9::GetAdapterIdentifier(UINT a, DWORD f, D3DADAPTER_IDENTIFIER9* p) { return m_inner->GetAdapterIdentifier(a, f, p); }
UINT    STDMETHODCALLTYPE HookedD3D9::GetAdapterModeCount(UINT a, D3DFORMAT f)                          { return m_inner->GetAdapterModeCount(a, f); }
HRESULT STDMETHODCALLTYPE HookedD3D9::EnumAdapterModes(UINT a, D3DFORMAT f, UINT i, D3DDISPLAYMODE* p)  { return m_inner->EnumAdapterModes(a, f, i, p); }
HRESULT STDMETHODCALLTYPE HookedD3D9::GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* p)                  { return m_inner->GetAdapterDisplayMode(a, p); }
HRESULT STDMETHODCALLTYPE HookedD3D9::CheckDeviceType(UINT a, D3DDEVTYPE c, D3DFORMAT d, D3DFORMAT b, BOOL w) { return m_inner->CheckDeviceType(a, c, d, b, w); }
HRESULT STDMETHODCALLTYPE HookedD3D9::CheckDeviceFormat(UINT a, D3DDEVTYPE d, D3DFORMAT f, DWORD u, D3DRESOURCETYPE r, D3DFORMAT cf) { return m_inner->CheckDeviceFormat(a, d, f, u, r, cf); }
HRESULT STDMETHODCALLTYPE HookedD3D9::CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE d, D3DFORMAT f, BOOL w, D3DMULTISAMPLE_TYPE m, DWORD* q) { return m_inner->CheckDeviceMultiSampleType(a, d, f, w, m, q); }
HRESULT STDMETHODCALLTYPE HookedD3D9::CheckDepthStencilMatch(UINT a, D3DDEVTYPE d, D3DFORMAT af, D3DFORMAT rtf, D3DFORMAT dsf) { return m_inner->CheckDepthStencilMatch(a, d, af, rtf, dsf); }
HRESULT STDMETHODCALLTYPE HookedD3D9::CheckDeviceFormatConversion(UINT a, D3DDEVTYPE d, D3DFORMAT sf, D3DFORMAT tf) { return m_inner->CheckDeviceFormatConversion(a, d, sf, tf); }
HRESULT STDMETHODCALLTYPE HookedD3D9::GetDeviceCaps(UINT a, D3DDEVTYPE d, D3DCAPS9* c)                  { return m_inner->GetDeviceCaps(a, d, c); }
HMONITOR STDMETHODCALLTYPE HookedD3D9::GetAdapterMonitor(UINT a)                                       { return m_inner->GetAdapterMonitor(a); }

HRESULT STDMETHODCALLTYPE HookedD3D9::CreateDevice(UINT a, D3DDEVTYPE d, HWND f, DWORD bf,
                                                    D3DPRESENT_PARAMETERS* pp,
                                                    IDirect3DDevice9** ppd)
{
    LOG("HookedD3D9::CreateDevice called: adapter=%u type=%d hwnd=%p flags=0x%08X",
        a, (int)d, (void*)f, bf);
    if (pp) {
        LOG("  BackBuffer %ux%u format=%d, Windowed=%d, SwapEffect=%d, MultiSample=%d",
            pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat,
            pp->Windowed, pp->SwapEffect, pp->MultiSampleType);
    }

    IDirect3DDevice9* realDevice = nullptr;
    HRESULT hr = m_inner->CreateDevice(a, d, f, bf, pp, &realDevice);
    if (FAILED(hr)) {
        LOG("HookedD3D9::CreateDevice: inner failed hr=0x%08X", hr);
        return hr;
    }

    // Patch the real device's vtable. This is the key v10 change:
    // we DON'T return a wrapped device; we return the REAL device,
    // but with 6 of its vtable entries pointing to our hook functions.
    if (!DeviceHooks::Install(realDevice)) {
        LOG("HookedD3D9::CreateDevice: DeviceHooks::Install FAILED - returning unhooked device");
    }

    
    *ppd = realDevice;
    LOG("HookedD3D9::CreateDevice: returning real device %p (vtable patched)", realDevice);
    return hr;
}

#pragma once
//
// d3d9_wrapper.h - Minimal IDirect3D9 wrapper.
//
// Compared to v9: the entire HookedD3DDevice9 class (132 forwarder
// methods, ~400 lines) is gone. Only the small HookedD3D9 wrapper
// remains, and only to intercept CreateDevice. The real device is
// patched via direct vtable hooks (see device_hooks.h) and returned
// to Skyrim unwrapped.
//
// IDirect3D9 itself has only 16 methods, all of which are infrequent
// at runtime (called once at startup). So even though we wrap them,
// the per-call overhead doesn't accumulate.
//

#include <windows.h>
#include <d3d9.h>
#include <Unknwn.h>

class HookedD3D9 : public IDirect3D9 {
public:
    explicit HookedD3D9(IDirect3D9* inner) : m_refCount(1), m_inner(inner) {}
    virtual ~HookedD3D9();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;

    // IDirect3D9
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override;
    UINT    STDMETHODCALLTYPE GetAdapterCount() override;
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a, DWORD f, D3DADAPTER_IDENTIFIER9* p) override;
    UINT    STDMETHODCALLTYPE GetAdapterModeCount(UINT a, D3DFORMAT f) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT a, D3DFORMAT f, UINT i, D3DDISPLAYMODE* p) override;
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* p) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT a, D3DDEVTYPE c, D3DFORMAT d, D3DFORMAT b, BOOL w) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a, D3DDEVTYPE d, D3DFORMAT f, DWORD u, D3DRESOURCETYPE r, D3DFORMAT cf) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE d, D3DFORMAT f, BOOL w, D3DMULTISAMPLE_TYPE m, DWORD* q) override;
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT a, D3DDEVTYPE d, D3DFORMAT af, D3DFORMAT rtf, D3DFORMAT dsf) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT a, D3DDEVTYPE d, D3DFORMAT sf, D3DFORMAT tf) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a, D3DDEVTYPE d, D3DCAPS9* c) override;
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT a) override;
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT a, D3DDEVTYPE d, HWND f, DWORD bf,
                                            D3DPRESENT_PARAMETERS* pp,
                                            IDirect3DDevice9** ppd) override;

private:
    LONG          m_refCount;
    IDirect3D9*   m_inner;
};

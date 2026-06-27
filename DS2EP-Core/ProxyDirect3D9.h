#pragma once
#include <d3d9.h>
#include "ProxyDirect3DDevice9.h"

// =============================================================================
//  ProxyDirect3D9  —  Wraps the IDirect3D9 factory object
//
//  The only call we actually care about is CreateDevice — that is where we
//  slip our ProxyDirect3DDevice9 wrapper in instead of the real device.
//  Every other method is a pure passthrough so the game behaves normally.
//
//  Note: DS2 calls Direct3DCreate9 once but may call CreateDevice multiple
//  times (e.g. across scene transitions).  Each ProxyDirect3DDevice9
//  constructed here calls Overlay::Init() which safely tears down any
//  previous ImGui context before re-initialising.  See Overlay.h.
// =============================================================================

class ProxyDirect3D9 : public IDirect3D9
{
private:
    IDirect3D9* originalD3D;
    ULONG        refCount;

public:
    ProxyDirect3D9(IDirect3D9* pOriginal) : originalD3D(pOriginal), refCount(1) {}

    // =========================================================================
    //  IUnknown
    // =========================================================================
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override {
        return originalD3D->QueryInterface(riid, ppvObj);
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refCount;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = --refCount;
        if (count == 0) {
            originalD3D->Release();
            delete this;
        }
        return count;
    }

    // =========================================================================
    //  IDirect3D9 — Adapter enumeration (all passthroughs)
    // =========================================================================
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override {
        return originalD3D->RegisterSoftwareDevice(pInitializeFunction);
    }
    UINT STDMETHODCALLTYPE GetAdapterCount() override {
        return originalD3D->GetAdapterCount();
    }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(
        UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override {
        return originalD3D->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
    }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override {
        return originalD3D->GetAdapterModeCount(Adapter, Format);
    }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(
        UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override {
        return originalD3D->EnumAdapterModes(Adapter, Format, Mode, pMode);
    }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(
        UINT Adapter, D3DDISPLAYMODE* pMode) override {
        return originalD3D->GetAdapterDisplayMode(Adapter, pMode);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(
        UINT Adapter, D3DDEVTYPE DevType,
        D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat,
        BOOL bWindowed) override {
        return originalD3D->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(
        UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat,
        DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override {
        return originalD3D->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(
        UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat,
        BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType,
        DWORD* pQualityLevels) override {
        return originalD3D->CheckDeviceMultiSampleType(
            Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
    }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(
        UINT Adapter, D3DDEVTYPE DeviceType,
        D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat,
        D3DFORMAT DepthStencilFormat) override {
        return originalD3D->CheckDepthStencilMatch(
            Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(
        UINT Adapter, D3DDEVTYPE DeviceType,
        D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override {
        return originalD3D->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
    }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(
        UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override {
        return originalD3D->GetDeviceCaps(Adapter, DeviceType, pCaps);
    }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override {
        return originalD3D->GetAdapterMonitor(Adapter);
    }

    // =========================================================================
    //  THE intercept — swap the real device for our proxy
    // =========================================================================
    HRESULT STDMETHODCALLTYPE CreateDevice(
        UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
        DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
        IDirect3DDevice9** ppReturnedDeviceInterface) override
    {
        IDirect3DDevice9* realDevice = nullptr;

        HRESULT hr = originalD3D->CreateDevice(
            Adapter, DeviceType, hFocusWindow,
            BehaviorFlags, pPresentationParameters, &realDevice);

        if (SUCCEEDED(hr) && realDevice) {
            // ProxyDirect3DDevice9::ctor calls Overlay::Init which safely
            // tears down any previous ImGui context before building a new one.
            *ppReturnedDeviceInterface = new ProxyDirect3DDevice9(realDevice, hFocusWindow);
        }
        return hr;
    }
};

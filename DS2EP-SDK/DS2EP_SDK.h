#pragma once
// =============================================================================
//  DS2EP_SDK.h  —  THE single authoritative interface definition.
//
//  Included by BOTH:
//    - DS2EP-Core (via DS2EPPlugin.h which just #includes this)
//    - Plugin DLLs (directly)
//
//  NEVER duplicate or reorder the virtual functions in this struct.
//  The vtable order is the ABI contract between core and plugins.
//  Adding new virtuals must always go at the END, before Destroy().
// =============================================================================
#include <d3d9.h>
#include <windows.h>
// ImGuiContext is needed for OnDrawMenu signature.
// The imgui folder path must be in your plugin project's include dirs.
#include "imgui.h"

struct DS2EPPluginInfo
{
    const char* name;
    const char* version;
    const char* description;
};

// =============================================================================
//  IDS2EPPlugin
//  VTABLE ORDER IS ABI — do not reorder, only append before Destroy()
// =============================================================================
struct IDS2EPPlugin
{
    // [0] destructor
    virtual ~IDS2EPPlugin() = default;

    // [1]
    virtual DS2EPPluginInfo GetInfo() = 0;

    // [2-5] Device lifecycle
    virtual void OnDeviceCreate (IDirect3DDevice9* dev) {}
    virtual void OnDeviceLost   (IDirect3DDevice9* dev) {}
    virtual void OnDeviceReset  (IDirect3DDevice9* dev) {}
    virtual void OnDeviceDestroy(IDirect3DDevice9* dev) {}

    // [6-8] Per-frame
    virtual void OnBeginScene(IDirect3DDevice9* dev) {}
    virtual void OnEndScene  (IDirect3DDevice9* dev) {}
    virtual void OnPresent   (IDirect3DDevice9* dev) {}

    // [9-12] Intercepts — return true to BLOCK (value/mat is writable)
    virtual bool OnSetRenderState  (IDirect3DDevice9* dev,
                                     D3DRENDERSTATETYPE state, DWORD& value)                { return false; }
    virtual bool OnSetSamplerState (IDirect3DDevice9* dev,
                                     DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD& value) { return false; }
    virtual bool OnSetTransform    (IDirect3DDevice9* dev,
                                     D3DTRANSFORMSTATETYPE state, D3DMATRIX& mat)           { return false; }
    virtual bool OnCreateTexture   (IDirect3DDevice9* dev,
                                     UINT w, UINT h, UINT levels, DWORD usage,
                                     D3DFORMAT fmt, D3DPOOL pool,
                                     IDirect3DTexture9** ppTex)                             { return false; }

    // [13] ImGui menu panel — context MUST be set before calling ImGui functions.
    // The core passes its ImGuiContext*. Call ImGui::SetCurrentContext(ctx) first.
    virtual void OnDrawMenu(ImGuiContext* ctx) {}

    // [14-15] Settings persistence
    virtual void OnSaveSettings(const char* iniPath) {}
    virtual void OnLoadSettings(const char* iniPath) {}

    // [16] ALWAYS LAST — append new virtuals above this line.
    //
    // IMPORTANT: Implement Destroy() as `delete this;`
    // The core calls Destroy() instead of `delete ptr` to avoid cross-DLL
    // heap corruption (each DLL has its own CRT heap in MSVC).
    // Your plugin object must free itself from its own heap.
    virtual void Destroy() {}
};

// Every plugin DLL must export exactly this symbol:
extern "C" __declspec(dllexport) IDS2EPPlugin* DS2EP_CreatePlugin();

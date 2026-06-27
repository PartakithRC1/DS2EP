# DS2 Enhancement Proxy — Plugin Development Guide

Welcome! DS2EP is a D3D9 proxy DLL for Dungeon Siege 2 that intercepts the
game's graphics calls and lets you enhance or modify rendering without touching
the game's executable. This guide explains how to write your own plugin.
Do not worry if having code to look at makes it easier, Antialising & View Dist-
ance plugins are both included and can be gone through to learn from.

---

## What a Plugin Can Do

Because every D3D9 call DS2 makes flows through the proxy, your plugin can:

- **Modify rendering state** — change fog, lighting, blending, culling, anything
- **Intercept draw calls** — inspect or redirect vertex/index buffer draws  
- **Post-process effects** — grab the backbuffer after DS2 draws, run your own
  passes (bloom, DoF, color grading, SSAO), and write results back
- **Upscale resources** — substitute higher-resolution textures or render targets
- **Add an in-game UI** — draw ImGui panels that appear in the DS2EP menu
- **Save per-user settings** — read and write your own `.ini` file automatically

---

## Prerequisites

- **Visual Studio 2026** (Community edition is free)
- **DirectX SDK** — the Windows 10/11 SDK includes D3D9 headers and `d3d9.lib`
- **32-bit (Win32) compilation** — DS2 is a 32-bit process, your DLL must match
- A copy of the DS2EP source (clone from GitHub (This project!))

---

## Project Setup

### 1. Folder structure

```
DS2EP/
  DS2EP-Core/           ← the proxy DLL (d3d9.dll)
    imgui/              ← ImGui source lives here
    DS2EP-Plugins/      ← your compiled .dll files go here at runtime
  DS2EP-SDK/
    DS2EP_SDK.h         ← the ONLY header your plugin needs
  DS2EP-Plugin-MyMod/   ← your new plugin project
    DS2EP-Plugin-MyMod.vcxproj
    MyMod.cpp
  DS2EP.sln
```

### 2. Create a new VS project

1. In Visual Studio: **File → Add → New Project**
2. Choose **Dynamic-Link Library (DLL)** — C++
3. Set platform to **Win32** (not x64)
4. Name it `DS2EP-Plugin-YourName`

### 3. Configure the project

In project **Properties** (make sure platform is **Win32**):

**General:**
- Output Directory: `$(SolutionDir)DS2EP-Core\DS2EP-Plugins\`
- Target Name: `yourpluginname` (lowercase, no spaces — this becomes the `.dll` filename)
- Target Extension: `.dll`

**C/C++ → General → Additional Include Directories:**
```
$(SolutionDir)DS2EP-SDK;$(SolutionDir)DS2EP-Core\imgui;%(AdditionalIncludeDirectories)
```

**C/C++ → Code Generation → Runtime Library:**
- Debug:   `Multi-threaded Debug (/MTd)`
- Release: `Multi-threaded (/MT)`

> ⚠️ **Must use /MT or /MTd** — not the DLL variants (/MD, /MDd).
> Each plugin must carry its own CRT to avoid heap corruption across DLL boundaries.

**C/C++ → Language → C++ Language Standard:** `ISO C++17`

**Linker → Input → Additional Dependencies:** `d3d9.lib`

**Linker → Advanced:** No module definition file needed.

---

## Writing Your First Plugin

Create `MyMod.cpp` in your project:

```cpp
#include "DS2EP_SDK.h"   // the only DS2EP header you need
#include "imgui.h"       // for OnDrawMenu

class MyPlugin : public IDS2EPPlugin
{
public:
    // ── Identity ──────────────────────────────────────────────────────────────
    DS2EPPluginInfo GetInfo() override {
        return {
            "My Plugin",      // display name in the DS2EP menu
            "1.0",            // version string
            "Does cool stuff" // short description shown in the menu
        };
    }

    // ── Device lifecycle ──────────────────────────────────────────────────────
    void OnDeviceCreate(IDirect3DDevice9* dev) override
    {
        m_dev = dev;
        // Create your D3D resources here (textures, shaders, render targets)
        // Use D3DPOOL_DEFAULT for render targets and dynamic resources
    }

    void OnDeviceLost(IDirect3DDevice9* dev) override
    {
        // REQUIRED if you have D3DPOOL_DEFAULT resources:
        // Release them here or Reset() will fail.
        // D3DPOOL_MANAGED resources survive Reset automatically.
    }

    void OnDeviceReset(IDirect3DDevice9* dev) override
    {
        m_dev = dev;
        // Recreate your D3DPOOL_DEFAULT resources here.
    }

    void OnDeviceDestroy(IDirect3DDevice9* dev) override
    {
        m_dev = nullptr;
        // Final cleanup — device is about to be released.
    }

    // ── Per-frame callbacks ───────────────────────────────────────────────────
    void OnBeginScene(IDirect3DDevice9* dev) override
    {
        // Called at the start of each frame, after DS2 sets its state.
        // Good place to force render state overrides that DS2 might reset.
        // Example: force wireframe on
        // dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
    }

    void OnEndScene(IDirect3DDevice9* dev) override
    {
        // Called after DS2 has finished drawing the frame.
        // The backbuffer contains the complete scene at this point.
        // This is where you do post-processing passes.
    }

    void OnPresent(IDirect3DDevice9* dev) override
    {
        // Very last callback before the frame is flipped to screen.
        // Good for 2D overlays drawn directly in screen space.
    }

    // ── Intercept callbacks ───────────────────────────────────────────────────
    // Return true to BLOCK the original call. Return false to let it through.
    // You can modify the value/matrix parameter before returning false.

    bool OnSetRenderState(IDirect3DDevice9* dev,
                           D3DRENDERSTATETYPE state, DWORD& value) override
    {
        // Example: always force anisotropic filtering
        // if (state == D3DRS_SPECULARENABLE) { value = TRUE; return false; }
        return false;
    }

    bool OnSetSamplerState(IDirect3DDevice9* dev,
                            DWORD sampler, D3DSAMPLERSTATETYPE type,
                            DWORD& value) override
    {
        return false;
    }

    bool OnSetTransform(IDirect3DDevice9* dev,
                         D3DTRANSFORMSTATETYPE state, D3DMATRIX& mat) override
    {
        // mat is writable — modify it before returning false to pass through
        return false;
    }

    bool OnCreateTexture(IDirect3DDevice9* dev,
                          UINT w, UINT h, UINT levels, DWORD usage,
                          D3DFORMAT fmt, D3DPOOL pool,
                          IDirect3DTexture9** ppTex) override
    {
        // Return true + set *ppTex if you want to substitute your own texture.
        return false;
    }

    // ── ImGui menu panel ──────────────────────────────────────────────────────
    void OnDrawMenu(ImGuiContext* ctx) override
    {
        // ALWAYS call this first — syncs the shared ImGui context
        ImGui::SetCurrentContext(ctx);

        // Now draw your controls normally
        ImGui::Checkbox("Enable my feature", &m_enabled);

        if (m_enabled) {
            ImGui::SetNextItemWidth(200);
            ImGui::SliderFloat("My Setting", &m_myValue, 0.f, 10.f, "%.2f");
        }

        // Save when something changed
        // if (changed && m_iniPath[0]) SaveSettings(m_iniPath);
    }

    // ── Settings ──────────────────────────────────────────────────────────────
    void OnSaveSettings(const char* iniPath) override
    {
        // Use Windows WritePrivateProfileStringA — no CRT file I/O needed
        char buf[32];
        snprintf(buf, sizeof(buf), "%.4f", m_myValue);
        WritePrivateProfileStringA("Settings", "my_value", buf, iniPath);
        WritePrivateProfileStringA("Settings", "enabled",
            m_enabled ? "true" : "false", iniPath);
    }

    void OnLoadSettings(const char* iniPath) override
    {
        strncpy_s(m_iniPath, iniPath, MAX_PATH);

        char buf[32];
        GetPrivateProfileStringA("Settings", "my_value", "1.0", buf, 32, iniPath);
        m_myValue = (float)atof(buf);

        GetPrivateProfileStringA("Settings", "enabled", "true", buf, 32, iniPath);
        m_enabled = (_stricmp(buf, "true") == 0 || strcmp(buf, "1") == 0);
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    void Destroy() override
    {
        // Always self-delete — the core must never call delete on us
        // (cross-DLL heap mismatch would cause debug_heap assertion)
        delete this;
    }

private:
    IDirect3DDevice9* m_dev    = nullptr;
    char   m_iniPath[MAX_PATH] = {};
    bool   m_enabled           = true;
    float  m_myValue           = 1.0f;
};

// ── Required export ────────────────────────────────────────────────────────────
extern "C" __declspec(dllexport) IDS2EPPlugin* DS2EP_CreatePlugin()
{
    return new MyPlugin();
}
```

Drop the compiled `.dll` into `DS2EP-Plugins\` and it loads automatically next time DS2 starts.

---

## Post-Processing Example (Bloom / DoF / Color Grade)

`OnEndScene` is where you do post-FX. Here's the skeleton pattern:

```cpp
void OnEndScene(IDirect3DDevice9* dev) override
{
    if (!m_enabled || !m_pSceneCopy) return;

    // 1. Get DS2's backbuffer (the fully rendered scene)
    IDirect3DSurface9* pBackBuf = nullptr;
    dev->GetRenderTarget(0, &pBackBuf);

    // 2. Copy it to your own render target
    IDirect3DSurface9* pCopySurf = nullptr;
    m_pSceneCopy->GetSurfaceLevel(0, &pCopySurf);
    dev->StretchRect(pBackBuf, nullptr, pCopySurf, nullptr, D3DTEXF_LINEAR);
    pCopySurf->Release();

    // 3. Set your RT as the render target
    dev->SetRenderTarget(0, m_pEffectSurface);

    // 4. Draw a fullscreen quad with your pixel shader
    //    (sample from m_pSceneCopy as input texture)
    DrawFullscreenQuad(dev, m_pSceneCopy, m_pMyShader);

    // 5. Copy result back to backbuffer
    dev->SetRenderTarget(0, pBackBuf);
    dev->StretchRect(m_pEffectSurface, nullptr, pBackBuf, nullptr, D3DTEXF_LINEAR);

    pBackBuf->Release();
}
```

**Pixel shaders** for D3D9 are written in HLSL and compiled to shader model `ps_3_0`.
You can compile them at load time with `D3DXCompileShaderFromFile` (requires D3DX)
or at build time with `fxc.exe` to a `.fxo` binary you load with `D3DXCreateEffect`.

---

## Important Rules

### ✅ Always do this
- Call `ImGui::SetCurrentContext(ctx)` as the **first line** of `OnDrawMenu`
- Implement `Destroy()` as `delete this`
- Release all `D3DPOOL_DEFAULT` resources in `OnDeviceLost`
- Recreate them in `OnDeviceReset`
- Use `/MT` or `/MTd` runtime (not `/MD`)

### ❌ Never do this
- Call `ImGui::NewFrame()`, `ImGui::Render()`, or `ImGui::EndFrame()` — the core handles this
- Call `dev->BeginScene()` or `dev->EndScene()` yourself inside callbacks
- Hold onto the `IDirect3DDevice9*` pointer after `OnDeviceDestroy` fires
- Delete objects across DLL boundaries — always self-delete in `Destroy()`
- Add virtual methods to `IDS2EPPlugin` — only the core maintainer does that
  (vtable order is the ABI contract; adding methods breaks existing plugins)

---

## Vtable Stability Notice

`IDS2EPPlugin` has a fixed vtable. **Never reorder or insert virtual methods.**
New virtuals can only be appended before `Destroy()` by the core maintainer,
and only with a version bump. If you need to call back into the core for
something not in the SDK, open an issue on GitHub and we'll add it properly.

---

## Sharing Your Plugin

1. Build in **Release|Win32**
2. The output `.dll` goes to `DS2EP-Plugins\` automatically
3. Optionally ship a matching `.ini` with default settings
4. Include your source — this is an open project and other DS2 fans will learn from it

If you want it in the main DS2EP Nexus release, open a pull request on GitHub
with your plugin source in a `DS2EP-Plugin-YourName\` folder.

---

## Reference: IDS2EPPlugin Vtable Order

```
[0]  ~IDS2EPPlugin()        destructor
[1]  GetInfo()              identity
[2]  OnDeviceCreate()       device created
[3]  OnDeviceLost()         before Reset() — release POOL_DEFAULT resources
[4]  OnDeviceReset()        after Reset()  — recreate them
[5]  OnDeviceDestroy()      device about to be released
[6]  OnBeginScene()         start of frame
[7]  OnEndScene()           end of frame — do post-FX here
[8]  OnPresent()            just before flip — 2D overlays here
[9]  OnSetRenderState()     intercept render state
[10] OnSetSamplerState()    intercept sampler state
[11] OnSetTransform()       intercept matrix uploads
[12] OnCreateTexture()      intercept texture creation
[13] OnDrawMenu()           ImGui panel (pass ctx first!)
[14] OnSaveSettings()       write your .ini
[15] OnLoadSettings()       read your .ini
[16] Destroy()              self-delete — always `delete this`
```

---

*DS2EP was built by the community (me), for the community (you). Dungeon Siege 2 deserves to look better — lets make it happen.*

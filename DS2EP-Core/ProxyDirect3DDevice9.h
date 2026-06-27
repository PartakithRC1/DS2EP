#pragma once
#include <d3d9.h>
#include <cmath>
#include <windows.h>
#include "Overlay.h"
#include "DS2EPPlugin.h"

// =============================================================================
//  ProxyDirect3DDevice9
//
//  CRITICAL DISCOVERY FROM GHIDRA ANALYSIS:
//  =========================================
//  DS2 does NOT send world-space fog distances to D3D. It normalizes them:
//
//    D3DRS_FOGSTART = fogNearDist / cameraFarDist     (a 0.0-1.0 ratio)
//    D3DRS_FOGEND   = fogFarDist  / cameraFarDist     (a 0.0-1.0 ratio)
//
//  This is why scaling the D3DRS_FOG* values did nothing — they're already
//  fractional (e.g. 0.05, 0.9). Multiplying by 500x sends them way past 1.0
//  which D3D clamps back to 1.0. No visible change.
//
//  The real fog distances live in the scene renderer object:
//    *(float*)( *(*(DAT_00d0158c + 0x24c)) + 0x108c )  = fogNearDist
//    *(float*)( *(*(DAT_00d0158c + 0x24c)) + 0x1090 )  = fogFarDist
//  where DAT_00d0158c = 0x00d0158c (DS2.exe base + offset, absolute VA in 32-bit)
//
//  The projection matrix far-clip IS in world space and works correctly.
//  But DS2 ALSO has a separate "frustumDepth" in MoodSetting (offset 0x1c)
//  which gates object rendering — objects beyond frustumDepth don't get drawn
//  even if the far-clip allows them. This is the primary LOD/streaming gate.
//
//  THE REAL APPROACH:
//  We intercept SetRenderState(FOGSTART/FOGEND) to capture the NORMALIZED values,
//  back-calculate the world-space fog distances, scale those, then re-normalize
//  with our new far-clip so everything is consistent.
//  We ALSO directly write the DS2 fog memory every frame via PushFrameOverrides.
//
//  INTERCEPTED CALLS
//  -----------------
//  SetTransform        — patch projection matrix far-clip (view distance)
//  SetRenderState      — intercept normalized fog, decode and re-encode correctly
//  SetSamplerState     — inject anisotropic filtering on every texture slot
//  SetLight            — scale diffuse/specular/range, override attenuation
//  CreateTexture       — detect shadow-map allocations and upscale them
//  BeginScene          — PushFrameOverrides: re-push all overrides every frame
//  Reset               — OnLostDevice / OnResetDevice lifecycle
//  Release             — full Overlay::Shutdown when refcount hits 0
//  EndScene / Present  — drive the ImGui overlay
// =============================================================================

// =============================================================================
//  DS2MemPatch  —  Direct memory access into DS2's renderer state
//
//  From Ghidra analysis:
//    0x00d0158c = pointer to world/scene manager
//    [sceneManager + 0x24c] = pointer to renderer object
//    [renderer + 0x108c]    = float fogNearDist  (world units)
//    [renderer + 0x1090]    = float fogFarDist   (world units)
//
//  We read these to know DS2's intended fog range, scale them, write them back.
//  The game then normalizes them itself before uploading to D3D.
// =============================================================================
namespace DS2Mem
{
    // Absolute virtual addresses from Ghidra (DS2 32-bit, no ASLR)
    static constexpr uintptr_t SCENE_MGR_PTR = 0x00d0158c;  // int* -> scene manager
    static constexpr uintptr_t RENDERER_OFFS  = 0x24c;       // scene manager -> renderer ptr
    static constexpr uintptr_t FOG_NEAR_OFFS  = 0x108c;      // renderer -> float fogNear
    static constexpr uintptr_t FOG_FAR_OFFS   = 0x1090;      // renderer -> float fogFar

    // Safe multi-level pointer dereference. Returns nullptr on any bad step.
    inline float* GetFogFloat(uintptr_t fieldOffset)
    {
        __try
        {
            uintptr_t sceneMgr = *reinterpret_cast<uintptr_t*>(SCENE_MGR_PTR);
            if (!sceneMgr) return nullptr;
            uintptr_t renderer = *reinterpret_cast<uintptr_t*>(sceneMgr + RENDERER_OFFS);
            if (!renderer) return nullptr;
            return reinterpret_cast<float*>(renderer + fieldOffset);
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    inline float ReadFogNear()  { float* p = GetFogFloat(FOG_NEAR_OFFS); return p ? *p : -1.f; }
    inline float ReadFogFar()   { float* p = GetFogFloat(FOG_FAR_OFFS);  return p ? *p : -1.f; }

    inline void WriteFogNear(float v) { float* p = GetFogFloat(FOG_NEAR_OFFS); if (p) *p = v; }
    inline void WriteFogFar (float v) { float* p = GetFogFloat(FOG_FAR_OFFS);  if (p) *p = v; }
}


class ProxyDirect3DDevice9 : public IDirect3DDevice9
{
public:
    ProxyDirect3DDevice9(IDirect3DDevice9* real, HWND hwnd)
        : m_real(real), m_hwnd(hwnd), m_ref(1)
    {
        Overlay::Init(real, hwnd);
        DS2EPCore::DispatchDeviceCreate(real);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        return m_real->QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_ref;
        if (r == 0) {
            DS2EPCore::DispatchDeviceDestroy(m_real);
            Overlay::Shutdown();
            m_real->Release();
            delete this;
        }
        return r;
    }

    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pp) override
    {
        DS2EPCore::DispatchDeviceLost(m_real);
        Overlay::OnLostDevice();
        HRESULT hr = m_real->Reset(pp);
        if (SUCCEEDED(hr)) {
            Overlay::OnResetDevice();
            DS2EPCore::DispatchDeviceReset(m_real);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE EndScene() override
    {
        DS2EPCore::DispatchEndScene(m_real);
        Overlay::OnEndScene(m_real);
        return m_real->EndScene();
    }

    HRESULT STDMETHODCALLTYPE Present(
        const RECT* src, const RECT* dst, HWND hwnd, const RGNDATA* rgn) override
    {
        DS2EPCore::DispatchPresent(m_real);
        Overlay::OnPresent(m_real);
        return m_real->Present(src, dst, hwnd, rgn);
    }

    // =========================================================================
    //  BeginScene  —  push persistent state overrides every frame
    //
    //  DS2 resets many states between scenes and draw calls. We use BeginScene
    //  as a guaranteed once-per-frame hook to force our overrides onto the
    //  device state directly, so they can't be quietly overwritten mid-frame.
    // =========================================================================
    HRESULT STDMETHODCALLTYPE BeginScene() override
    {
        HRESULT hr = m_real->BeginScene();
        if (SUCCEEDED(hr))
            PushFrameOverrides();
        return hr;
    }

    // =========================================================================
    //  SetTransform  —  patch projection matrix far-clip
    //
    //  DS2 uploads a left-handed perspective matrix for D3DTS_PROJECTION.
    //  Element [2][2] = farZ/(farZ-nearZ),  [3][2] = -nearZ*farZ/(farZ-nearZ)
    //  We recover nearZ and farZ, scale farZ, rebuild those two elements.
    // =========================================================================
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state,
                                            const D3DMATRIX* pM) override
    {
        if (!pM) return m_real->SetTransform(state, pM);

        D3DMATRIX m = *pM;

        // Let plugins intercept (e.g. ViewDistance patches far-clip)
        // If a plugin returns true it wants to block — but we still call through
        // with the (now modified) matrix since plugins mutate m in-place.
        DS2EPCore::DispatchSetTransform(m_real, state, m);

        return m_real->SetTransform(state, &m);
    }

    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override
    {
        // Let plugins intercept render state (e.g. to block fog enable)
        if (DS2EPCore::DispatchSetRenderState(m_real, state, value))
            return S_OK;

        return m_real->SetRenderState(state, value);
    }

    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD sampler,
                                               D3DSAMPLERSTATETYPE type,
                                               DWORD value) override
    {
        // Let plugins intercept first (e.g. AF plugin overrides filter types)
        DS2EPCore::DispatchSetSamplerState(m_real, sampler, type, value);
        return m_real->SetSamplerState(sampler, type, value);
    }

    // SetLight — passthrough (lighting handled via plugins if needed)
    HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT9* pLight) override {
        return m_real->SetLight(index, pLight);
    }

    HRESULT STDMETHODCALLTYPE CreateTexture(
        UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
        D3DPOOL pool, IDirect3DTexture9** ppTex, HANDLE* pShared) override
    {
        // Let plugins intercept — antialiasing.dll handles shadow upscale,
        // other plugins can create their own render targets here.
        if (DS2EPCore::DispatchCreateTexture(m_real, w, h, levels, usage, fmt, pool, ppTex))
            return S_OK;

        return m_real->CreateTexture(w, h, levels, usage, fmt, pool, ppTex, pShared);
    }

    // =========================================================================
    //  All remaining IDirect3DDevice9 methods — pure passthrough
    //  (grouped by category for readability)
    // =========================================================================

    // ── Device state ─────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override {
        return m_real->TestCooperativeLevel();
    }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override {
        return m_real->GetAvailableTextureMem();
    }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override {
        return m_real->EvictManagedResources();
    }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D9) override {
        return m_real->GetDirect3D(ppD3D9);
    }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* pCaps) override {
        return m_real->GetDeviceCaps(pCaps);
    }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT sw, D3DDISPLAYMODE* pMode) override {
        return m_real->GetDisplayMode(sw, pMode);
    }
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParm) override {
        return m_real->GetCreationParameters(pParm);
    }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* pSurf) override {
        return m_real->SetCursorProperties(x, y, pSurf);
    }
    void STDMETHODCALLTYPE SetCursorPosition(int x, int y, DWORD flags) override {
        m_real->SetCursorPosition(x, y, flags);
    }
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL show) override {
        return m_real->ShowCursor(show);
    }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(
        D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** ppSC) override {
        return m_real->CreateAdditionalSwapChain(pp, ppSC);
    }
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT i, IDirect3DSwapChain9** ppSC) override {
        return m_real->GetSwapChain(i, ppSC);
    }
    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override {
        return m_real->GetNumberOfSwapChains();
    }

    // ── Draw calls ───────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE DrawPrimitive(
        D3DPRIMITIVETYPE pt, UINT sv, UINT pc) override {
        return m_real->DrawPrimitive(pt, sv, pc);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(
        D3DPRIMITIVETYPE pt, INT bv, UINT minv, UINT nv, UINT si, UINT pc) override {
        return m_real->DrawIndexedPrimitive(pt, bv, minv, nv, si, pc);
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(
        D3DPRIMITIVETYPE pt, UINT pc, const void* pvd, UINT vs) override {
        return m_real->DrawPrimitiveUP(pt, pc, pvd, vs);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(
        D3DPRIMITIVETYPE pt, UINT minvi, UINT nv, UINT pc,
        const void* pid, D3DFORMAT idf, const void* pvd, UINT vs) override {
        return m_real->DrawIndexedPrimitiveUP(pt, minvi, nv, pc, pid, idf, pvd, vs);
    }
    HRESULT STDMETHODCALLTYPE ProcessVertices(
        UINT srcsi, UINT dstsi, UINT nv,
        IDirect3DVertexBuffer9* pvb, IDirect3DVertexDeclaration9* vd, DWORD flags) override {
        return m_real->ProcessVertices(srcsi, dstsi, nv, pvb, vd, flags);
    }

    // ── Render targets & surfaces ─────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD idx, IDirect3DSurface9* pRT) override {
        return m_real->SetRenderTarget(idx, pRT);
    }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD idx, IDirect3DSurface9** ppRT) override {
        return m_real->GetRenderTarget(idx, ppRT);
    }
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pDS) override {
        return m_real->SetDepthStencilSurface(pDS);
    }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppDS) override {
        return m_real->GetDepthStencilSurface(ppDS);
    }
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(
        IDirect3DSurface9* pRT, IDirect3DSurface9* pDest) override {
        return m_real->GetRenderTargetData(pRT, pDest);
    }
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT sw, IDirect3DSurface9* pDest) override {
        return m_real->GetFrontBufferData(sw, pDest);
    }
    HRESULT STDMETHODCALLTYPE StretchRect(
        IDirect3DSurface9* ps, const RECT* sr,
        IDirect3DSurface9* pd, const RECT* dr, D3DTEXTUREFILTERTYPE f) override {
        return m_real->StretchRect(ps, sr, pd, dr, f);
    }
    HRESULT STDMETHODCALLTYPE ColorFill(
        IDirect3DSurface9* pSurf, const RECT* pRect, D3DCOLOR color) override {
        return m_real->ColorFill(pSurf, pRect, color);
    }
    HRESULT STDMETHODCALLTYPE UpdateSurface(
        IDirect3DSurface9* pSrc, const RECT* pSrcR,
        IDirect3DSurface9* pDst, const POINT* pDstP) override {
        return m_real->UpdateSurface(pSrc, pSrcR, pDst, pDstP);
    }
    HRESULT STDMETHODCALLTYPE UpdateTexture(
        IDirect3DBaseTexture9* pSrc, IDirect3DBaseTexture9* pDst) override {
        return m_real->UpdateTexture(pSrc, pDst);
    }

    // ── Clear & viewport ─────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE Clear(
        DWORD cnt, const D3DRECT* pRects, DWORD flags,
        D3DCOLOR col, float z, DWORD stencil) override {
        return m_real->Clear(cnt, pRects, flags, col, z, stencil);
    }
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* pVP) override {
        return m_real->SetViewport(pVP);
    }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pVP) override {
        return m_real->GetViewport(pVP);
    }

    // ── Transform getters ────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE st, D3DMATRIX* pM) override {
        return m_real->GetTransform(st, pM);
    }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE st, const D3DMATRIX* pM) override {
        return m_real->MultiplyTransform(st, pM);
    }

    // ── Material ─────────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* pMat) override {
        return m_real->SetMaterial(pMat);
    }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pMat) override {
        return m_real->GetMaterial(pMat);
    }

    // ── Lights ───────────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE GetLight(DWORD idx, D3DLIGHT9* pL) override {
        return m_real->GetLight(idx, pL);
    }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD idx, BOOL en) override {
        return m_real->LightEnable(idx, en);
    }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD idx, BOOL* pEn) override {
        return m_real->GetLightEnable(idx, pEn);
    }

    // ── Clip planes ──────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD idx, const float* pPlane) override {
        return m_real->SetClipPlane(idx, pPlane);
    }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD idx, float* pPlane) override {
        return m_real->GetClipPlane(idx, pPlane);
    }

    // ── Render state getters ─────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE st, DWORD* pV) override {
        return m_real->GetRenderState(st, pV);
    }

    // ── State blocks ─────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9** ppSB) override {
        return m_real->CreateStateBlock(t, ppSB);
    }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override {
        return m_real->BeginStateBlock();
    }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB) override {
        return m_real->EndStateBlock(ppSB);
    }

    // ── Textures ─────────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DBaseTexture9* pTex) override {
        return m_real->SetTexture(stage, pTex);
    }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage, IDirect3DBaseTexture9** ppTex) override {
        return m_real->GetTexture(stage, ppTex);
    }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(
        DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD* pV) override {
        return m_real->GetTextureStageState(st, t, pV);
    }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(
        DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD v) override {
        return m_real->SetTextureStageState(st, t, v);
    }
    HRESULT STDMETHODCALLTYPE GetSamplerState(
        DWORD s, D3DSAMPLERSTATETYPE t, DWORD* pV) override {
        return m_real->GetSamplerState(s, t, pV);
    }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pPasses) override {
        return m_real->ValidateDevice(pPasses);
    }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT pal, const PALETTEENTRY* pE) override {
        return m_real->SetPaletteEntries(pal, pE);
    }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT pal, PALETTEENTRY* pE) override {
        return m_real->GetPaletteEntries(pal, pE);
    }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT pal) override {
        return m_real->SetCurrentTexturePalette(pal);
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* pPal) override {
        return m_real->GetCurrentTexturePalette(pPal);
    }
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* pR) override {
        return m_real->SetScissorRect(pR);
    }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pR) override {
        return m_real->GetScissorRect(pR);
    }
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL b) override {
        return m_real->SetSoftwareVertexProcessing(b);
    }
    BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override {
        return m_real->GetSoftwareVertexProcessing();
    }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float seg) override {
        return m_real->SetNPatchMode(seg);
    }
    float STDMETHODCALLTYPE GetNPatchMode() override {
        return m_real->GetNPatchMode();
    }

    // ── Vertex / Index buffers ────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(
        UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
        IDirect3DVertexBuffer9** ppVB, HANDLE* pSH) override {
        return m_real->CreateVertexBuffer(length, usage, fvf, pool, ppVB, pSH);
    }
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(
        UINT length, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
        IDirect3DIndexBuffer9** ppIB, HANDLE* pSH) override {
        return m_real->CreateIndexBuffer(length, usage, fmt, pool, ppIB, pSH);
    }
    HRESULT STDMETHODCALLTYPE SetStreamSource(
        UINT stream, IDirect3DVertexBuffer9* pVB, UINT off, UINT stride) override {
        return m_real->SetStreamSource(stream, pVB, off, stride);
    }
    HRESULT STDMETHODCALLTYPE GetStreamSource(
        UINT stream, IDirect3DVertexBuffer9** ppVB, UINT* pOff, UINT* pStride) override {
        return m_real->GetStreamSource(stream, ppVB, pOff, pStride);
    }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT stream, UINT freq) override {
        return m_real->SetStreamSourceFreq(stream, freq);
    }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT stream, UINT* pFreq) override {
        return m_real->GetStreamSourceFreq(stream, pFreq);
    }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIB) override {
        return m_real->SetIndices(pIB);
    }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIB) override {
        return m_real->GetIndices(ppIB);
    }

    // ── Vertex declarations / FVF ─────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(
        const D3DVERTEXELEMENT9* pElem, IDirect3DVertexDeclaration9** ppVD) override {
        return m_real->CreateVertexDeclaration(pElem, ppVD);
    }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* pVD) override {
        return m_real->SetVertexDeclaration(pVD);
    }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** ppVD) override {
        return m_real->GetVertexDeclaration(ppVD);
    }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) override {
        return m_real->SetFVF(fvf);
    }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) override {
        return m_real->GetFVF(pFVF);
    }

    // ── Shaders ───────────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE CreateVertexShader(
        const DWORD* pFunc, IDirect3DVertexShader9** ppVS) override {
        return m_real->CreateVertexShader(pFunc, ppVS);
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pVS) override {
        return m_real->SetVertexShader(pVS);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppVS) override {
        return m_real->GetVertexShader(ppVS);
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(
        UINT reg, const float* pC, UINT cnt) override {
        return m_real->SetVertexShaderConstantF(reg, pC, cnt);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(
        UINT reg, float* pC, UINT cnt) override {
        return m_real->GetVertexShaderConstantF(reg, pC, cnt);
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(
        UINT reg, const int* pC, UINT cnt) override {
        return m_real->SetVertexShaderConstantI(reg, pC, cnt);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(
        UINT reg, int* pC, UINT cnt) override {
        return m_real->GetVertexShaderConstantI(reg, pC, cnt);
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(
        UINT reg, const BOOL* pC, UINT cnt) override {
        return m_real->SetVertexShaderConstantB(reg, pC, cnt);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(
        UINT reg, BOOL* pC, UINT cnt) override {
        return m_real->GetVertexShaderConstantB(reg, pC, cnt);
    }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(
        const DWORD* pFunc, IDirect3DPixelShader9** ppPS) override {
        return m_real->CreatePixelShader(pFunc, ppPS);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pPS) override {
        return m_real->SetPixelShader(pPS);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppPS) override {
        return m_real->GetPixelShader(ppPS);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(
        UINT reg, const float* pC, UINT cnt) override {
        return m_real->SetPixelShaderConstantF(reg, pC, cnt);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(
        UINT reg, float* pC, UINT cnt) override {
        return m_real->GetPixelShaderConstantF(reg, pC, cnt);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(
        UINT reg, const int* pC, UINT cnt) override {
        return m_real->SetPixelShaderConstantI(reg, pC, cnt);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(
        UINT reg, int* pC, UINT cnt) override {
        return m_real->GetPixelShaderConstantI(reg, pC, cnt);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(
        UINT reg, const BOOL* pC, UINT cnt) override {
        return m_real->SetPixelShaderConstantB(reg, pC, cnt);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(
        UINT reg, BOOL* pC, UINT cnt) override {
        return m_real->GetPixelShaderConstantB(reg, pC, cnt);
    }

    // ── Resource creation (non-texture) ───────────────────────────────────────
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(
        UINT w, UINT h, UINT d, UINT lv, DWORD usage, D3DFORMAT fmt,
        D3DPOOL pool, IDirect3DVolumeTexture9** ppVT, HANDLE* pSH) override {
        return m_real->CreateVolumeTexture(w, h, d, lv, usage, fmt, pool, ppVT, pSH);
    }
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(
        UINT edge, UINT lv, DWORD usage, D3DFORMAT fmt,
        D3DPOOL pool, IDirect3DCubeTexture9** ppCT, HANDLE* pSH) override {
        return m_real->CreateCubeTexture(edge, lv, usage, fmt, pool, ppCT, pSH);
    }
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(
        UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
        DWORD msq, BOOL lock, IDirect3DSurface9** ppSurf, HANDLE* pSH) override {
        return m_real->CreateRenderTarget(w, h, fmt, ms, msq, lock, ppSurf, pSH);
    }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(
        UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
        DWORD msq, BOOL discard, IDirect3DSurface9** ppSurf, HANDLE* pSH) override {
        return m_real->CreateDepthStencilSurface(w, h, fmt, ms, msq, discard, ppSurf, pSH);
    }
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(
        UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool,
        IDirect3DSurface9** ppSurf, HANDLE* pSH) override {
        return m_real->CreateOffscreenPlainSurface(w, h, fmt, pool, ppSurf, pSH);
    }

    // ── Queries ───────────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9** ppQ) override {
        return m_real->CreateQuery(type, ppQ);
    }

    // ── Clip status ───────────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* pCS) override {
        return m_real->SetClipStatus(pCS);
    }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* pCS) override {
        return m_real->GetClipStatus(pCS);
    }

    // ── Back buffer / raster status ───────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE GetBackBuffer(
        UINT iSwapChain, UINT iBackBuffer,
        D3DBACKBUFFER_TYPE type, IDirect3DSurface9** ppBackBuffer) override {
        return m_real->GetBackBuffer(iSwapChain, iBackBuffer, type, ppBackBuffer);
    }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(
        UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) override {
        return m_real->GetRasterStatus(iSwapChain, pRasterStatus);
    }

    // ── Dialog box mode ───────────────────────────────────────────────────────
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL bEnableDialogs) override {
        return m_real->SetDialogBoxMode(bEnableDialogs);
    }

    // ── Gamma ramp ────────────────────────────────────────────────────────────
    void STDMETHODCALLTYPE SetGammaRamp(
        UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP* pRamp) override {
        m_real->SetGammaRamp(iSwapChain, Flags, pRamp);
    }
    void STDMETHODCALLTYPE GetGammaRamp(
        UINT iSwapChain, D3DGAMMARAMP* pRamp) override {
        m_real->GetGammaRamp(iSwapChain, pRamp);
    }

    // ── Higher-order primitives (N-patch / rect-patch / tri-patch) ───────────
    // DS2 doesn't use these but they are pure virtual in IDirect3DDevice9.
    HRESULT STDMETHODCALLTYPE DrawRectPatch(
        UINT Handle, const float* pNumSegs,
        const D3DRECTPATCH_INFO* pRectPatchInfo) override {
        return m_real->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo);
    }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(
        UINT Handle, const float* pNumSegs,
        const D3DTRIPATCH_INFO* pTriPatchInfo) override {
        return m_real->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo);
    }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override {
        return m_real->DeletePatch(Handle);
    }

private:
    // PushFrameOverrides — all per-frame logic now lives in plugins (OnBeginScene).
    // BeginScene() calls this which dispatches to all loaded plugins.
    void PushFrameOverrides()
    {
        DS2EPCore::DispatchBeginScene(m_real);
    }

    IDirect3DDevice9* m_real;
    HWND              m_hwnd;
    ULONG             m_ref;
};

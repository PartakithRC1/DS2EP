#include "DS2EP_SDK.h"
#include "imgui.h"
#include <windows.h>
#include <cmath>
#include <stdio.h>

// =============================================================================
//  DS2EP Plugin: View Distance  v1.1
//  Project: DS2EP-Plugin-ViewDistance  (32-bit DLL)
//  Output:  DS2EP-Plugins\viewdistance.dll
//
//  APPROACH: Pure D3D9 proxy interception — no memory hacking.
//
//  HOW DS2 FOG WORKS (learned from testing):
//    DS2 calls SetRenderState(D3DRS_FOGSTART, bits) and (D3DRS_FOGEND, bits)
//    where the DWORD bits are the IEEE 754 float representation of a
//    normalized [0..1] depth value.  Multiplying those by a scale > 1.0
//    clamps to 1.0 and does nothing visible.
//
//  WHAT ACTUALLY WORKS:
//    1. Intercept SetRenderState FOGSTART/FOGEND — just DISABLE fog entirely
//       when our scale is active. This is the cleanest approach for large
//       view distances.
//    2. Intercept SetTransform(D3DTS_PROJECTION) — patch the far-clip Z in
//       the matrix using the standard D3D left-handed projection math.
//       Cache the patched matrix and re-push it every BeginScene so DS2
//       can't restore the original via state blocks.
//    3. Re-push D3DRS_FOGENABLE=FALSE every BeginScene for the same reason.
//
//  This is exactly what worked in the pre-plugin proxy version.
// =============================================================================

static void   IniWriteFloat(const char* f, const char* k, float v)
{ char b[32]; snprintf(b,32,"%.4f",v); WritePrivateProfileStringA("Settings",k,b,f); }
static void   IniWriteBool (const char* f, const char* k, bool v)
{ WritePrivateProfileStringA("Settings",k,v?"true":"false",f); }
static float  IniReadFloat (const char* f, const char* k, float d)
{ char b[32]; snprintf(b,32,"%.4f",d); GetPrivateProfileStringA("Settings",k,b,b,32,f); return (float)atof(b); }
static bool   IniReadBool  (const char* f, const char* k, bool d)
{ char b[8]; strcpy_s(b,d?"true":"false"); GetPrivateProfileStringA("Settings",k,b,b,8,f);
  return (_stricmp(b,"true")==0 || strcmp(b,"1")==0); }

// =============================================================================
class ViewDistancePlugin : public IDS2EPPlugin
{
public:
    DS2EPPluginInfo GetInfo() override {
        return { "View Distance", "1.1",
                 "Extends far-clip and disables fog.\n"
                 "Pure D3D9 interception — no memory hacking." };
    }

    void OnDeviceCreate (IDirect3DDevice9* dev) override { m_dev = dev; }
    void OnDeviceLost   (IDirect3DDevice9*)     override { m_hasProjCache = false; }
    void OnDeviceReset  (IDirect3DDevice9* dev) override { m_dev = dev; m_hasProjCache = false; }
    void OnDeviceDestroy(IDirect3DDevice9*)     override { m_dev = nullptr; m_hasProjCache = false; }

    // =========================================================================
    //  OnSetTransform — intercept projection matrix, patch far-clip
    //
    //  Standard D3D left-handed perspective projection:
    //    m._33 = farZ / (farZ - nearZ)
    //    m._43 = -nearZ * farZ / (farZ - nearZ)
    //
    //  We recover nearZ and farZ, scale farZ, rebuild those two elements.
    //  Return false so the (now-patched) matrix still gets sent to D3D.
    // =========================================================================
    bool OnSetTransform(IDirect3DDevice9* dev,
                         D3DTRANSFORMSTATETYPE state, D3DMATRIX& m) override
    {
        if (!m_enabled || state != D3DTS_PROJECTION) return false;

        float A = m._33;  // farZ / (farZ - nearZ)
        float B = m._43;  // -nearZ * farZ / (farZ - nearZ)

        // Guard against orthographic (A == 1, B == 0) and degenerate matrices
        if (fabsf(A - 1.f) > 1e-5f && fabsf(B) > 1e-5f)
        {
            float nearZ = -B / A;
            float farZ  =  B / (1.f - A);

            if (nearZ > 0.f && farZ > nearZ)
            {
                float newFar = farZ * m_farClipScale;
                m._33 = newFar / (newFar - nearZ);
                m._43 = -nearZ * newFar / (newFar - nearZ);
            }
        }

        // Cache the patched matrix so we can re-push it every frame
        m_lastProj     = m;
        m_hasProjCache = true;

        return false; // pass through with patched values
    }

    // =========================================================================
    //  OnSetRenderState — swallow fog enable/start/end when we're active
    //
    //  DS2 normalizes fog distances to [0..1] before calling SetRenderState,
    //  so we can't scale them here. Instead we just disable fog entirely
    //  when our scale is active (> 1x). This gives a clean no-fog view.
    //  Players who want original fog can set scale to 1x.
    // =========================================================================
    bool OnSetRenderState(IDirect3DDevice9* dev,
                           D3DRENDERSTATETYPE state, DWORD& value) override
    {
        if (!m_enabled || m_farClipScale <= 1.f) return false;

        if (state == D3DRS_FOGENABLE)
        {
            value = FALSE;   // modify and let it pass through as FALSE
            return false;
        }
        // Swallow start/end — irrelevant once fog is disabled
        if (state == D3DRS_FOGSTART || state == D3DRS_FOGEND)
            return true; // block entirely

        return false;
    }

    // =========================================================================
    //  OnBeginScene — re-push overrides every frame
    //
    //  DS2 can restore render states via state blocks or on scene transitions.
    //  Re-pushing here ensures our values win regardless.
    // =========================================================================
    void OnBeginScene(IDirect3DDevice9* dev) override
    {
        if (!m_enabled) return;

        // Re-push patched projection matrix
        if (m_hasProjCache)
            dev->SetTransform(D3DTS_PROJECTION, &m_lastProj);

        // Re-push fog disable
        if (m_farClipScale > 1.f)
            dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    }

    // =========================================================================
    //  OnDrawMenu
    // =========================================================================
    void OnDrawMenu(ImGuiContext* ctx) override
    {
        ImGui::SetCurrentContext(ctx);

        bool changed = false;
        changed |= ImGui::Checkbox("Plugin Enabled", &m_enabled);
        ImGui::Separator();

        if (!m_enabled) { ImGui::TextDisabled("(disabled)"); return; }

        ImGui::TextDisabled("Ctrl+click any slider to type exact value.");
        ImGui::Spacing();

        ImGui::SetNextItemWidth(220);
        changed |= ImGui::SliderFloat("Far Clip Scale", &m_farClipScale,
            1.f, 10000.f, "%.0fx", ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Multiplies the projection matrix far-Z every frame.\n"
                "Above 1x: fog is also disabled for a clean view.\n"
                "1x = original game distances.");

        ImGui::Spacing();

        // Preset buttons
        struct { const char* label; float val; } presets[] = {
            {"1x (off)", 1.f}, {"50x", 50.f}, {"200x", 200.f},
            {"1000x", 1000.f}, {"MAX", 10000.f}
        };
        for (auto& p : presets) {
            if (ImGui::Button(p.label)) { m_farClipScale = p.val; changed = true; }
            ImGui::SameLine();
        }
        ImGui::NewLine();

        ImGui::Spacing();
        ImGui::Separator();
        if (m_hasProjCache) {
            // Back-calculate and display the actual far distance from cached matrix
            float A = m_lastProj._33, B = m_lastProj._43;
            if (fabsf(A - 1.f) > 1e-5f && fabsf(B) > 1e-5f) {
                float nearZ = -B / A;
                float farZ  =  B / (1.f - A);
                ImGui::TextDisabled("Active: near=%.3f  far=%.1f  (fog %s)",
                    nearZ, farZ, m_farClipScale > 1.f ? "OFF" : "ON");
            }
        } else {
            ImGui::TextDisabled("Projection not yet captured (load a map first)");
        }

        if (changed && m_iniPath[0])
            SaveSettings(m_iniPath);
    }

    void OnSaveSettings(const char* path) override { SaveSettings(path); }
    void OnLoadSettings(const char* path) override { LoadSettings(path); }
    void Destroy() override { delete this; }

private:
    void SaveSettings(const char* path)
    {
        strncpy_s(m_iniPath, path, MAX_PATH);
        IniWriteBool (path, "enabled",        m_enabled);
        IniWriteFloat(path, "far_clip_scale", m_farClipScale);
    }
    void LoadSettings(const char* path)
    {
        strncpy_s(m_iniPath, path, MAX_PATH);
        m_enabled      = IniReadBool (path, "enabled",        true);
        m_farClipScale = IniReadFloat(path, "far_clip_scale", 200.f);
    }

    IDirect3DDevice9* m_dev = nullptr;
    char  m_iniPath[MAX_PATH] = {};

    bool  m_enabled      = true;
    float m_farClipScale = 200.f;

    // Cached patched projection matrix — re-pushed every BeginScene
    D3DMATRIX m_lastProj     = {};
    bool      m_hasProjCache = false;
};

extern "C" __declspec(dllexport) IDS2EPPlugin* DS2EP_CreatePlugin()
{
    return new ViewDistancePlugin();
}

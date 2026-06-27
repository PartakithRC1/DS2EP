#include "../DS2EP-SDK/DS2EP_SDK.h"
#include "../../imgui/imgui.h"
#include <stdio.h>
#include <string>

// =============================================================================
//  DS2EP Plugin: Anisotropic Filtering + Shadow Upscale
//  Project: DS2EP-Plugin-Antialiasing  (32-bit DLL)
//  Output:  DS2EP-Plugins\antialiasing.dll
// =============================================================================

// ── Simple ini read/write ────────────────────────────────────────────────────
static void IniWriteFloat(const char* path, const char* key, float v)
{
    char buf[32]; snprintf(buf,sizeof(buf),"%.4f",v);
    WritePrivateProfileStringA("Settings",key,buf,path);
}
static void IniWriteInt(const char* path, const char* key, int v)
{
    char buf[16]; snprintf(buf,sizeof(buf),"%d",v);
    WritePrivateProfileStringA("Settings",key,buf,path);
}
static void IniWriteBool(const char* path, const char* key, bool v)
{
    WritePrivateProfileStringA("Settings",key,v?"true":"false",path);
}
static float IniReadFloat(const char* path, const char* key, float def)
{
    char buf[32]; snprintf(buf,sizeof(buf),"%.4f",def);
    GetPrivateProfileStringA("Settings",key,buf,buf,sizeof(buf),path);
    return (float)atof(buf);
}
static int IniReadInt(const char* path, const char* key, int def)
{
    return (int)GetPrivateProfileIntA("Settings",key,def,path);
}
static bool IniReadBool(const char* path, const char* key, bool def)
{
    char buf[8]; strcpy_s(buf,def?"true":"false");
    GetPrivateProfileStringA("Settings",key,buf,buf,sizeof(buf),path);
    return (_stricmp(buf,"true")==0 || strcmp(buf,"1")==0);
}

// =============================================================================
class AntialiasingPlugin : public IDS2EPPlugin
{
public:
    DS2EPPluginInfo GetInfo() override {
        return { "Anisotropic Filtering & Shadows", "1.0",
                 "Forces AF on all texture samplers.\nUpscales shadow map render targets." };
    }

    void OnDeviceCreate(IDirect3DDevice9* dev) override { m_dev = dev; }
    void OnDeviceDestroy(IDirect3DDevice9*) override    { m_dev = nullptr; }

    // ── Sampler state intercept ───────────────────────────────────────────────
    bool OnSetSamplerState(IDirect3DDevice9*, DWORD /*sampler*/,
                            D3DSAMPLERSTATETYPE type, DWORD& value) override
    {
        if (!m_enabled || !m_forceAF) return false;

        if (type == D3DSAMP_MINFILTER || type == D3DSAMP_MAGFILTER)
            { value = D3DTEXF_ANISOTROPIC; return false; } // let it through with new value
        if (type == D3DSAMP_MIPFILTER)
            { value = D3DTEXF_LINEAR;      return false; }
        if (type == D3DSAMP_MAXANISOTROPY)
            { value = (DWORD)m_afLevel;    return false; }
        return false;
    }

    // ── Shadow map upscale ────────────────────────────────────────────────────
    bool OnCreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels,
                          DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                          IDirect3DTexture9** ppTex) override
    {
        if (!m_enabled || !m_shadowUpscale) return false;
        if (!(usage & D3DUSAGE_RENDERTARGET)) return false;
        if ((int)w != m_shadowSrcW || (int)h != m_shadowSrcH) return false;

        UINT nw = w * (UINT)m_shadowScale;
        UINT nh = h * (UINT)m_shadowScale;
        HRESULT hr = dev->CreateTexture(nw, nh, levels, usage, fmt, pool, ppTex, nullptr);
        return SUCCEEDED(hr); // true = we handled it
    }

    // ── Menu ─────────────────────────────────────────────────────────────────
    void OnDrawMenu(ImGuiContext* ctx) override
    {
        // Sync to the core's ImGui context — each DLL has its own GImGui static.
        ImGui::SetCurrentContext(ctx);
        ImGui::Checkbox("Plugin Enabled", &m_enabled);
        ImGui::Separator();

        // ── AF ───────────────────────────────────────────────────────────────
        ImGui::SeparatorText("Anisotropic Filtering");
        bool changed = false;
        changed |= ImGui::Checkbox("Force AF on All Samplers", &m_forceAF);
        if (m_forceAF)
        {
            const char* labels[] = {"1x","2x","4x","8x","16x"};
            const int   vals[]   = {1,2,4,8,16};
            int idx = 4;
            for (int i=0;i<5;i++) if (vals[i]==m_afLevel){idx=i;break;}
            ImGui::SetNextItemWidth(160);
            if (ImGui::BeginCombo("AF Level##af", labels[idx]))
            {
                for (int i=0;i<5;i++){
                    bool sel=(i==idx);
                    if (ImGui::Selectable(labels[i],sel)){m_afLevel=vals[i];changed=true;}
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        ImGui::Spacing();

        // ── Shadows ──────────────────────────────────────────────────────────
        ImGui::SeparatorText("Shadow Map Upscaling");
        changed |= ImGui::Checkbox("Upscale Shadow Maps", &m_shadowUpscale);
        if (m_shadowUpscale)
        {
            const char* scaleLabels[] = {"1x  (512)","2x (1024)","4x (2048)","8x (4096)"};
            const int   scaleVals[]   = {1,2,4,8};
            int si = 3;
            for (int i=0;i<4;i++) if (scaleVals[i]==m_shadowScale){si=i;break;}
            ImGui::SetNextItemWidth(160);
            if (ImGui::BeginCombo("Shadow Scale##shd", scaleLabels[si]))
            {
                for (int i=0;i<4;i++){
                    bool sel=(i==si);
                    if (ImGui::Selectable(scaleLabels[i],sel)){m_shadowScale=scaleVals[i];changed=true;}
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Source RT detection size (tune if no effect):");
            ImGui::SetNextItemWidth(90);
            if (ImGui::InputInt("W##sw",&m_shadowSrcW,64)) { m_shadowSrcW=max(64,min(4096,m_shadowSrcW)); changed=true; }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            if (ImGui::InputInt("H##sh",&m_shadowSrcH,64)) { m_shadowSrcH=max(64,min(4096,m_shadowSrcH)); changed=true; }
            ImGui::TextDisabled("  Output: %dx%d", m_shadowSrcW*m_shadowScale, m_shadowSrcH*m_shadowScale);
        }

        if (changed && m_iniPath[0])
            SaveSettings(m_iniPath);
    }

    void OnSaveSettings(const char* path) override { SaveSettings(path); }
    void OnLoadSettings(const char* path) override { LoadSettings(path); }
    void Destroy() override
    {
        // Self-delete: we were new'd in this DLL's heap.
        // The core calls Destroy() instead of delete to avoid cross-DLL heap mismatch.
        delete this;
    }

private:
    void SaveSettings(const char* path)
    {
        strncpy_s(m_iniPath, path, MAX_PATH);
        IniWriteBool (path,"enabled",        m_enabled);
        IniWriteBool (path,"force_af",       m_forceAF);
        IniWriteInt  (path,"af_level",       m_afLevel);
        IniWriteBool (path,"shadow_upscale", m_shadowUpscale);
        IniWriteInt  (path,"shadow_scale",   m_shadowScale);
        IniWriteInt  (path,"shadow_src_w",   m_shadowSrcW);
        IniWriteInt  (path,"shadow_src_h",   m_shadowSrcH);
    }
    void LoadSettings(const char* path)
    {
        strncpy_s(m_iniPath, path, MAX_PATH);
        m_enabled       = IniReadBool(path,"enabled",        true);
        m_forceAF       = IniReadBool(path,"force_af",       true);
        m_afLevel       = IniReadInt (path,"af_level",       16);
        m_shadowUpscale = IniReadBool(path,"shadow_upscale", true);
        m_shadowScale   = IniReadInt (path,"shadow_scale",   8);
        m_shadowSrcW    = IniReadInt (path,"shadow_src_w",   512);
        m_shadowSrcH    = IniReadInt (path,"shadow_src_h",   512);
    }

    IDirect3DDevice9* m_dev = nullptr;
    char m_iniPath[MAX_PATH] = {};

    bool m_enabled       = true;
    bool m_forceAF       = true;
    int  m_afLevel       = 16;
    bool m_shadowUpscale = true;
    int  m_shadowScale   = 8;
    int  m_shadowSrcW    = 512;
    int  m_shadowSrcH    = 512;
};

extern "C" __declspec(dllexport) IDS2EPPlugin* DS2EP_CreatePlugin()
{
    return new AntialiasingPlugin();
}

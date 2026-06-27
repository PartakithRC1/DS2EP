#pragma once
#include <d3d9.h>
#include <windows.h>
#include <float.h>
#include <string>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx9.h"
#include "imgui/imgui_impl_win32.h"
#include "PluginSystem.h"
#include "DS2EPPlugin.h"

// =============================================================================
//  FpuGuard  —  DS2 trashes x87 precision/rounding mode via _controlfp.
//  Wrap every ImGui call block with this to keep the CRT happy.
// =============================================================================
#define _FPU_MASK (_MCW_EM | _MCW_RC | _MCW_PC | _MCW_IC)
struct FpuGuard {
    unsigned int saved;
    FpuGuard()  { _controlfp_s(&saved,0,0); unsigned int d; _controlfp_s(&d,_CW_DEFAULT,_FPU_MASK); }
    ~FpuGuard() { unsigned int d; _controlfp_s(&d,saved,_FPU_MASK); }
};

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

// Forward declarations — implemented in ProxyDirect3DDevice9.h
namespace DS2Mem {
    float ReadFogNear();
    float ReadFogFar();
}

// =============================================================================
//  Overlay
//
//  Menu auto-builds from whatever .ini files exist in DS2EP-Plugins\.
//  Each plugin gets its own collapsible section.
//  The proxy device reads settings via PluginSystem::GetFloat/Bool/Int
//  rather than from a hard-coded DS2Settings struct.
// =============================================================================
class Overlay
{
public:
    static void Init(IDirect3DDevice9* device, HWND hwnd)
    {
        ShutdownImGui();
        s_Device = device;
        s_Hwnd   = hwnd;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        io.BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;
        io.WantSetMousePos = false;

        StyleDS2();
        ImGui_ImplWin32_Init(hwnd);
        ImGui_ImplDX9_Init(device);

        if (hwnd != s_HookedHwnd)
        {
            if (s_HookedHwnd && s_OrigWndProc)
                SetWindowLongPtrA(s_HookedHwnd, GWLP_WNDPROC, (LONG_PTR)s_OrigWndProc);
            s_OrigWndProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
            s_HookedHwnd  = hwnd;
        }
        s_Initialised = true;
    }

    static void OnEndScene(IDirect3DDevice9*)
    {
        if (!s_Initialised) return;
        PluginSystem::Tick();
        CheckHotkey();
        if (s_FrameOpen) { ImGui::EndFrame(); s_FrameOpen = false; }
        if (!s_Visible) return;

        FpuGuard fpu;
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;
        ImGui::NewFrame(); s_FrameOpen = true;
        DrawMenu();
        ImGui::EndFrame(); s_FrameOpen = false; s_FrameReady = true;
    }

    static void OnPresent(IDirect3DDevice9*)
    {
        if (!s_Initialised) return;
        if (s_FrameReady) {
            FpuGuard fpu;
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            s_FrameReady = false;
            return;
        }
        // Fallback: EndScene was skipped
        PluginSystem::Tick();
        CheckHotkey();
        if (!s_Visible) return;
        FpuGuard fpu;
        if (s_FrameOpen) { ImGui::EndFrame(); s_FrameOpen = false; }
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;
        ImGui::NewFrame();
        DrawMenu();
        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }

    static void OnLostDevice()  { if (s_Initialised) { FpuGuard g; ImGui_ImplDX9_InvalidateDeviceObjects(); } }
    static void OnResetDevice() { if (s_Initialised) { FpuGuard g; ImGui_ImplDX9_CreateDeviceObjects();    } }

    static void Shutdown()
    {
        ShutdownImGui();
        if (s_HookedHwnd && s_OrigWndProc) {
            SetWindowLongPtrA(s_HookedHwnd, GWLP_WNDPROC, (LONG_PTR)s_OrigWndProc);
            s_OrigWndProc = nullptr; s_HookedHwnd = nullptr;
        }
    }

private:
    static void ShutdownImGui()
    {
        if (!s_Initialised) return;
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        s_Initialised = s_FrameOpen = s_FrameReady = false;
        s_Device = nullptr;
    }

    static void CloseMenu() { s_Visible = false; s_HotkeyHeld = false; ImGui::GetIO().MouseDrawCursor = false; }

    static void CheckHotkey()
    {
        bool combo = (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                     (GetAsyncKeyState(VK_LSHIFT)  & 0x8000) &&
                     (GetAsyncKeyState('0')         & 0x8000);
        if (combo && !s_HotkeyHeld) {
            s_Visible    = !s_Visible;
            s_HotkeyHeld = true;
            ImGui::GetIO().MouseDrawCursor = s_Visible;
        }
        if (!combo) s_HotkeyHeld = false;
    }

    // =========================================================================
    //  DrawMenu  —  auto-builds sections from PluginSystem
    // =========================================================================
    static void DrawMenu()
    {
        ImGui::SetNextWindowSize(ImVec2(460, 680), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos (ImVec2(40,  40),  ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("DS2 Enhancement Proxy", nullptr,
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize))
        { ImGui::End(); return; }

        // Plugin directory info bar
        ImGui::TextDisabled("Plugins: %s", PluginSystem::PluginDir().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Open Folder"))
            ShellExecuteA(nullptr,"open",PluginSystem::PluginDir().c_str(),nullptr,nullptr,SW_SHOW);
        ImGui::Separator();
        ImGui::Spacing();

        auto& plugins = PluginSystem::PluginsMut();
        if (plugins.empty()) {
            ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "No plugins found in DS2EP-Plugins\\");
        }

        for (auto& p : plugins)
        {
            // Skip core.ini — it has no user-facing settings panel
            if (p.name == "DS2 Enhancement Proxy Core") {
                continue;
            }

            std::string header = p.name.empty() ? p.path : p.name;
            bool open = ImGui::CollapsingHeader(header.c_str());

            // Enabled toggle inline with header
            ImGui::SameLine(ImGui::GetWindowWidth() - 60.f);
            std::string chkId = "##en_" + p.path;
            if (ImGui::Checkbox(chkId.c_str(), &p.enabled)) {
                PluginSystem::SetBool(p.name.c_str(), "enabled", p.enabled);
            }
            ImGui::SameLine(); ImGui::TextDisabled(p.enabled ? "ON" : "OFF");

            if (!open) continue;
            ImGui::Indent(10.f);

            // ── Per-plugin panels ─────────────────────────────────────────
            std::string stem = FileStem(p.path);

            if (stem == "antialiasing")   DrawPanel_Antialiasing(p);
            else if (stem == "viewdistance")  DrawPanel_ViewDistance(p);
            else if (stem == "resolution")    DrawPanel_Resolution(p);
            else                              DrawPanel_Generic(p);

            ImGui::Unindent(10.f);
            ImGui::Spacing();
        }

        // ── DLL Plugins (full D3D9 access) ───────────────────────────────────
        if (!DS2EPCore::g_Plugins.empty())
        {
            ImGui::Spacing();
            ImGui::SeparatorText("DLL Plugins");
            for (auto& lp : DS2EPCore::g_Plugins)
            {
                if (!lp.ptr) continue;
                DS2EPPluginInfo info = lp.ptr->GetInfo();
                // Guard against null fields from a badly compiled plugin
                const char* nm  = (info.name        && info.name[0])        ? info.name        : "(unnamed)";
                const char* ver = (info.version      && info.version[0])     ? info.version     : "?";
                const char* dsc = (info.description  && info.description[0]) ? info.description : "";

                char hdr[256];
                snprintf(hdr, sizeof(hdr), "%s  v%s", nm, ver);

                if (ImGui::CollapsingHeader(hdr))
                {
                    ImGui::Indent(10.f);
                    if (dsc[0]) { ImGui::TextDisabled("%s", dsc); ImGui::Spacing(); }
                    // Pass the core's context so the plugin can set it before
                    // calling any ImGui functions (each plugin DLL has its own
                    // GImGui static — must be synced to the core's context).
                    lp.ptr->OnDrawMenu(ImGui::GetCurrentContext());
                    ImGui::Unindent(10.f);
                }
            }
        }

        ImGui::Separator(); ImGui::Spacing();
        ImGui::TextDisabled("CTRL + LSHIFT + 0  —  toggle menu");
        float bw = 80.f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth()-bw)*0.5f);
        if (ImGui::Button("Close", ImVec2(bw,0))) CloseMenu();
        ImGui::End();
    }

    // ── Plugin panels ─────────────────────────────────────────────────────────

    static void DrawPanel_Antialiasing(PluginFile& p)
    {
        bool forceAF = GetB(p, "force_af", true);
        if (ImGui::Checkbox("Force Anisotropic Filtering", &forceAF))
            SetB(p, "force_af", forceAF);

        if (forceAF) {
            const char* labels[] = {"1x","2x","4x","8x","16x"};
            const int   vals[]   = {1,2,4,8,16};
            int cur = GetI(p, "af_level", 16);
            int idx = 4;
            for (int i=0;i<5;i++) if (vals[i]==cur){idx=i;break;}
            ImGui::SetNextItemWidth(160);
            if (ImGui::BeginCombo("AF Level", labels[idx])) {
                for (int i=0;i<5;i++){
                    bool sel=(i==idx);
                    if (ImGui::Selectable(labels[i],sel)) SetI(p,"af_level",vals[i]);
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        ImGui::Spacing();
        bool shadowUp = GetB(p, "shadow_upscale", true);
        if (ImGui::Checkbox("Upscale Shadow Maps", &shadowUp))
            SetB(p, "shadow_upscale", shadowUp);
        if (shadowUp) {
            const char* scl[] = {"1x (512)","2x (1024)","4x (2048)","8x (4096)"};
            const int   sv[]  = {1,2,4,8};
            int cur = GetI(p,"shadow_scale",8), si=3;
            for(int i=0;i<4;i++) if(sv[i]==cur){si=i;break;}
            ImGui::SetNextItemWidth(160);
            if (ImGui::BeginCombo("Shadow Scale", scl[si])) {
                for(int i=0;i<4;i++){
                    bool sel=(i==si);
                    if(ImGui::Selectable(scl[i],sel)) SetI(p,"shadow_scale",sv[i]);
                    if(sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            int sw = GetI(p,"shadow_src_width",512), sh = GetI(p,"shadow_src_height",512);
            ImGui::TextDisabled("Source detection size (tune if no effect)");
            ImGui::SetNextItemWidth(90); if(ImGui::InputInt("W##s",&sw,64,512)) SetI(p,"shadow_src_width",max(64,min(4096,sw)));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90); if(ImGui::InputInt("H##s",&sh,64,512)) SetI(p,"shadow_src_height",max(64,min(4096,sh)));
            ImGui::TextDisabled("  Output: %dx%d", GetI(p,"shadow_src_width",512)*cur, GetI(p,"shadow_src_height",512)*cur);
        }
    }

    static void DrawPanel_ViewDistance(PluginFile& p)
    {
        ImGui::TextDisabled("World-space multipliers. Ctrl+click to type exact value.");
        ImGui::Spacing();

        float fc = GetF(p,"far_clip_scale",200.f);
        float fs = GetF(p,"fog_start_scale",200.f);
        float fe = GetF(p,"fog_end_scale",200.f);

        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("Far Clip Scale",  &fc, 1.f, 10000.f, "%.0fx", ImGuiSliderFlags_Logarithmic))
            SetF(p,"far_clip_scale",fc);
        HelpTip("Multiplies projection matrix far-Z.\nControls how far geometry renders before being clipped.\nTry 200-2000.");

        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("Fog Start Scale", &fs, 1.f, 10000.f, "%.0fx", ImGuiSliderFlags_Logarithmic))
            SetF(p,"fog_start_scale",fs);

        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("Fog End Scale",   &fe, 1.f, 10000.f, "%.0fx", ImGuiSliderFlags_Logarithmic))
            SetF(p,"fog_end_scale",fe);

        ImGui::Spacing();
        if (ImGui::Button("1x (default)"))  { SetF(p,"far_clip_scale",1);    SetF(p,"fog_start_scale",1);    SetF(p,"fog_end_scale",1);    }
        ImGui::SameLine();
        if (ImGui::Button("100x"))           { SetF(p,"far_clip_scale",100);  SetF(p,"fog_start_scale",100);  SetF(p,"fog_end_scale",100);  }
        ImGui::SameLine();
        if (ImGui::Button("500x"))           { SetF(p,"far_clip_scale",500);  SetF(p,"fog_start_scale",500);  SetF(p,"fog_end_scale",500);  }
        ImGui::SameLine();
        if (ImGui::Button("5000x"))          { SetF(p,"far_clip_scale",5000); SetF(p,"fog_start_scale",5000); SetF(p,"fog_end_scale",5000); }

        ImGui::Spacing();
        float fn = DS2Mem::ReadFogNear(), ff = DS2Mem::ReadFogFar();
        if (fn > 0.f)
            ImGui::TextDisabled("DS2 fog memory: near=%.1f  far=%.1f world units", fn, ff);
        else
            ImGui::TextDisabled("DS2 fog memory: not readable (load a map first)");
    }

    static void DrawPanel_Resolution(PluginFile& p)
    {
        bool spoof = GetB(p,"spoof_enabled",false);
        if (ImGui::Checkbox("Virtual Resolution Spoof", &spoof)) SetB(p,"spoof_enabled",spoof);
        HelpTip("Tells DS2 it's running at this resolution.\nThe actual backbuffer stays at your real desktop resolution.\nChange requires game restart.");

        if (spoof) {
            int sw = GetI(p,"spoof_width",1920), sh = GetI(p,"spoof_height",1080);
            ImGui::SetNextItemWidth(100); if (ImGui::InputInt("Width##sp",&sw,1,100))  SetI(p,"spoof_width",max(320,sw));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100); if (ImGui::InputInt("Height##sp",&sh,1,100)) SetI(p,"spoof_height",max(240,sh));
            ImGui::Spacing();
            ImGui::TextDisabled("Presets:");
            ImGui::SameLine(); if(ImGui::SmallButton("1920x1080")){SetI(p,"spoof_width",1920);SetI(p,"spoof_height",1080);}
            ImGui::SameLine(); if(ImGui::SmallButton("2560x1440")){SetI(p,"spoof_width",2560);SetI(p,"spoof_height",1440);}
            ImGui::SameLine(); if(ImGui::SmallButton("3840x2160")){SetI(p,"spoof_width",3840);SetI(p,"spoof_height",2160);}
        }

        ImGui::Spacing();
        float vs = GetF(p,"viewport_scale",1.f);
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("Viewport Scale", &vs, 0.25f, 1.f, "%.2f"))
            SetF(p,"viewport_scale",vs);
        HelpTip("Scales the 3D render area to a fraction of the backbuffer.\n1.0 = fullscreen. 0.75 = 75% centered.");

        ImGui::Spacing();
        float usc = GetF(p,"ui_scale_compensation",1.f);
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("UI Scale Compensation", &usc, 0.25f, 4.f, "%.3f"))
            SetF(p,"ui_scale_compensation",usc);
        HelpTip("Compensates icon/texture coordinate scaling when spoof resolution\ndiffers from real backbuffer.\n1.0 = no compensation.\nSet to (real_width / spoof_width) for 1:1 icons.");
    }

    // Fallback for unknown plugins — shows all their key=value pairs as text
    static void DrawPanel_Generic(PluginFile& p)
    {
        ImGui::TextDisabled("(no custom panel — raw settings)");
        for (auto& kv : p.values) {
            if (kv.first.rfind("plugin.",0)==0) continue; // skip meta
            ImGui::TextDisabled("  %s = %s", kv.first.c_str(), kv.second.c_str());
        }
    }

    // ── Helpers ───────────────────────────────────────────────────────────────
    static void HelpTip(const char* txt) {
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", txt);
    }
    static std::string FileStem(const std::string& path) {
        size_t s = path.rfind('\\'); std::string n = (s==std::string::npos)?path:path.substr(s+1);
        size_t d = n.rfind('.'); if(d!=std::string::npos) n=n.substr(0,d);
        for(auto& c:n) c=(char)tolower((unsigned char)c);
        return n;
    }

    // Short helpers that read/write through PluginSystem.
    // Use filename stem (always populated) not p.name (may be empty if ini has no name= field).
    static std::string PluginKey(const PluginFile& p)
    {
        return FileStem(p.path);  // e.g. "viewdistance", "antialiasing"
    }
    static float GetF(const PluginFile& p, const char* k, float d)
    { return PluginSystem::GetFloat(PluginKey(p).c_str(), k, d); }
    static int   GetI(const PluginFile& p, const char* k, int   d)
    { return PluginSystem::GetInt  (PluginKey(p).c_str(), k, d); }
    static bool  GetB(const PluginFile& p, const char* k, bool  d)
    { return PluginSystem::GetBool (PluginKey(p).c_str(), k, d); }
    static void  SetF(PluginFile& p, const char* k, float v)
    { PluginSystem::SetFloat(PluginKey(p).c_str(), k, v); }
    static void  SetI(PluginFile& p, const char* k, int   v)
    { PluginSystem::SetInt  (PluginKey(p).c_str(), k, v); }
    static void  SetB(PluginFile& p, const char* k, bool  v)
    { PluginSystem::SetBool (PluginKey(p).c_str(), k, v); }

    // ── DS2 amber theme ───────────────────────────────────────────────────────
    static void StyleDS2()
    {
        ImGui::StyleColorsDark();
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding=4.f; s.FrameRounding=3.f; s.ScrollbarRounding=3.f; s.GrabRounding=3.f;
        s.WindowBorderSize=1.f; s.FrameBorderSize=0.f;
        ImVec4* c = s.Colors;
        c[ImGuiCol_WindowBg]         = {0.10f,0.09f,0.08f,0.95f};
        c[ImGuiCol_TitleBg]          = {0.18f,0.13f,0.07f,1.00f};
        c[ImGuiCol_TitleBgActive]    = {0.30f,0.20f,0.05f,1.00f};
        c[ImGuiCol_FrameBg]          = {0.20f,0.17f,0.12f,1.00f};
        c[ImGuiCol_FrameBgHovered]   = {0.35f,0.28f,0.10f,1.00f};
        c[ImGuiCol_FrameBgActive]    = {0.45f,0.35f,0.10f,1.00f};
        c[ImGuiCol_CheckMark]        = {0.95f,0.75f,0.20f,1.00f};
        c[ImGuiCol_SliderGrab]       = {0.85f,0.65f,0.15f,1.00f};
        c[ImGuiCol_SliderGrabActive] = {1.00f,0.82f,0.30f,1.00f};
        c[ImGuiCol_Button]           = {0.30f,0.22f,0.06f,1.00f};
        c[ImGuiCol_ButtonHovered]    = {0.55f,0.42f,0.10f,1.00f};
        c[ImGuiCol_ButtonActive]     = {0.70f,0.55f,0.15f,1.00f};
        c[ImGuiCol_Header]           = {0.30f,0.22f,0.06f,1.00f};
        c[ImGuiCol_HeaderHovered]    = {0.50f,0.38f,0.10f,1.00f};
        c[ImGuiCol_HeaderActive]     = {0.65f,0.50f,0.15f,1.00f};
        c[ImGuiCol_Separator]        = {0.40f,0.32f,0.15f,1.00f};
        c[ImGuiCol_SeparatorHovered] = {0.70f,0.55f,0.20f,1.00f};
        c[ImGuiCol_PopupBg]          = {0.12f,0.10f,0.08f,0.97f};
        c[ImGuiCol_Border]           = {0.40f,0.32f,0.12f,0.80f};
        c[ImGuiCol_Text]             = {0.92f,0.86f,0.70f,1.00f};
        c[ImGuiCol_TextDisabled]     = {0.55f,0.48f,0.35f,1.00f};
    }

    // ── Subclassed WndProc ────────────────────────────────────────────────────
    static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (s_Initialised && s_Visible) {
            ImGuiIO& io = ImGui::GetIO();
            switch (msg) {
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            case WM_MOUSEWHEEL:
            case WM_XBUTTONDOWN: case WM_XBUTTONUP:
                if (msg != WM_MOUSEWHEEL) {
                    short x=(short)(lParam&0xFFFF), y=(short)((lParam>>16)&0xFFFF);
                    io.AddMousePosEvent((float)x,(float)y);
                }
                ImGui_ImplWin32_WndProcHandler(hWnd,msg,wParam,lParam);
                if (io.WantCaptureMouse) return 0;
                break;
            case WM_SETCURSOR:
                if (io.WantCaptureMouse) return 1;
                break;
            case WM_KEYDOWN: case WM_KEYUP:
            case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            case WM_CHAR:
                ImGui_ImplWin32_WndProcHandler(hWnd,msg,wParam,lParam);
                if (io.WantCaptureKeyboard) return 0;
                break;
            default:
                ImGui_ImplWin32_WndProcHandler(hWnd,msg,wParam,lParam);
                break;
            }
        }
        return CallWindowProcA(s_OrigWndProc, hWnd, msg, wParam, lParam);
    }

    static inline bool     s_Initialised  = false;
    static inline bool     s_Visible      = false;
    static inline bool     s_HotkeyHeld   = false;
    static inline bool     s_FrameOpen    = false;
    static inline bool     s_FrameReady   = false;
    static inline HWND     s_Hwnd         = nullptr;
    static inline HWND     s_HookedHwnd   = nullptr;
    static inline IDirect3DDevice9* s_Device = nullptr;
    static inline WNDPROC  s_OrigWndProc  = nullptr;
};

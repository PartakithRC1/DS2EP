#pragma once
#include <d3d9.h>
#include <windows.h>
#include <vector>
#include <string>

// =============================================================================
//  DS2EPPlugin.h  —  Core-side plugin system
//
//  The interface (IDS2EPPlugin, DS2EPPluginInfo) lives in DS2EP_SDK.h.
//  We include it here so the core and plugins share ONE vtable definition.
//  The path assumes DS2EP_SDK.h is in ..\DS2EP-SDK\ relative to the core.
//  Adjust if your folder layout differs.
// =============================================================================
#include "DS2EP_SDK.h"

// =============================================================================
//  DS2EPCore  —  loads plugin DLLs, dispatches events to them
// =============================================================================
namespace DS2EPCore
{
    struct LoadedPlugin
    {
        IDS2EPPlugin* ptr     = nullptr;
        HMODULE       hMod    = nullptr;
        std::string   iniPath;
    };

    inline std::vector<LoadedPlugin> g_Plugins;

    inline void LoadPluginDLLs(const std::string& pluginDir)
    {
        std::string pattern = pluginDir + "*.dll";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string dllPath = pluginDir + fd.cFileName;
            HMODULE hMod = LoadLibraryA(dllPath.c_str());
            if (!hMod) continue;

            auto* fn = (IDS2EPPlugin*(*)())GetProcAddress(hMod, "DS2EP_CreatePlugin");
            if (!fn) { FreeLibrary(hMod); continue; }

            IDS2EPPlugin* plugin = fn();
            if (!plugin) { FreeLibrary(hMod); continue; }

            // Derive matching .ini path (same folder, same stem)
            std::string iniPath = dllPath.substr(0, dllPath.rfind('.')) + ".ini";
            DWORD attr = GetFileAttributesA(iniPath.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES)
                plugin->OnLoadSettings(iniPath.c_str());

            g_Plugins.push_back({ plugin, hMod, iniPath });
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    inline void UnloadAll()
    {
        for (auto& lp : g_Plugins) {
            if (lp.ptr) {
                lp.ptr->Destroy();
                // Do NOT call delete lp.ptr here — the object was new'd inside
                // the plugin DLL's CRT heap. Deleting from the core's CRT heap
                // causes debug_heap.cpp __acrt_first_block assertion.
                // The plugin is responsible for freeing itself inside Destroy().
                lp.ptr = nullptr;
            }
            if (lp.hMod) { FreeLibrary(lp.hMod); lp.hMod = nullptr; }
        }
        g_Plugins.clear();
    }

    // ── Dispatchers ───────────────────────────────────────────────────────────
    inline void DispatchDeviceCreate (IDirect3DDevice9* d) { for(auto& lp:g_Plugins) lp.ptr->OnDeviceCreate(d);  }
    inline void DispatchDeviceLost   (IDirect3DDevice9* d) { for(auto& lp:g_Plugins) lp.ptr->OnDeviceLost(d);   }
    inline void DispatchDeviceReset  (IDirect3DDevice9* d) { for(auto& lp:g_Plugins) lp.ptr->OnDeviceReset(d);  }
    inline void DispatchDeviceDestroy(IDirect3DDevice9* d) { for(auto& lp:g_Plugins) lp.ptr->OnDeviceDestroy(d);}
    inline void DispatchBeginScene   (IDirect3DDevice9* d) { for(auto& lp:g_Plugins) lp.ptr->OnBeginScene(d);   }
    inline void DispatchEndScene     (IDirect3DDevice9* d) { for(auto& lp:g_Plugins) lp.ptr->OnEndScene(d);     }
    inline void DispatchPresent      (IDirect3DDevice9* d) { for(auto& lp:g_Plugins) lp.ptr->OnPresent(d);      }
    inline void DispatchDrawMenu(ImGuiContext* ctx)
    { for(auto& lp:g_Plugins) lp.ptr->OnDrawMenu(ctx); }

    inline bool DispatchSetRenderState(IDirect3DDevice9* d,
        D3DRENDERSTATETYPE state, DWORD& value)
    { for(auto& lp:g_Plugins) if(lp.ptr->OnSetRenderState(d,state,value))  return true; return false; }

    inline bool DispatchSetSamplerState(IDirect3DDevice9* d,
        DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD& value)
    { for(auto& lp:g_Plugins) if(lp.ptr->OnSetSamplerState(d,sampler,type,value)) return true; return false; }

    inline bool DispatchSetTransform(IDirect3DDevice9* d,
        D3DTRANSFORMSTATETYPE state, D3DMATRIX& mat)
    { for(auto& lp:g_Plugins) if(lp.ptr->OnSetTransform(d,state,mat)) return true; return false; }

    inline bool DispatchCreateTexture(IDirect3DDevice9* d,
        UINT w, UINT h, UINT levels, DWORD usage,
        D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** ppTex)
    { for(auto& lp:g_Plugins) if(lp.ptr->OnCreateTexture(d,w,h,levels,usage,fmt,pool,ppTex)) return true; return false; }
}

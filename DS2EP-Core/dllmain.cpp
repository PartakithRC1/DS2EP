#include <windows.h>
#include <d3d9.h>
#include "PluginSystem.h"
#include "DS2EPPlugin.h"
#include "ProxyDirect3D9.h"

// =============================================================================
//  dllmain.cpp  —  DS2 Enhancement Proxy core loader
//
//  Load order:
//    1. PluginSystem::Init()  — scans DS2EP-Plugins\*.ini, hot-reload ready
//    2. DS2EPCore::LoadPluginDLLs() — loads DS2EP-Plugins\*.dll plugins
//    3. Real d3d9.dll loaded from System32
//    4. Direct3DCreate9 exported — DS2 calls this, we return ProxyDirect3D9
// =============================================================================

typedef IDirect3D9* (WINAPI* Direct3DCreate9_t)(UINT SDKVersion);
Direct3DCreate9_t Real_Direct3DCreate9 = nullptr;
HMODULE           hRealD3D9            = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        // Init plugin system first (reads ini files, creates DS2EP-Plugins\ if needed)
        PluginSystem::Init();

        // Load plugin DLLs
        DS2EPCore::LoadPluginDLLs(PluginSystem::PluginDir());

        // Load real System32 d3d9.dll
        char sysDir[MAX_PATH];
        GetSystemDirectoryA(sysDir, MAX_PATH);
        strcat_s(sysDir, "\\d3d9.dll");
        hRealD3D9 = LoadLibraryA(sysDir);
        if (hRealD3D9)
            Real_Direct3DCreate9 = (Direct3DCreate9_t)
                GetProcAddress(hRealD3D9, "Direct3DCreate9");
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        DS2EPCore::UnloadAll();
        if (hRealD3D9) FreeLibrary(hRealD3D9);
    }
    return TRUE;
}

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion)
{
    if (!Real_Direct3DCreate9) return nullptr;
    IDirect3D9* realD3D = Real_Direct3DCreate9(SDKVersion);
    if (!realD3D) return nullptr;
    return new ProxyDirect3D9(realD3D);
}

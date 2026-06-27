#pragma once
#include <windows.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdio>

// =============================================================================
//  PluginSystem  —  reads \DS2EP-Plugins\*.ini, exposes typed settings,
//                   hot-reloads when any file changes on disk.
//
//  INI format:
//    [Plugin]
//    name    = My Plugin
//    enabled = true
//
//    [Settings]
//    my_float = 1.5
//    my_bool  = true
//    my_int   = 8
//
//  All keys are lowercase. Section headers are stripped of brackets.
//  Lines beginning with # or ; are comments.
//
//  Hot-reload: PluginSystem::Tick() must be called once per frame.
//  It checks file write-times every ~2 seconds and re-reads changed files.
// =============================================================================

struct PluginFile
{
    std::string                                 path;       // absolute path
    std::string                                 name;       // from [Plugin] name=
    bool                                        enabled = true;
    std::unordered_map<std::string, std::string> values; // all key=value pairs
    FILETIME                                    lastWrite = {};
};

class PluginSystem
{
public:
    // -------------------------------------------------------------------------
    //  Init  —  call once at DLL_PROCESS_ATTACH.
    //  Finds the game exe directory, appends \DS2EP-Plugins\, scans for *.ini.
    // -------------------------------------------------------------------------
    static void Init()
    {
        // Resolve plugin directory relative to d3d9.dll location
        char dllPath[MAX_PATH] = {};
        HMODULE hSelf = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&Init), &hSelf);
        GetModuleFileNameA(hSelf, dllPath, MAX_PATH);

        // Strip filename, keep directory
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';

        s_PluginDir = std::string(dllPath) + "DS2EP-Plugins\\";

        // Create the folder if it doesn't exist yet
        CreateDirectoryA(s_PluginDir.c_str(), nullptr);

        ScanAndLoad();
    }

    // -------------------------------------------------------------------------
    //  Tick  —  call once per frame. Re-scans every 2 seconds.
    // -------------------------------------------------------------------------
    static void Tick()
    {
        DWORD now = GetTickCount();
        if (now - s_LastScanTick < 2000) return;
        s_LastScanTick = now;

        bool anyChanged = false;
        for (auto& p : s_Plugins)
        {
            FILETIME ft = GetFileWriteTime(p.path.c_str());
            if (CompareFileTime(&ft, &p.lastWrite) != 0)
            {
                LoadFile(p);
                anyChanged = true;
            }
        }
        // Also check if new .ini files have appeared
        if (CountIniFiles() != (int)s_Plugins.size())
        {
            ScanAndLoad();
            anyChanged = true;
        }

        if (anyChanged)
            s_DirtyFlag = true;
    }

    // Returns true once after any reload, then resets.
    static bool ConsumeReload()
    {
        if (s_DirtyFlag) { s_DirtyFlag = false; return true; }
        return false;
    }

    // -------------------------------------------------------------------------
    //  Typed accessors  —  look up key across all enabled plugins.
    //  'key' format: "pluginname.keyname"  e.g. "antialiasing.af_level"
    //  Falls back to defaultVal if not found or plugin disabled.
    // -------------------------------------------------------------------------
    static float   GetFloat (const char* pluginName, const char* key, float   def = 0.f);
    static int     GetInt   (const char* pluginName, const char* key, int     def = 0);
    static bool    GetBool  (const char* pluginName, const char* key, bool    def = false);
    static std::string GetStr(const char* pluginName, const char* key, const char* def = "");

    // Write a value back to the ini file (for when menu changes a setting)
    static void    SetFloat (const char* pluginName, const char* key, float   val);
    static void    SetInt   (const char* pluginName, const char* key, int     val);
    static void    SetBool  (const char* pluginName, const char* key, bool    val);

    // Direct access to plugin list for the menu UI
    static const std::vector<PluginFile>& Plugins() { return s_Plugins; }
    static std::vector<PluginFile>&       PluginsMut() { return s_Plugins; }
    static const std::string&             PluginDir() { return s_PluginDir; }

private:
    // ── Internal helpers ─────────────────────────────────────────────────────

    static std::string Trim(const std::string& s)
    {
        if (s.empty()) return {};
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return {};
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }
    static std::string Lower(std::string s)
    {
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        return s;
    }

    static FILETIME GetFileWriteTime(const char* path)
    {
        FILETIME ft = {};
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa))
            ft = fa.ftLastWriteTime;
        return ft;
    }

    static void LoadFile(PluginFile& p)
    {
        p.values.clear();
        p.name    = "";
        p.enabled = true;
        p.lastWrite = GetFileWriteTime(p.path.c_str());

        FILE* f = nullptr;
        fopen_s(&f, p.path.c_str(), "r");
        if (!f) return;

        char   line[512];
        std::string section;
        while (fgets(line, sizeof(line), f))
        {
            std::string L = Trim(line);
            if (L.empty() || L[0] == '#' || L[0] == ';') continue;

            if (L[0] == '[')
            {
                // Section header
                size_t end = L.find(']');
                if (end != std::string::npos)
                    section = Lower(L.substr(1, end - 1));
                continue;
            }

            size_t eq = L.find('=');
            if (eq == std::string::npos) continue;

            std::string rawKey = Trim(L.substr(0, eq));
            std::string val    = Trim(L.substr(eq + 1));

            // Strip inline comments
            size_t commentPos = val.find('#');
            if (commentPos != std::string::npos) val = Trim(val.substr(0, commentPos));

            std::string key = Lower(rawKey);

            if (section == "plugin")
            {
                if (key == "name")    p.name    = val;
                if (key == "enabled") p.enabled = (Lower(val) == "true" || val == "1");
            }
            // Store everything flat with section prefix for lookup
            p.values[section + "." + key] = val;
        }
        fclose(f);
    }

    static void ScanAndLoad()
    {
        // Keep existing entries, update or add
        std::string pattern = s_PluginDir + "*.ini";
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;

        std::vector<std::string> found;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            found.push_back(s_PluginDir + fd.cFileName);
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);

        std::sort(found.begin(), found.end()); // alphabetical, core.ini first

        // Rebuild list preserving any already-loaded plugins
        std::vector<PluginFile> updated;
        for (auto& fp : found)
        {
            // Find existing or create new
            PluginFile* existing = nullptr;
            for (auto& ex : s_Plugins)
                if (ex.path == fp) { existing = &ex; break; }

            if (existing)
            {
                updated.push_back(*existing);
            }
            else
            {
                PluginFile p;
                p.path = fp;
                LoadFile(p);
                updated.push_back(std::move(p));
            }
        }
        s_Plugins = std::move(updated);
        s_DirtyFlag = true;
    }

    static int CountIniFiles()
    {
        int count = 0;
        std::string pattern = s_PluginDir + "*.ini";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) count++; }
        while (FindNextFileA(h, &fd));
        FindClose(h);
        return count;
    }

    // Find a plugin by its ini filename stem (e.g. "antialiasing" for antialiasing.ini)
    // Also matches by Plugin.name field.  Always falls back to stem so plugins
    // with a missing [Plugin] name= field are still found correctly.
    static const PluginFile* FindPlugin(const char* name)
    {
        if (!name || !*name) return nullptr;   // guard empty/null
        std::string needle = Lower(name);
        if (needle.empty()) return nullptr;

        for (auto& p : s_Plugins)
        {
            // Match by Plugin.name field (if set)
            if (!p.name.empty() && Lower(p.name) == needle) return &p;

            // Always try filename stem as fallback
            size_t slash = p.path.rfind('\\');
            std::string stem = (slash == std::string::npos) ? p.path : p.path.substr(slash + 1);
            size_t dot = stem.rfind('.');
            if (dot != std::string::npos) stem = stem.substr(0, dot);
            if (!stem.empty() && Lower(stem) == needle) return &p;
        }
        return nullptr;
    }
    static PluginFile* FindPluginMut(const char* name)
    {
        return const_cast<PluginFile*>(FindPlugin(name));
    }

    static void WriteKeyToFile(PluginFile& p, const char* section,
                                const char* key, const char* newVal)
    {
        // Read all lines, find the key, replace in-place, rewrite file
        FILE* f = nullptr;
        fopen_s(&f, p.path.c_str(), "r");
        if (!f) return;

        std::vector<std::string> lines;
        char buf[512];
        while (fgets(buf, sizeof(buf), f)) lines.push_back(buf);
        fclose(f);

        std::string sectionLow = Lower(section);
        std::string keyLow     = Lower(key);
        std::string curSection;
        bool written = false;

        for (auto& L : lines)
        {
            std::string t = Trim(L);
            if (t.empty()) continue;
            if (t[0] == '[')
            {
                size_t e = t.find(']');
                if (e != std::string::npos) curSection = Lower(t.substr(1, e - 1));
                continue;
            }
            size_t eq = t.find('=');
            if (eq != std::string::npos && curSection == sectionLow)
            {
                std::string k = Lower(Trim(t.substr(0, eq)));
                if (k == keyLow)
                {
                    L = key + std::string(" = ") + newVal + "\n";
                    written = true;
                }
            }
        }
        if (!written)
            lines.push_back(std::string(key) + " = " + newVal + "\n");

        fopen_s(&f, p.path.c_str(), "w");
        if (!f) return;
        for (auto& L : lines) fputs(L.c_str(), f);
        fclose(f);

        // Update in-memory value immediately
        p.values[sectionLow + "." + keyLow] = newVal;
        p.lastWrite = GetFileWriteTime(p.path.c_str());
    }

    static inline std::vector<PluginFile> s_Plugins;
    static inline std::string             s_PluginDir;
    static inline DWORD                   s_LastScanTick = 0;
    static inline bool                    s_DirtyFlag    = false;
};

// ─── Typed accessor implementations ──────────────────────────────────────────

inline float PluginSystem::GetFloat(const char* pluginName, const char* key, float def)
{
    if (!pluginName || !key) return def;
    const PluginFile* p = FindPlugin(pluginName);
    if (!p || !p->enabled) return def;
    std::string k = std::string("settings.") + Lower(key);
    auto it = p->values.find(k);
    if (it == p->values.end()) return def;
    try { return std::stof(it->second); } catch (...) { return def; }
}

inline int PluginSystem::GetInt(const char* pluginName, const char* key, int def)
{
    if (!pluginName || !key) return def;
    const PluginFile* p = FindPlugin(pluginName);
    if (!p || !p->enabled) return def;
    std::string k = std::string("settings.") + Lower(key);
    auto it = p->values.find(k);
    if (it == p->values.end()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}

inline bool PluginSystem::GetBool(const char* pluginName, const char* key, bool def)
{
    if (!pluginName || !key) return def;
    const PluginFile* p = FindPlugin(pluginName);
    if (!p || !p->enabled) return def;
    std::string k = std::string("settings.") + Lower(key);
    auto it = p->values.find(k);
    if (it == p->values.end()) return def;
    std::string v = Lower(it->second);
    return (v == "true" || v == "1" || v == "yes");
}

inline std::string PluginSystem::GetStr(const char* pluginName, const char* key, const char* def)
{
    const PluginFile* p = FindPlugin(pluginName);
    if (!p || !p->enabled) return def;
    std::string k = std::string("settings.") + Lower(key);
    auto it = p->values.find(k);
    return (it == p->values.end()) ? def : it->second;
}

inline void PluginSystem::SetFloat(const char* pluginName, const char* key, float val)
{
    if (!pluginName || !key) return;
    PluginFile* p = FindPluginMut(pluginName);
    if (!p) return;
    char buf[64]; snprintf(buf, sizeof(buf), "%.4f", val);
    WriteKeyToFile(*p, "Settings", key, buf);
    s_DirtyFlag = true;
}

inline void PluginSystem::SetInt(const char* pluginName, const char* key, int val)
{
    if (!pluginName || !key) return;
    PluginFile* p = FindPluginMut(pluginName);
    if (!p) return;
    char buf[32]; snprintf(buf, sizeof(buf), "%d", val);
    WriteKeyToFile(*p, "Settings", key, buf);
    s_DirtyFlag = true;
}

inline void PluginSystem::SetBool(const char* pluginName, const char* key, bool val)
{
    if (!pluginName || !key) return;
    PluginFile* p = FindPluginMut(pluginName);
    if (!p) return;
    WriteKeyToFile(*p, "Settings", key, val ? "true" : "false");
    s_DirtyFlag = true;
}

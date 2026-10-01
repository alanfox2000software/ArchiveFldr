// ThirdParty.cpp — see ThirdParty.h for the layout contract.
#include "stdafx.h"
#include "ThirdParty.h"

namespace ThirdParty {

// ─────────────────────────────────────────────────────────────────────────
// The component table.
//
// ADDING A NEW ENGINE DLL — copy one row and fill it in:
//
//   { L"unrar",                      // thirdparty\unrar\
//     L"UnRAR engine",               // shown in error messages
//     L"unrar.dll",                  // ';'-separated candidates, best first
//     L"SOFTWARE\\WinRAR", L"exe64", L"unrar.dll" },   // optional reg hint
//
// Nothing else in the resolver needs to change: naming, bitness suffixes,
// the directory search order and the diagnostics all come from here.
// ─────────────────────────────────────────────────────────────────────────
static const Component kComponents[] =
{
    { L"7z", L"7-Zip engine", L"7z.dll;7za.dll",
      L"SOFTWARE\\7-Zip", L"Path", L"7z.dll" },
};

const Component* Find(const wchar_t* id)
{
    if (!id) return nullptr;
    for (const auto& c : kComponents)
        if (_wcsicmp(c.id, id) == 0) return &c;
    return nullptr;
}

std::vector<const Component*> All()
{
    std::vector<const Component*> v;
    for (const auto& c : kComponents) v.push_back(&c);
    return v;
}

const wchar_t* BitnessTag()
{
    return (sizeof(void*) == 8) ? L"64" : L"32";
}

std::wstring ModuleDir()
{
    wchar_t path[MAX_PATH * 2] = {};
    HMODULE hSelf = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDir), &hSelf))
        return L"";
    if (!GetModuleFileNameW(hSelf, path, ARRAYSIZE(path))) return L"";
    PathRemoveFileSpecW(path);
    return path;
}

static bool FileExists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::vector<std::wstring> SplitNames(const wchar_t* list)
{
    std::vector<std::wstring> out;
    if (!list) return out;
    std::wstring cur;
    for (const wchar_t* p = list; ; ++p)
    {
        if (*p == L';' || *p == L'\0')
        {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            if (*p == L'\0') break;
        }
        else cur += *p;
    }
    return out;
}

// "7z.dll" → "7z.64.dll", "7z64.dll", "7z.dll"
static std::vector<std::wstring> NameVariants(const std::wstring& base)
{
    std::wstring stem = base, ext;
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos) { stem = base.substr(0, dot); ext = base.substr(dot); }

    const std::wstring bits = BitnessTag();
    return { stem + L"." + bits + ext,     // 7z.64.dll
             stem + bits + ext,            // unrar64.dll
             base };                       // 7z.dll
}

std::vector<std::wstring> ProbePaths(const Component& c)
{
    std::vector<std::wstring> out;
    const std::wstring dir = ModuleDir();
    if (dir.empty()) return out;

    // Search directories, most specific first.
    const std::wstring subDirs[] = {
        L"\\thirdparty\\" + std::wstring(c.id) + L"\\",
        L"\\thirdparty\\",
        L"\\" + std::wstring(c.id) + L"\\",
        L"\\",
    };

    for (const auto& sub : subDirs)
        for (const auto& base : SplitNames(c.baseNames))
            for (const auto& name : NameVariants(base))
                out.push_back(dir + sub + name);

    return out;
}

// HKLM/HKCU\<key>\<value> → folder; returns folder\<fileName> when it exists.
static std::wstring TryRegistryHint(const Component& c)
{
    if (!c.regKey || !c.regValue || !c.regFileName) return L"";

    const struct { HKEY root; DWORD view; } kHives[] = {
        { HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY },
        { HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY },
        { HKEY_CURRENT_USER,  KEY_WOW64_64KEY },
        { HKEY_CURRENT_USER,  KEY_WOW64_32KEY },
    };

    for (const auto& h : kHives)
    {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(h.root, c.regKey, 0, KEY_READ | h.view, &hKey) != ERROR_SUCCESS)
            continue;

        wchar_t val[MAX_PATH] = {};
        DWORD cb = sizeof(val), type = 0;
        LSTATUS st = RegQueryValueExW(hKey, c.regValue, nullptr, &type,
                                      reinterpret_cast<LPBYTE>(val), &cb);
        RegCloseKey(hKey);
        if (st != ERROR_SUCCESS || type != REG_SZ || !val[0]) continue;

        std::wstring p = val;
        // The value may name the install folder or an executable inside it.
        if (FileExists(p))
        {
            std::wstring folder = p;
            PathRemoveFileSpecW(folder.data());
            p = folder.c_str();
        }
        if (p.empty()) continue;
        if (p.back() != L'\\') p += L'\\';
        p += c.regFileName;
        if (FileExists(p)) return p;
    }
    return L"";
}

std::wstring Resolve(const wchar_t* id)
{
    const Component* c = Find(id);
    if (!c) return L"";

    for (const auto& cand : ProbePaths(*c))
        if (FileExists(cand)) return cand;

    return TryRegistryHint(*c);
}

std::wstring DescribeSearch(const wchar_t* id)
{
    const Component* c = Find(id);
    if (!c) return L"";

    std::wstring s = std::wstring(c->displayName) + L" (" +
                     SplitNames(c->baseNames).front() + L", " +
                     BitnessTag() + L"-bit) was looked for here:\n";
    for (const auto& p : ProbePaths(*c))
        s += L"\n    " + p;
    if (c->regKey)
        s += L"\n\n...and via an installed copy registered under\n    HKLM\\" +
             std::wstring(c->regKey) + L"\\" + c->regValue +
             L"  (and HKCU, both registry views).";
    return s;
}

HMODULE Load(const std::wstring& fullPath)
{
    if (fullPath.empty()) return nullptr;
    return LoadLibraryExW(fullPath.c_str(), nullptr,
                          LOAD_WITH_ALTERED_SEARCH_PATH);
}

} // namespace ThirdParty

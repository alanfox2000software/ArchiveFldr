// ThirdParty.cpp — see ThirdParty.h for the layout contract.
#include "stdafx.h"
#include "ThirdParty.h"
#include "SysInfo.h"

namespace ThirdParty {

// ─────────────────────────────────────────────────────────────────────────
// The component table.
//
// ADDING A NEW ENGINE DLL — copy one row and fill it in:
//
//   { L"unrar",                      // thirdparty\unrar\
//     L"UnRAR engine",               // shown in error messages
//     L"unrar.dll",                  // ';'-separated candidates, best first
//     nullptr,                       // companion DLLs, if it is split up
//     L"SOFTWARE\\WinRAR", L"exe64", L"unrar.dll" },   // optional reg hint
//
// Nothing else in the resolver needs to change: naming, bitness suffixes,
// the directory search order and the diagnostics all come from here.
// ─────────────────────────────────────────────────────────────────────────
static const Component kComponents[] =
{
    { L"7z", L"7-Zip engine", L"7z.dll;7za.dll", nullptr,
      L"SOFTWARE\\7-Zip", L"Path", L"7z.dll" },

    // Brotli ships as three DLLs. libbrotlidec is the one we call; it needs
    // libbrotlicommon beside it, and libbrotlienc only for compression.
    { L"brotli", L"Brotli codec", L"libbrotlidec.dll;brotlidec.dll",
      L"libbrotlicommon.dll;libbrotlienc.dll", nullptr, nullptr, nullptr },

    { L"lizard", L"Lizard codec", L"liblizard.dll;lizard.dll",
      nullptr, nullptr, nullptr, nullptr },

    { L"lz4", L"LZ4 codec", L"liblz4.dll;lz4.dll",
      nullptr, nullptr, nullptr, nullptr },

    { L"lz5", L"LZ5 codec", L"liblz5.dll;lz5.dll",
      nullptr, nullptr, nullptr, nullptr },

    // zstd also ships a separate XP-compatible build (libzstd.xp.<bits>.dll);
    // NameVariants() prefers it automatically when we are on XP.
    { L"zstd", L"Zstandard codec", L"libzstd.dll;zstd.dll",
      nullptr, nullptr, nullptr, nullptr },

    // wimlib's SONAME carries its ABI number, hence the -15.
    { L"WimLib", L"WimLib engine", L"libwim-15.dll;libwim.dll;wim.dll",
      nullptr, nullptr, nullptr, nullptr },

    { L"Unrar", L"UnRAR engine", L"unrar.dll",
      nullptr, L"SOFTWARE\\WinRAR", L"exe64", L"unrar.dll" },
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

// "7z.dll" → "7z.64.dll", "7z64.dll", "7z.dll" (+ the .xp. build, whose
// position in the list depends on the Windows we are running on).
static std::vector<std::wstring> NameVariants(const std::wstring& base)
{
    std::wstring stem = base, ext;
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos) { stem = base.substr(0, dot); ext = base.substr(dot); }

    const std::wstring bits = BitnessTag();
    const std::wstring xp   = stem + L".xp." + bits + ext;   // libzstd.xp.64.dll

    std::vector<std::wstring> v;
    // On XP the XP build is the only one likely to load at all, so it goes
    // first. Everywhere else it is a last resort: it works fine on modern
    // Windows, so a user who only shipped that file should still be served.
    if (SysInfo::IsXP()) v.push_back(xp);

    v.push_back(stem + L"." + bits + ext);   // liblz4.64.dll
    v.push_back(stem + bits + ext);          // unrar64.dll
    v.push_back(base);                       // 7z.dll

    if (!SysInfo::IsXP()) v.push_back(xp);
    return v;
}

std::vector<std::wstring> ProbePaths(const Component& c)
{
    std::vector<std::wstring> out;
    const std::wstring dir = ModuleDir();
    if (dir.empty()) return out;

    // Search directories, most specific first. The <bits> subdirectory
    // comes first because that is how brotli is laid out
    // (thirdparty\brotli\64\libbrotlidec.dll) and a 64-bit DLL found in a
    // "64" folder is a stronger match than a bare one further out.
    const std::wstring id   = c.id;
    const std::wstring bits = BitnessTag();
    const std::wstring subDirs[] = {
        L"\\thirdparty\\" + id + L"\\" + bits + L"\\",
        L"\\thirdparty\\" + id + L"\\",
        L"\\thirdparty\\" + bits + L"\\",
        L"\\thirdparty\\",
        L"\\" + id + L"\\" + bits + L"\\",
        L"\\" + id + L"\\",
        L"\\",
    };

    // Each platform now builds into its own folder (<Config>\x64,
    // <Config>\x32), so a single shared thirdparty\ tree sits one
    // level up rather than beside the module. Look in both: beside
    // first, because a deployed install keeps everything together.
    std::wstring parent = dir;
    const size_t slash = parent.find_last_of(L"\\/");
    if (slash != std::wstring::npos) parent.erase(slash);
    if (parent == dir) parent.clear();          // already at a root

    const std::wstring roots[] = { dir, parent };

    for (const auto& root : roots)
    {
        if (root.empty()) continue;
        for (const auto& sub : subDirs)
            for (const auto& base : SplitNames(c.baseNames))
                for (const auto& name : NameVariants(base))
                    out.push_back(root + sub + name);
    }

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

        wchar_t val[MAX_PATH + 1] = {};   // slack element, never written
        DWORD cb = sizeof(val) - sizeof(wchar_t), type = 0;
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

HMODULE LoadComponent(const wchar_t* id, std::wstring* resolvedPath)
{
    if (resolvedPath) resolvedPath->clear();

    const Component* c = Find(id);
    if (!c) return nullptr;

    const std::wstring primary = Resolve(id);
    if (primary.empty()) return nullptr;
    if (resolvedPath) *resolvedPath = primary;

    // Pull in the companions from the same folder first. Brotli needs this:
    // libbrotlidec.dll imports libbrotlicommon.dll, and although
    // LOAD_WITH_ALTERED_SEARCH_PATH would normally find it next door, doing
    // it explicitly also copes with the companion carrying a bitness suffix
    // the import table does not mention.
    if (c->companions)
    {
        std::wstring folder = primary;
        PathRemoveFileSpecW(&folder[0]);
        folder.resize(wcslen(folder.c_str()));
        if (!folder.empty() && folder.back() != L'\\') folder += L'\\';

        for (const auto& base : SplitNames(c->companions))
            for (const auto& name : NameVariants(base))
            {
                const std::wstring cand = folder + name;
                if (FileExists(cand)) { Load(cand); break; }
            }
    }

    return Load(primary);
}

} // namespace ThirdParty

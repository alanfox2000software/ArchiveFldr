// SysInfo.cpp — see SysInfo.h
#include "stdafx.h"
#include "SysInfo.h"

namespace SysInfo {

// ─────────────────────────────────────────────────────────────────────────
// Version
// ─────────────────────────────────────────────────────────────────────────
namespace {

// Declared locally rather than pulling in <winternl.h>, which drags along a
// pile of conflicting NT definitions.
typedef struct _SHELLNSE_OSVERSIONINFOW {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
} SHELLNSE_OSVERSIONINFOW;

typedef LONG (WINAPI* PfnRtlGetVersion)(SHELLNSE_OSVERSIONINFOW*);

DWORD QueryVersion()
{
    // ntdll is always already loaded; GetModuleHandle avoids a ref count we
    // would then have to manage.
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
    {
        if (auto fn = reinterpret_cast<PfnRtlGetVersion>(
                GetProcAddress(nt, "RtlGetVersion")))
        {
            SHELLNSE_OSVERSIONINFOW vi{};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (fn(&vi) == 0)
                return (DWORD)((vi.dwMajorVersion << 8) | vi.dwMinorVersion);
        }
    }

    // Pre-Vista ntdll always has RtlGetVersion, so reaching here means
    // something very unusual. Assume XP: the conservative answer, since it
    // only ever makes us avoid APIs rather than call missing ones.
    return 0x0501;
}

} // namespace

DWORD Version()
{
    static const DWORD v = QueryVersion();
    return v;
}

std::wstring Name()
{
    const DWORD v = Version();
    const wchar_t* n =
        v >= 0x0A00 ? L"Windows 10 or 11" :
        v >= 0x0603 ? L"Windows 8.1"      :
        v >= 0x0602 ? L"Windows 8"        :
        v >= 0x0601 ? L"Windows 7"        :
        v >= 0x0600 ? L"Windows Vista"    :
        v >= 0x0502 ? L"Windows XP x64 / Server 2003" :
                      L"Windows XP";

    wchar_t buf[96];
    swprintf_s(buf, 96, L"%s (%u.%u)", n, (unsigned)(v >> 8), (unsigned)(v & 0xFF));
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────
// Late binding
// ─────────────────────────────────────────────────────────────────────────
FARPROC BindRaw(const wchar_t* systemDll, const char* procName)
{
    if (!systemDll || !procName) return nullptr;

    // Already loaded? Use it. Otherwise load by full System32 path so we
    // can never be fooled by a same-named DLL sitting beside an archive.
    HMODULE h = GetModuleHandleW(systemDll);
    if (!h)
    {
        wchar_t sys[MAX_PATH] = {};
        UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return nullptr;

        std::wstring full = sys;
        if (!full.empty() && full.back() != L'\\') full += L'\\';
        full += systemDll;

        // Intentionally leaked: an OS DLL we will keep calling.
        h = LoadLibraryExW(full.c_str(), nullptr, 0);
        if (!h) return nullptr;
    }
    return GetProcAddress(h, procName);
}

// ─────────────────────────────────────────────────────────────────────────
// XP-safe stand-ins
// ─────────────────────────────────────────────────────────────────────────
LSTATUS DeleteRegTree(HKEY root, const wchar_t* subKey)
{
    // Vista+ has this in advapi32; XP does not, so it must be late bound.
    typedef LSTATUS (WINAPI* PfnRegDeleteTreeW)(HKEY, LPCWSTR);
    static const auto fn = Bind<PfnRegDeleteTreeW>(L"advapi32.dll", "RegDeleteTreeW");
    if (fn) return fn(root, subKey);

    // XP path: depth-first by hand. RegDeleteKey on XP refuses a key that
    // still has subkeys, so the children have to go first.
    HKEY hKey = nullptr;
    LSTATUS st = RegOpenKeyExW(root, subKey, 0, KEY_READ | KEY_WRITE, &hKey);
    if (st != ERROR_SUCCESS) return st;

    for (;;)
    {
        wchar_t name[256];
        DWORD   cch = ARRAYSIZE(name);
        // Always enumerate index 0: each successful delete reshuffles the
        // remaining children, so walking an index forward would skip some.
        if (RegEnumKeyExW(hKey, 0, name, &cch,
                          nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;

        LSTATUS sub = DeleteRegTree(hKey, name);
        if (sub != ERROR_SUCCESS) { RegCloseKey(hKey); return sub; }
    }

    RegCloseKey(hKey);
    return RegDeleteKeyW(root, subKey);
}

HRESULT OpenFileStreamRead(const wchar_t* path, IStream** ppStream)
{
    if (!path || !ppStream) return E_POINTER;
    *ppStream = nullptr;

    typedef HRESULT (WINAPI* PfnEx)(LPCWSTR, DWORD, DWORD, BOOL, IStream*, IStream**);
    static const auto ex = Bind<PfnEx>(L"shlwapi.dll", "SHCreateStreamOnFileEx");
    if (ex)
        return ex(path, STGM_READ | STGM_SHARE_DENY_WRITE,
                  FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, ppStream);

    // Plain XP / XP SP1: the older entry point, present since Windows 2000.
    typedef HRESULT (WINAPI* PfnW)(LPCWSTR, DWORD, IStream**);
    static const auto plain = Bind<PfnW>(L"shlwapi.dll", "SHCreateStreamOnFileW");
    if (plain)
        return plain(path, STGM_READ | STGM_SHARE_DENY_WRITE, ppStream);

    return E_NOTIMPL;
}

HRESULT GetNameFromIDList(PCIDLIST_ABSOLUTE pidl, DWORD sigdn, PWSTR* ppszName)
{
    if (!ppszName) return E_POINTER;
    *ppszName = nullptr;
    if (!pidl) return E_INVALIDARG;

    typedef HRESULT (WINAPI* PfnGetName)(PCIDLIST_ABSOLUTE, DWORD, PWSTR*);
    static const auto fn = Bind<PfnGetName>(L"shell32.dll", "SHGetNameFromIDList");
    if (fn) return fn(pidl, sigdn, ppszName);

    // ── XP ──────────────────────────────────────────────────────────────
    if (sigdn == kSigdnFileSysPath)
    {
        wchar_t path[MAX_PATH] = {};
        if (!SHGetPathFromIDListW(pidl, path) || !path[0]) return E_FAIL;
        return SHStrDupW(path, ppszName);
    }

    // Any other request becomes "ask the desktop folder to name it", which
    // yields the desktop-absolute parsing name.
    IShellFolder* desktop = nullptr;
    HRESULT hr = SHGetDesktopFolder(&desktop);
    if (FAILED(hr) || !desktop) return FAILED(hr) ? hr : E_FAIL;

    STRRET sr{};
    hr = desktop->GetDisplayNameOf(reinterpret_cast<PCUITEMID_CHILD>(pidl),
                                   SHGDN_FORPARSING, &sr);
    if (SUCCEEDED(hr))
    {
        wchar_t buf[MAX_PATH * 2] = {};
        hr = StrRetToBufW(&sr, reinterpret_cast<PCUITEMID_CHILD>(pidl),
                          buf, ARRAYSIZE(buf));
        if (SUCCEEDED(hr)) hr = SHStrDupW(buf, ppszName);
    }
    desktop->Release();
    return hr;
}

} // namespace SysInfo

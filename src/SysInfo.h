// SysInfo.h
// ─────────────────────────────────────────────────────────────────────────
// Which Windows are we actually running on, and how to reach an API that
// may not exist there.
//
// ArchiveFldr is built with a modern SDK (so Vista+ interfaces such as
// IThumbnailProvider compile) but must LOAD on everything from Windows XP
// up to Windows 11. Those two requirements only coexist if no post-XP
// export is ever bound statically: a single unresolved import in the PE
// import table stops the DLL loading at all, long before any version check
// of ours could run.
//
// So the rule in this project is:
//
//   * Anything older than XP-era: call it directly.
//   * Anything newer: go through Bind<>() here and degrade when it is null.
//
// See the XP notes in BUILD-XP.md for the toolset side of the story.
// ─────────────────────────────────────────────────────────────────────────
#pragma once
#include "stdafx.h"

namespace SysInfo {

// Packed major/minor, comparable with the _WIN32_WINNT_* constants:
//   Windows XP        0x0501      Windows 7    0x0601
//   Server 2003 / XP64 0x0502     Windows 8    0x0602
//   Vista             0x0600      Windows 8.1  0x0603
//   Windows 10 / 11   0x0A00
//
// Read through RtlGetVersion, which — unlike GetVersionEx — keeps telling
// the truth on 8.1 and later without a compatibility manifest.
DWORD Version();

inline bool AtLeast(DWORD packedMajorMinor) { return Version() >= packedMajorMinor; }

inline bool IsXP()           { return Version() <  0x0600; }  // XP / Server 2003
inline bool IsVistaOrLater() { return Version() >= 0x0600; }
inline bool IsWin7OrLater()  { return Version() >= 0x0601; }
inline bool IsWin8OrLater()  { return Version() >= 0x0602; }
inline bool IsWin10OrLater() { return Version() >= 0x0A00; }

// True on 64-bit Windows, whether this process is the 64-bit one or a
// 32-bit one running under WOW64. Late bound: IsWow64Process arrived in
// XP SP2, and a static import would stop the DLL loading on anything
// older.
inline bool Is64BitWindows()
{
#ifdef _WIN64
    return true;
#else
    typedef BOOL (WINAPI* PFNISWOW64)(HANDLE, PBOOL);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return false;
    PFNISWOW64 fn = (PFNISWOW64)GetProcAddress(k32, "IsWow64Process");
    BOOL wow = FALSE;
    return fn && fn(GetCurrentProcess(), &wow) && wow;
#endif
}

// "Windows XP", "Windows 11", … for diagnostics and the settings page.
std::wstring Name();

// ── Late binding ─────────────────────────────────────────────────────────
// Resolve an export from a System32 DLL, or nullptr. The module is loaded
// once and cached; the handle is deliberately never freed (these are OS
// DLLs that stay for the life of the process).
FARPROC BindRaw(const wchar_t* systemDll, const char* procName);

template <typename Fn>
inline Fn Bind(const wchar_t* systemDll, const char* procName)
{
    return reinterpret_cast<Fn>(BindRaw(systemDll, procName));
}

// ── XP-safe stand-ins for APIs we would otherwise import statically ──────

// RegDeleteTreeW is Vista+. Deletes the key and everything under it.
LSTATUS DeleteRegTree(HKEY root, const wchar_t* subKey);

// SHCreateStreamOnFileEx is XP SP2+; falls back to SHCreateStreamOnFile,
// which has been there since Windows 2000.
HRESULT OpenFileStreamRead(const wchar_t* path, IStream** ppStream);

// SIGDN values, spelled out because the enum itself is Vista-only and is
// not declared when the headers are pinned to XP.
constexpr DWORD kSigdnFileSysPath           = 0x80058000;
constexpr DWORD kSigdnDesktopAbsoluteParsing = 0x80028000;

// SHGetNameFromIDList is Vista+. On XP this falls back to
// SHGetPathFromIDList for a file system path, and to the desktop folder's
// own GetDisplayNameOf for a parsing name — which is what the Vista API
// does internally anyway.
// On success *ppszName is a CoTaskMemAlloc'd string the caller frees.
HRESULT GetNameFromIDList(PCIDLIST_ABSOLUTE pidl, DWORD sigdn, PWSTR* ppszName);

} // namespace SysInfo

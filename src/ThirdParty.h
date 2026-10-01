// ThirdParty.h
// ─────────────────────────────────────────────────────────────────────────
// Universal layout + loader for the external ("third party") engine DLLs
// that ShellNSE drives but does not ship: 7z.dll today, and whatever comes
// next (unrar.dll, a zip engine, a zstd engine, …).
//
// Every component follows the SAME on-disk convention, so adding one is a
// single row in kComponents[] plus the code that talks to it — never a new
// ad-hoc search routine:
//
//   <ShellNSE dir>\thirdparty\<id>\<base>.<bits>.dll     preferred
//   <ShellNSE dir>\thirdparty\<id>\<base><bits>.dll      (unrar64.dll style)
//   <ShellNSE dir>\thirdparty\<id>\<base>.dll            plain copy
//   <ShellNSE dir>\thirdparty\<base…>                    flat thirdparty dir
//   <ShellNSE dir>\<id>\<base…>                          short layout
//   <ShellNSE dir>\<base…>                               next to ShellNSE
//   …then the component's optional registry install hint.
//
// <bits> is 64 for ShellNSE.64.dll and 32 for ShellNSE.32.dll: the engine
// DLL must always match the bitness of the host process.
// ─────────────────────────────────────────────────────────────────────────
#pragma once
#include "stdafx.h"

namespace ThirdParty {

// ── One externally supplied DLL ──────────────────────────────────────────
struct Component
{
    const wchar_t* id;           // folder name under thirdparty\  e.g. L"7z"
    const wchar_t* displayName;  // shown in UI/diagnostics  e.g. L"7-Zip engine"
    const wchar_t* baseNames;    // ';'-separated file names, best first:
                                 //   L"7z.dll;7za.dll"
    const wchar_t* regKey;       // optional install hint, may be nullptr:
    const wchar_t* regValue;     //   HKLM/HKCU\<regKey>\<regValue> = folder
    const wchar_t* regFileName;  //   + this file name
};

// Table lookup. Returns nullptr for an unknown id.
const Component*               Find(const wchar_t* id);
std::vector<const Component*>  All();

// Folder that holds ShellNSE.<bits>.dll (no trailing backslash).
std::wstring   ModuleDir();
// L"64" or L"32", matching the current process.
const wchar_t* BitnessTag();

// Every candidate path Resolve() will try, in order. Useful for telling the
// user exactly where a missing DLL was looked for.
std::vector<std::wstring> ProbePaths(const Component& c);

// First candidate that exists on disk, or L"" when none does.
std::wstring Resolve(const wchar_t* id);

// Multi-line "searched here" report for message boxes / the settings page.
std::wstring DescribeSearch(const wchar_t* id);

// LoadLibraryEx with LOAD_WITH_ALTERED_SEARCH_PATH so the engine can pull in
// its own neighbouring dependencies.
HMODULE Load(const std::wstring& fullPath);

} // namespace ThirdParty

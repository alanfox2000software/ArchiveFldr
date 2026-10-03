// ThirdParty.h
// ─────────────────────────────────────────────────────────────────────────
// Universal layout + loader for the external ("third party") engine DLLs
// that ArchiveFldr drives but does not ship: 7z.dll today, and whatever comes
// next (unrar.dll, a zip engine, a zstd engine, …).
//
// Every component follows the SAME on-disk convention, so adding one is a
// single row in kComponents[] plus the code that talks to it — never a new
// ad-hoc search routine:
//
//   <ArchiveFldr dir>\thirdparty\<id>\<bits>\<base…>        brotli layout
//   <ArchiveFldr dir>\thirdparty\<id>\<base…>               most engines
//   <ArchiveFldr dir>\thirdparty\<bits>\<base…>
//   <ArchiveFldr dir>\thirdparty\<base…>                    flat thirdparty dir
//   <ArchiveFldr dir>\<id>\<base…>                          short layout
//   <ArchiveFldr dir>\<base…>                               next to ArchiveFldr
//   …then the component's optional registry install hint.
//
// and within each of those directories, for each base name:
//
//   <base>.xp.<bits>.dll   only preferred when running on XP / 2003
//   <base>.<bits>.dll      liblz4.64.dll, libzstd.32.dll
//   <base><bits>.dll       unrar64.dll
//   <base>.dll             plain copy
//
// <bits> is 64 for ArchiveFldr.64.dll and 32 for ArchiveFldr.32.dll: the engine
// DLL must always match the bitness of the host process. The .xp. variant
// exists because some projects ship a separate XP-compatible build (zstd
// does); it is tried last elsewhere, since an XP build still runs happily
// on Windows 11.
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
    const wchar_t* companions;   // ';'-separated DLLs that must be loaded
                                 // from the same folder first (brotli splits
                                 // itself across libbrotlicommon/dec/enc);
                                 // nullptr when the DLL stands alone
    const wchar_t* regKey;       // optional install hint, may be nullptr:
    const wchar_t* regValue;     //   HKLM/HKCU\<regKey>\<regValue> = folder
    const wchar_t* regFileName;  //   + this file name
};

// Table lookup. Returns nullptr for an unknown id.
const Component*               Find(const wchar_t* id);
std::vector<const Component*>  All();

// Folder that holds ArchiveFldr.<bits>.dll (no trailing backslash).
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

// Resolve a component, load any companions sitting beside it, then load the
// DLL itself. This is what engines should call: it is the only path that
// gets split libraries like brotli loaded in the right order.
// Returns nullptr if the component cannot be found or fails to load;
// `resolvedPath` (optional) receives the full path that was tried.
HMODULE LoadComponent(const wchar_t* id, std::wstring* resolvedPath = nullptr);

} // namespace ThirdParty

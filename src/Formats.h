// Formats.h
// ─────────────────────────────────────────────────────────────────────────
// Every file type ArchiveFldr claims, in one table.
//
// This used to live in four places that quietly drifted apart: the
// extension list in GUIDs.h, the pretty-name table in ArchiveEngine.cpp,
// and two copies of the registration list in Registry.cpp. Adding a format
// meant remembering all four. Now it is one row here.
//
// A row says which engine opens the file and which third-party DLL that
// engine needs (see ThirdParty.h). Nothing else in the project decides
// what a ".lz4" is.
// ─────────────────────────────────────────────────────────────────────────
#pragma once
#include "stdafx.h"

namespace Formats {

enum class EngineKind
{
    SevenZip,   // thirdparty\7z\7z.dll — multi-file archives
    Codec,      // one compressed stream holding exactly one file
    Unrar,      // thirdparty\Unrar\unrar.dll
    Wim,        // thirdparty\WimLib\libwim-15.dll
};

struct Format
{
    const wchar_t* ext;        // L".zst", lowercase, leading dot
    const wchar_t* name;       // L"Zstandard" — shown in the UI
    const wchar_t* progId;     // L"ArchiveFldr.ZstFile", or nullptr to claim
                               // the extension without owning the file type
    EngineKind     engine;     // preferred engine
    const wchar_t* component;  // ThirdParty id that engine needs
    const wchar_t* codec;      // CodecEngine id when engine == Codec
    const wchar_t* unwrapTo;   // Codec only: extension the single member
                               // gets instead of none. ".tzst" unwraps to
                               // "name.tar"; ".zst" just drops the suffix.
};

// Lookup by extension, including the dot. Case-insensitive. nullptr when
// the extension is not one of ours.
const Format* Find(const wchar_t* ext);

// Convenience for the shell side: is this a file we claim at all?
bool IsArchiveExtension(const wchar_t* ext);

// Display name for an extension, or L"Archive" when unknown.
std::wstring NameFor(const wchar_t* ext);

// Everything in the table.
std::vector<const Format*> All();

// Just the rows with a progId — the set Registry.cpp registers and, just
// as importantly, the set it unregisters. One list, so the two can never
// disagree again.
std::vector<const Format*> Registrable();

// For a single-stream codec file, the name its one member should carry:
//   "notes.txt.zst" -> "notes.txt"      (unwrapTo = nullptr)
//   "backup.tzst"   -> "backup.tar"     (unwrapTo = L".tar")
//   "blob.zst"      -> "blob"           (no inner extension to recover)
std::wstring InnerNameFor(const std::wstring& archiveFileName);

} // namespace Formats

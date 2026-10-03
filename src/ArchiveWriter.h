// ArchiveWriter.h — creating archives
//
// Every IArchiveEngine::Create in this project returns false: the read
// engines are readers. Compression is a separate job with a separate
// shape — it starts from a list of paths on disk rather than from an
// open archive — so it lives here instead of being bolted onto the
// engine interface.
//
// The backend is 7z.dll's IOutArchive, which is why the formats offered
// are whatever the user's copy of 7z.dll says it can write, asked for at
// runtime rather than hard-coded.
#pragma once
#include "stdafx.h"
#include "ArchiveEngine.h"   // ProgressFn

namespace ArchiveWriter
{

// ── What to store ────────────────────────────────────────
struct Item
{
    std::wstring diskPath;        // source on disk; empty for a pure directory entry
    std::wstring nameInArchive;   // relative path, backslash separated
    bool         isDir  = false;
    uint64_t     size   = 0;
    FILETIME     mtime  {};
    DWORD        attrib = FILE_ATTRIBUTE_NORMAL;
};

// ── How to store it ──────────────────────────────────────
struct Options
{
    std::wstring format       = L"zip";  // handler name: "zip", "7z", "tar", ...
    int          level        = 5;       // 0 = store, 9 = ultra
    bool         solid        = false;   // 7z only
    bool         encryptNames = false;   // 7z only, needs a password
    std::wstring password;               // empty = no encryption
    int          threads      = 0;       // 0 = let 7-Zip decide
};

// ── Capability ───────────────────────────────────────────
bool IsAvailable();

// Handler names 7z.dll reports as writable, in its own order.
std::vector<std::wstring> WritableFormats();
bool FormatIsWritable(const std::wstring& format);

// The handler name to use for a target file name, by extension
// ("x.tar.gz" -> "gzip"). Empty when nothing can write it.
std::wstring FormatForTargetName(const std::wstring& fileName);

// The usual extension for a handler name ("7z" -> L".7z").
std::wstring DefaultExtensionFor(const std::wstring& format);

// ── Gathering ────────────────────────────────────────────
// Walks the selection: files are stored under their own name,
// directories are walked recursively and stored with their tree. Names
// are relative to each top-level item's parent, so compressing
// C:\a\b\dir yields dir\..., not a\b\dir\...
std::vector<Item> CollectItems(const std::vector<std::wstring>& paths,
                               uint64_t* totalBytes = nullptr);

// Default output path for a selection: the single item's name plus the
// extension, or the containing folder's name when several were picked.
// Never returns a path that already exists — it appends " (2)" and so on.
std::wstring SuggestOutputPath(const std::vector<std::wstring>& paths,
                               const std::wstring& extension);

// ── Doing it ─────────────────────────────────────────────
// Writes to a temporary file next to outPath and moves it into place on
// success, so a failure part way through cannot leave a half-written
// archive where the user asked for a good one.
bool Compress(const std::wstring& outPath,
              const std::vector<Item>& items,
              const Options& opt,
              ProgressFn progress,
              std::wstring* error);

} // namespace ArchiveWriter

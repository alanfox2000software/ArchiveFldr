// ArchiveOps.h
// Shared archive-side plumbing used by the shell objects: the item context
// menu, the drag-out data object and the drop target all need the same few
// operations (resolve a PIDL back to an entry, flatten a folder, stage files
// in a temp directory, expand a dropped folder, report engine limits).
#pragma once
#include "stdafx.h"
#include "ArchiveEngine.h"

namespace ArchiveOps {

using EnginePtr = std::shared_ptr<IArchiveEngine>;

// ── Path helpers (internal archive paths always use '/') ────────────────
std::wstring Join   (const std::wstring& dir, const std::wstring& name);
std::wstring ToWin32(const std::wstring& internalPath);   // '/' → '\'

// ── Entry lookup ────────────────────────────────────────────────────────
// Find the entry called `name` directly inside `dir` ("" = archive root).
bool FindEntry(const EnginePtr& eng, const std::wstring& dir,
               const std::wstring& name, ArchiveEntry& out);

// Everything at or below `e`: files always, plus directories that have no
// files under them (so empty folders survive a copy).
void Flatten(const EnginePtr& eng, const ArchiveEntry& e,
             std::vector<ArchiveEntry>& out);

// ── Temp staging ────────────────────────────────────────────────────────
std::wstring MakeTempDir(const std::wstring& tag);
void         RemoveTree (const std::wstring& dir);

// Extract one entry under destDir, keeping its path inside the archive.
// `produced` receives the full on-disk path of the extracted file.
bool ExtractEntry(const EnginePtr& eng, const ArchiveEntry& e,
                  const std::wstring& destDir, std::wstring* produced);

// ── Capability gates (show the reason, return false when blocked) ───────
bool EnsureCanRead(HWND hwnd, const EnginePtr& eng);
bool EnsureCanAdd (HWND hwnd, const EnginePtr& eng);

// ── Data object → file system paths (CF_HDROP or a shell ID list) ───────
bool PathsFromDataObject(IDataObject* pdo, std::vector<std::wstring>& paths);

// Expand dropped roots into individual files, each with the relative name it
// should take inside the archive ("folder\\sub\\file.txt").
struct AddItem { std::wstring src; std::wstring rel; };
void ExpandForAdd(const std::vector<std::wstring>& roots,
                  std::vector<AddItem>& out);

// Directory inside the archive that `item` belongs in, given the folder the
// user dropped on. Keeps a dropped tree's shape: "docs\a\b.txt" dropped on
// "src/" targets "src/docs/a/".
std::wstring TargetDirFor(const std::wstring& baseDir, const AddItem& item);

// True when the clipboard currently holds something ExpandForAdd can use.
bool ClipboardHasFiles();

} // namespace ArchiveOps

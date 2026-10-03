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

// ── Passwords ───────────────────────────────────────────────────────────
// An archive with encrypted headers cannot even enumerate its names.
// Prompt and reopen it (up to three attempts). False means the user
// cancelled or no supplied password could open the archive.
bool EnsureOpenPassword(HWND hwnd, const EnginePtr& eng);

// Make sure the engine holds a password before encrypted items are read:
// prompt when entries advertise encryption, or after the decoder itself
// requested a password that property metadata missed. False only when the
// user cancels the prompt.
bool EnsureReadPassword(HWND hwnd, const EnginePtr& eng);

// After an extract/test failed because the password is missing or wrong
// (eng->LastErrorWasWrongPassword()), ask again. False = user gave up.
bool AskPasswordAgain(HWND hwnd, const EnginePtr& eng);

// ExtractEntry plus the password conversation around it: prompt first if
// encrypted items need one, re-prompt on a wrong password (3 tries).
bool ExtractEntryPrompting(HWND hwnd, const EnginePtr& eng,
                           const ArchiveEntry& e,
                           const std::wstring& destDir,
                           std::wstring* produced);

// ── Data object → file system paths (CF_HDROP or a shell ID list) ───────
bool PathsFromDataObject(IDataObject* pdo, std::vector<std::wstring>& paths);

// Expand dropped roots into individual files, each with the relative name it
// should take inside the archive ("folder\\sub\\file.txt").
struct AddItem { std::wstring src; std::wstring rel; };
void ExpandForAdd(const std::vector<std::wstring>& roots,
                  std::vector<AddItem>& out);

// Turn expanded drop/paste items into the writer's shape: disk path plus
// the name the file takes inside the archive (TargetDirFor(baseDir, it) +
// file name), with size, attributes and timestamp read from disk.
void BuildWriterItems(const std::vector<AddItem>& in,
                      const std::wstring& baseDir,
                      std::vector<ArchiveWriter::Item>& out);

// Directory inside the archive that `item` belongs in, given the folder the
// user dropped on. Keeps a dropped tree's shape: "docs\a\b.txt" dropped on
// "src/" targets "src/docs/a/".
std::wstring TargetDirFor(const std::wstring& baseDir, const AddItem& item);

// Compression ratio for one entry, or an em dash when the archive does not
// give us the numbers to work it out. Shared so the view column and the
// properties text can never disagree.
std::wstring FormatRatio(uint64_t uncompressed, uint64_t packed);

// Size as Explorer itself writes it in a details column: whole kilobytes,
// rounded up, grouped for the user's locale ("1,744 KB"). Explorer formats
// the standard Size column this way from PKEY_Size, so any column we draw
// ourselves has to match or the two sit side by side in different units.
std::wstring FormatSizeKB(uint64_t bytes);

// True when the clipboard currently holds something ExpandForAdd can use.
bool ClipboardHasFiles();

} // namespace ArchiveOps

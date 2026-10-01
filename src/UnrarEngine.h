// UnrarEngine.h
// ─────────────────────────────────────────────────────────────────────────
// RAR support through RARLAB's unrar.dll (thirdparty\Unrar\unrar64.dll or
// unrar.dll — see ThirdParty.h for the full search order).
//
// unrar.dll is a sequential reader, not a random-access one: you open the
// archive, then walk headers from the start, deciding for each whether to
// skip it or unpack it. There is no "give me entry 7". So this engine
// lists once into memory on Open(), and every extraction re-opens the
// archive and walks to the entries it wants — which is also exactly how
// WinRAR itself does it.
// ─────────────────────────────────────────────────────────────────────────
#pragma once
#include "stdafx.h"
#include "ArchiveEngine.h"

// Is unrar.dll present and usable (right bitness, exports we need)?
bool IsUnrarAvailable();

// Full path of the unrar.dll in use, or L"" when unresolved.
std::wstring GetUnrarPath();

class CUnrarEngine final : public IArchiveEngine
{
public:
    CUnrarEngine();
    ~CUnrarEngine() override;

    bool Open  (const std::wstring& path) override;
    bool Create(const std::wstring& path) override;
    void Close ()                         override;
    bool IsOpen()     const override { return m_open; }
    bool IsReadOnly() const override { return true; }   // unrar never writes

    std::vector<ArchiveEntry> List(const std::wstring& dirPath) override;

    bool ExtractAll (const std::wstring& destDir, ProgressFn cb) override;
    bool ExtractFile(const ArchiveEntry& e,
                     const std::wstring& destDir, ProgressFn cb) override;

    bool AddFile   (const std::wstring&, const std::wstring&, ProgressFn) override
                   { return false; }
    bool DeleteFile(const ArchiveEntry&) override { return false; }
    bool Rename    (const ArchiveEntry&, const std::wstring&) override
                   { return false; }

    bool Test(ProgressFn cb) override;

    EngineCaps GetCaps() const override;

    std::wstring GetFormatName()  const override { return L"RAR"; }
    std::wstring GetFilePath()    const override { return m_filePath; }
    std::wstring GetComment()     const override { return m_comment; }
    uint64_t     GetFileCount()   const override;
    uint64_t     GetTotalSize()   const override;
    uint64_t     GetPackedSize()  const override;

private:
    // Walk the archive once, filling m_allEntries.
    bool BuildEntryList();

    // Re-open and unpack: every entry whose fullPath is in `wanted`, or all
    // of them when `wanted` is empty. destDir empty = test only.
    bool Run(const std::set<std::wstring>& wanted,
             const std::wstring& destDir, ProgressFn cb);

    std::vector<ArchiveEntry> m_allEntries;  // flat, directories synthesised
    std::wstring              m_comment;
    bool                      m_encryptedNames = false;
};

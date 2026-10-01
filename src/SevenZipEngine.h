// SevenZipEngine.h
// Real IArchiveEngine implementation for .7z / .7zip archives, backed by
// the external 7-Zip engine DLL dropped in by the user at:
//   thirdparty\7z\7z.64.dll   (64-bit ShellNSE.64.dll)
//   thirdparty\7z\7z.32.dll   (32-bit ShellNSE.32.dll)
//
// ShellNSE itself ships NO decoder code — it only talks to 7z.dll through
// the small, stable "COM-lite" interface declared in Sdk7z.h, exactly the
// way the 7-Zip SDK's own Client7z.cpp sample does.
#pragma once
#include "stdafx.h"
#include "ArchiveEngine.h"
#include "Sdk7z.h"

// Returns true if a usable 7z.dll (matching the current process bitness)
// could be located and its CreateObject() entry point resolved.
// The module is loaded once and kept for the lifetime of the process.
bool   Is7zEngineAvailable();
// Full path that ShellNSE looked for / loaded (for diagnostics & the
// Settings → Integration page "status" readout).
std::wstring Get7zEnginePath();

class C7zArchiveEngine final : public IArchiveEngine
{
public:
    C7zArchiveEngine();
    ~C7zArchiveEngine() override;

    bool Open  (const std::wstring& path) override;
    bool Create(const std::wstring& path) override; // not supported (read-only)
    void Close ()                         override;
    bool IsOpen()      const override { return m_open; }
    bool IsReadOnly()  const override { return true; }

    std::vector<ArchiveEntry> List(const std::wstring& dirPath) override;

    bool ExtractAll (const std::wstring& destDir, ProgressFn cb) override;
    bool ExtractFile(const ArchiveEntry& e,
                     const std::wstring& destDir, ProgressFn cb) override;

    // Modification is not supported by the extraction-only engine.
    bool AddFile   (const std::wstring&, const std::wstring&, ProgressFn) override { return false; }
    bool DeleteFile(const ArchiveEntry&) override { return false; }
    bool Rename    (const ArchiveEntry&, const std::wstring&) override { return false; }

    bool Test(ProgressFn cb) override;

    // Real extraction + testing; no compressor is wired up, so adding,
    // deleting and renaming stay off (the shell greys those commands out).
    EngineCaps GetCaps() const override;

    std::wstring GetFormatName()  const override { return L"7-Zip"; }
    std::wstring GetFilePath()    const override { return m_filePath; }
    std::wstring GetComment()     const override { return L""; }
    uint64_t     GetFileCount()   const override;
    uint64_t     GetTotalSize()   const override;
    uint64_t     GetPackedSize()  const override;

    // Last human-readable error (empty when the previous call succeeded).
    const std::wstring& GetLastError() const { return m_lastError; }

private:
    bool ExtractIndices(const std::vector<UINT32>& indices,
                        const std::wstring& destDir, ProgressFn cb);
    void BuildEntryList();
    // Split each solid block's packed size across the files sharing it.
    void SpreadSolidBlockPackSizes(
        const std::vector<std::pair<size_t, uint64_t>>& blockOf);

    ComPtr<IInArchive7z>      m_archive;
    std::vector<ArchiveEntry> m_allEntries; // flat list, directories synthesized
    std::wstring              m_lastError;
};

// SevenZipEngine.h
// Real IArchiveEngine implementation for every container 7z.dll can read
// — .7z, .zip, .tar, .wim, .iso, .cab, .gz, .xz and the rest — backed by
// the external 7-Zip engine DLL dropped in by the user at:
//   thirdparty\7z\7z.64.dll   (64-bit ArchiveFldr.64.dll)
//   thirdparty\7z\7z.32.dll   (32-bit ArchiveFldr.32.dll)
//
// ArchiveFldr itself ships NO decoder code — it only talks to 7z.dll through
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
// Full path that ArchiveFldr looked for / loaded (for diagnostics & the
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

    // Per-item modification is not supported; adding goes through the
    // batch update below.
    bool AddFile   (const std::wstring&, const std::wstring&, ProgressFn) override { return false; }
    bool DeleteFile(const ArchiveEntry&) override { return false; }
    bool Rename    (const ArchiveEntry&, const std::wstring&) override { return false; }

    // Add files in one IOutArchive update pass: old items are copied by
    // the handler, new ones are compressed with `opt`. In-place updates
    // write a temp file, close, swap and reopen.
    bool AddItems(const std::vector<ArchiveWriter::Item>& items,
                  const ArchiveWriter::Options& opt,
                  const std::wstring& destPath,
                  ProgressFn cb,
                  std::wstring* err) override;

    bool Test(ProgressFn cb) override;

    // Real extraction + testing; canAdd is true when this archive's own
    // handler can update it (zip, 7z, tar, wim with a writable 7z.dll).
    EngineCaps GetCaps() const override;

    // ── Password ─────────────────────────────────────────
    void SetPassword(const std::wstring& pw) override { m_password = pw; }
    std::wstring GetPassword() const override { return m_password; }
    bool PasswordNeededToOpen() const override { return m_needPasswordToOpen; }
    bool LastErrorWasWrongPassword() const override { return m_wrongPassword; }
    bool HasEncryptedItems() const override
    {
        for (const auto& e : m_allEntries) if (e.isEncrypted) return true;
        return false;
    }

    // Handler id that opened this archive ("zip", "7z", ...), as opposed
    // to the pretty format name shown in the UI.
    std::wstring GetHandlerName() const override { return m_handlerName; }

    std::wstring GetFormatName()  const override
    { return m_formatName.empty() ? std::wstring(L"7-Zip") : m_formatName; }
    std::wstring GetFilePath()    const override { return m_filePath; }
    std::wstring GetComment()     const override { return L""; }
    uint64_t     GetFileCount()   const override;
    uint64_t     GetTotalSize()   const override;
    uint64_t     GetPackedSize()  const override;

    // Last human-readable error (empty when the previous call succeeded).
    const std::wstring& GetLastError() const { return m_lastError; }

private:
    // Display name of the handler that actually opened the file ("tar",
    // "wim", …), so the UI does not call every archive "7-Zip".
    std::wstring m_formatName;
    bool ExtractIndices(const std::vector<UINT32>& indices,
                        const std::wstring& destDir, ProgressFn cb);
    void BuildEntryList();
    // Split each solid block's packed size across the files sharing it.
    void SpreadSolidBlockPackSizes(
        const std::vector<std::pair<size_t, uint64_t>>& blockOf);

    ComPtr<IInArchive7z>      m_archive;
    std::vector<ArchiveEntry> m_allEntries; // flat list, directories synthesized
    std::wstring              m_lastError;
    std::wstring              m_password;           // for reading (see SetPassword)
    std::wstring              m_handlerName;        // "zip", "7z", ... (handler id)
    bool                      m_needPasswordToOpen = false;
    bool                      m_wrongPassword      = false;
    // Name to give the payload of a single-stream container (.bz2, .gz,
    // .xz), which carries no name of its own. Empty for every other
    // archive. Both the listing and the extract callback read this, so
    // the two cannot disagree about what the file is called.
    std::wstring              m_innerName;
};

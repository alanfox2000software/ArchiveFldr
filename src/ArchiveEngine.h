// ArchiveEngine.h
// Abstract archive engine interface + factory.
// Integrate 7-zip SDK / libarchive / minizip here.
#pragma once
#include "stdafx.h"

// ── Archive Entry ─────────────────────────────────────────
struct ArchiveEntry {
    std::wstring name;
    std::wstring fullPath;
    bool         isDirectory       = false;
    uint64_t     uncompressedSize  = 0;
    uint64_t     compressedSize    = 0;
    uint32_t     crc32             = 0;
    std::wstring compressionMethod;
    FILETIME     modifiedTime      = {};
    bool         isEncrypted       = false;

    // Index of this entry inside the backing archive engine's own item
    // table (e.g. the 7z SDK's IInArchive item index). -1 means this entry
    // has no single backing item — e.g. a directory synthesized by the
    // engine because the archive didn't store an explicit entry for it.
    int64_t      engineIndex       = -1;
};

// ── Progress callback ─────────────────────────────────────
using ProgressFn = std::function<void(int /*pct*/,
                                      const std::wstring& /*currentFile*/)>;

// ─────────────────────────────────────────────────────────
// IArchiveEngine — abstract interface
// ─────────────────────────────────────────────────────────
class IArchiveEngine
{
public:
    virtual ~IArchiveEngine() = default;

    // ── Lifecycle ────────────────────────────────────────
    virtual bool Open  (const std::wstring& path) = 0;
    virtual bool Create(const std::wstring& path) = 0;
    virtual void Close ()                         = 0;
    virtual bool IsOpen()      const              = 0;
    virtual bool IsReadOnly()  const              = 0;

    // ── Enumeration ──────────────────────────────────────
    // List entries directly inside dirPath ("" = root)
    virtual std::vector<ArchiveEntry> List(const std::wstring& dirPath) = 0;

    // ── Extraction ───────────────────────────────────────
    virtual bool ExtractAll (const std::wstring& destDir, ProgressFn cb) = 0;
    virtual bool ExtractFile(const ArchiveEntry& e,
                             const std::wstring& destDir, ProgressFn cb) = 0;

    // ── Modification ────────────────────────────────────
    virtual bool AddFile   (const std::wstring& srcPath,
                            const std::wstring& archivePath,
                            ProgressFn cb) = 0;
    virtual bool DeleteFile(const ArchiveEntry& e) = 0;
    virtual bool Rename    (const ArchiveEntry& e, const std::wstring& newName) = 0;

    // ── Integrity ────────────────────────────────────────
    virtual bool Test(ProgressFn cb) = 0;

    // ── Metadata ─────────────────────────────────────────
    virtual std::wstring GetFormatName()  const = 0;
    virtual std::wstring GetFilePath()    const = 0;
    virtual std::wstring GetComment()     const = 0;
    virtual uint64_t     GetFileCount()   const = 0;
    virtual uint64_t     GetTotalSize()   const = 0;
    virtual uint64_t     GetPackedSize()  const = 0;

    // ── Helpers ───────────────────────────────────────────
    std::wstring GetFormattedSize(uint64_t sz) const {
        wchar_t buf[32]; StrFormatByteSizeW(sz, buf, 32); return buf;
    }

protected:
    std::wstring m_filePath;
    bool         m_open     = false;
    bool         m_readOnly = false;
};

// ─────────────────────────────────────────────────────────
// Stub engine — realistic demo data
// Replace with 7-zip SDK / libarchive dispatch per format
// ─────────────────────────────────────────────────────────
class CStubArchiveEngine final : public IArchiveEngine
{
public:
    bool Open  (const std::wstring& path) override;
    bool Create(const std::wstring& path) override;
    void Close ()                         override;
    bool IsOpen()      const override { return m_open; }
    bool IsReadOnly()  const override { return m_readOnly; }

    std::vector<ArchiveEntry> List(const std::wstring& dirPath) override;

    bool ExtractAll (const std::wstring& destDir, ProgressFn cb) override;
    bool ExtractFile(const ArchiveEntry& e,
                     const std::wstring& destDir, ProgressFn cb) override;
    bool AddFile   (const std::wstring& src,
                    const std::wstring& archPath, ProgressFn cb) override;
    bool DeleteFile(const ArchiveEntry& e) override;
    bool Rename    (const ArchiveEntry& e, const std::wstring& newName) override;
    bool Test      (ProgressFn cb)         override;

    std::wstring GetFormatName()  const override;
    std::wstring GetFilePath()    const override { return m_filePath; }
    std::wstring GetComment()     const override { return m_comment; }
    uint64_t     GetFileCount()   const override;
    uint64_t     GetTotalSize()   const override;
    uint64_t     GetPackedSize()  const override;

private:
    void BuildSampleEntries();
    static std::wstring ExtToFormatName(const wchar_t* ext);

    std::vector<ArchiveEntry> m_allEntries;
    std::wstring              m_comment;
    std::wstring              m_formatName;
};

// ─────────────────────────────────────────────────────────
// Factory — detects format and returns correct engine
// In production dispatch to: 7zSDK / minizip / libarchive
// ─────────────────────────────────────────────────────────
std::shared_ptr<IArchiveEngine> CreateArchiveEngine(const std::wstring& path);
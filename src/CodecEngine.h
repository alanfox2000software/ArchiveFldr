// CodecEngine.h
// ─────────────────────────────────────────────────────────────────────────
// Engine for the single-stream codecs: Brotli, LZ4, LZ5, Lizard, Zstandard.
//
// These formats are not archives. A .zst file is one compressed stream
// holding exactly one file and nothing else — no names, no directory, no
// per-member metadata. So the view shows precisely one entry, named after
// the container with the codec suffix removed:
//
//     notes.txt.zst  ->  notes.txt
//     backup.tzst    ->  backup.tar
//
// Every codec is reached through LoadLibrary/GetProcAddress against the
// DLLs in thirdparty\ (see ThirdParty.h). Nothing is linked at build time,
// so a missing codec degrades to a clear "that DLL was not found, here is
// where I looked" rather than a load failure.
// ─────────────────────────────────────────────────────────────────────────
#pragma once
#include "stdafx.h"
#include "ArchiveEngine.h"

// True when `codecId` ("zstd", "lz4", "lz5", "lizard", "brotli") has a
// usable DLL on this machine right now.
bool IsCodecAvailable(const wchar_t* codecId);

// Full path of the DLL backing `codecId`, or L"" when unresolved.
std::wstring GetCodecPath(const wchar_t* codecId);

class CCodecEngine final : public IArchiveEngine
{
public:
    // codecId must be one of the ids used in Formats.cpp.
    explicit CCodecEngine(const wchar_t* codecId);
    ~CCodecEngine() override;

    bool Open  (const std::wstring& path) override;
    bool Create(const std::wstring& path) override;
    void Close ()                         override;
    bool IsOpen()     const override { return m_open; }
    bool IsReadOnly() const override { return m_readOnly; }

    std::vector<ArchiveEntry> List(const std::wstring& dirPath) override;

    bool ExtractAll (const std::wstring& destDir, ProgressFn cb) override;
    bool ExtractFile(const ArchiveEntry& e,
                     const std::wstring& destDir, ProgressFn cb) override;

    bool AddFile   (const std::wstring& srcPath,
                    const std::wstring& archivePath, ProgressFn cb) override;
    bool DeleteFile(const ArchiveEntry& e) override;
    bool Rename    (const ArchiveEntry& e, const std::wstring& newName) override;

    bool Test(ProgressFn cb) override;

    EngineCaps GetCaps() const override;

    std::wstring GetFormatName()  const override { return m_formatName; }
    std::wstring GetFilePath()    const override { return m_filePath; }
    std::wstring GetComment()     const override { return L""; }
    uint64_t     GetFileCount()   const override { return m_open ? 1 : 0; }
    uint64_t     GetTotalSize()   const override { return m_entry.uncompressedSize; }
    uint64_t     GetPackedSize()  const override { return m_entry.compressedSize; }

private:
    // Decompress the whole stream. `destFile` empty = test only (decode and
    // throw the bytes away). Returns false and fills m_lastError on failure.
    bool Decode(const std::wstring& destFile, ProgressFn cb,
                uint64_t outputLimit = 0);

    // Last resort for formats that do not record the original size: decode
    // the stream and count. Bounded; leaves the size unknown if too costly.
    void MeasureSize();

    std::wstring m_codecId;
    std::wstring m_formatName;
    std::wstring m_lastError;
    ArchiveEntry m_entry;      // the one and only member
    bool         m_sizeIsExact = false;   // did the header tell us the size?
};

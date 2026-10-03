// WimEngine.h
// ─────────────────────────────────────────────────────────────────────────
// Windows Imaging Format (.wim / .swm / .esd) through wimlib:
//   thirdparty\WimLib\libwim-15.64.dll
//   thirdparty\WimLib\libwim-15.32.dll
//
// A WIM is not a plain archive — it is a container of one or more *images*,
// each a complete directory tree, sharing deduplicated file data. When a
// file holds a single image its root is shown directly; when it holds
// several, each appears as a top-level folder named after the image, the
// way DISM and 7-Zip present them.
//
// Split WIMs (.swm) keep their file data spread across sibling parts.
// Opening part 1 is enough to list the contents, but extracting needs the
// rest, so the siblings are found and referenced automatically.
// ─────────────────────────────────────────────────────────────────────────
#pragma once
#include "stdafx.h"
#include "ArchiveEngine.h"

// Is a usable libwim present? (Right bitness, new enough ABI.)
bool IsWimLibAvailable();

// Full path of the libwim in use, or L"" when unresolved.
std::wstring GetWimLibPath();

class CWimEngine final : public IArchiveEngine
{
public:
    CWimEngine();
    ~CWimEngine() override;

    bool Open  (const std::wstring& path) override;
    bool Create(const std::wstring& path) override;
    void Close ()                         override;
    bool IsOpen()     const override { return m_open; }
    bool IsReadOnly() const override { return true; }

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

    std::wstring GetFormatName()  const override { return m_formatName; }
    std::wstring GetFilePath()    const override { return m_filePath; }
    std::wstring GetComment()     const override { return m_comment; }
    uint64_t     GetFileCount()   const override;
    uint64_t     GetTotalSize()   const override;
    uint64_t     GetPackedSize()  const override;

private:
    // Walk every image, filling m_allEntries.
    bool BuildEntryList();

    // Split our internal path ("2/Windows/x.dll") into the 1-based image
    // number and the WIM-side path ("\Windows\x.dll").
    bool SplitImagePath(const std::wstring& full,
                        int* image, std::wstring* wimPath) const;

    // For a .swm, reference the sibling parts so file data can be read.
    void ReferenceSplitParts();

    void*                     m_wim = nullptr;   // WIMStruct*
    std::vector<ArchiveEntry> m_allEntries;
    std::wstring              m_formatName = L"Windows image";
    std::wstring              m_comment;
    std::wstring              m_lastError;
    int                       m_imageCount = 0;
    uint64_t                  m_fileBytes  = 0;
};

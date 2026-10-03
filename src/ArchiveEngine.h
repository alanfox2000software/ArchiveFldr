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
    // False when the format does not record the original size. A raw
    // Brotli stream never does, and LZ4/LZ5/Zstandard only do when the
    // compressor chose to write it. Zero must not be shown as "0 KB".
    bool         sizeKnown         = true;
    uint64_t     compressedSize    = 0;
    // True when compressedSize is this item's share of a solid block
    // rather than a figure the archive stores for it alone.
    bool         packedIsShared    = false;
    uint32_t     crc32             = 0;
    // Whether crc32 above means anything. Several formats store no
    // per-file CRC at all — tar carries only a header checksum, WIM uses
    // SHA-1 — and for those a zero must read as "not stored", not as a
    // checksum that happens to be zero.
    bool         hasCrc            = false;
    // Hex digest for formats that use something other than CRC-32.
    std::wstring sha1;
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

// Declared in ArchiveWriter.h; named here so the engine interface can
// accept them without a circular include.
namespace ArchiveWriter { struct Item; struct Options; }

// ─────────────────────────────────────────────────────────
// EngineCaps — what the backend behind this archive can really do.
//
// Shell UI (context menus, drag & drop, the data object) asks for this
// instead of assuming: a format with no third-party DLL behind it must
// grey its commands out and say so, never silently do nothing.
// ─────────────────────────────────────────────────────────
struct EngineCaps
{
    bool canExtract = false;   // can produce real file data
    bool canAdd     = false;   // can add / update entries
    bool canDelete  = false;
    bool canRename  = false;
    bool canTest    = false;

    // True when the entries are placeholder/demo data rather than the real
    // contents of the file on disk (no engine is wired up for this format).
    bool isStub     = true;

    std::wstring engineName;         // "7-Zip"
    std::wstring backendPath;        // third-party DLL actually loaded
    std::wstring unavailableReason;  // why isStub / !canExtract, for the user
};

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

    // Add a batch of files/folders in one update pass, with compression
    // settings. `destPath` empty (or equal to GetFilePath()) updates the
    // archive in place; anything else writes the updated copy there and
    // leaves the original untouched. Engines that cannot write keep the
    // default.
    virtual bool AddItems(const std::vector<ArchiveWriter::Item>& /*items*/,
                          const ArchiveWriter::Options& /*opt*/,
                          const std::wstring& /*destPath*/,
                          ProgressFn /*cb*/,
                          std::wstring* err)
    {
        if (err) *err = L"This archive format cannot be written.";
        return false;
    }

    // ── Password ─────────────────────────────────────────
    // The password used for reading: listing an archive with encrypted
    // headers, extracting or testing encrypted items. Set before (or
    // between) operations; an engine that does not support encryption
    // ignores it.
    virtual void SetPassword(const std::wstring&) {}
    virtual std::wstring GetPassword() const { return L""; }

    // True after Open() failed because the archive's headers are
    // encrypted and the current password is missing or wrong. The UI
    // asks for a password and calls Open() again.
    virtual bool PasswordNeededToOpen() const { return false; }

    // True when the last extract/test failed in a way that points at a
    // missing or wrong password (encrypted items present). The UI asks
    // again rather than reporting plain corruption.
    virtual bool LastErrorWasWrongPassword() const { return false; }

    // Any entry flagged encrypted? (Names may be readable while the
    // data still needs a password — a plain encrypted zip.)
    virtual bool HasEncryptedItems() const { return false; }

    // Short handler id of the open archive ("zip", "7z", ...), empty when
    // unknown. This is the format key ArchiveWriter's choice lists take,
    // which is what the Add to Archive dialog shows in update mode.
    virtual std::wstring GetHandlerName() const { return L""; }

    // ── Integrity ────────────────────────────────────────
    virtual bool Test(ProgressFn cb) = 0;

    // ── Capabilities ─────────────────────────────────────
    // Conservative default: an engine that does not override this is
    // treated as a placeholder with nothing real behind it.
    virtual EngineCaps GetCaps() const
    {
        EngineCaps c;
        c.engineName = GetFormatName();
        return c;
    }

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
    EngineCaps GetCaps() const             override;

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
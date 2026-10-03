// ShellFolder.h
// Implements the core Shell Namespace Extension folder object.
// Interfaces: IShellFolder2, IPersistFolder2, IShellDetails.
// The folder is intentionally not an IDropTarget: opened archives are read-only
// from Explorer's paste/drag-in perspective.
#pragma once
#include "stdafx.h"
#include "ArchiveEngine.h"

// ── PIDL helpers ─────────────────────────────────────────
// Our item IDs are simple: a fixed header + wchar_t name + flags
#pragma pack(push, 1)
struct NSE_ITEMID {
    USHORT  cb;           // size of this structure
    BYTE    magic;        // 0xAE = our marker
    BYTE    flags;        // NSE_FLAG_*
    UINT32  crc32;        // CRC of entry
    UINT64  fileSize;     // uncompressed size
    UINT64  packedSize;   // compressed size
    FILETIME mtime;       // last modified
    // Compression method, carried in the ID itself so the details view can
    // fill the column without re-listing the archive for every row.
    // External/encrypted coder chains can be long (for example
    // "ZSTD 7zAES:19 (...)"). Keep enough text to avoid the visibly
    // unterminated method shown by the old 15-character payload.
    WCHAR   method[96];
    WCHAR   name[1];      // null-terminated name (variable length)
};
#pragma pack(pop)

#define NSE_MAGIC       0xAE
#define NSE_FLAG_DIR    0x01
#define NSE_FLAG_ENC    0x02  // encrypted
#define NSE_FLAG_SOLID  0x04  // part of solid block
#define NSE_FLAG_HASCRC 0x08  // crc32 below is a real checksum
#define NSE_FLAG_NOSIZE 0x10  // uncompressed size is unknown

// ─────────────────────────────────────────────────────────
// PIDL factory & parser
// ─────────────────────────────────────────────────────────
class CPidlMgr
{
public:
    static LPITEMIDLIST  Create(const ArchiveEntry& e);
    static LPITEMIDLIST  Clone (LPCITEMIDLIST pidl);
    static LPITEMIDLIST  Concat(LPCITEMIDLIST a, LPCITEMIDLIST b);
    static void          Free  (LPITEMIDLIST& pidl);
    static bool          IsOurs(LPCITEMIDLIST pidl);
    static const NSE_ITEMID* GetItem(LPCITEMIDLIST pidl);
    static std::wstring  GetName(LPCITEMIDLIST pidl);
    static std::wstring  GetMethod(LPCITEMIDLIST pidl);
    static bool          IsDir (LPCITEMIDLIST pidl);
    // Walk to last item in a multi-level PIDL
    static LPCITEMIDLIST  GetLast(LPCITEMIDLIST pidl);
    // Copy just the FIRST item of a multi-level PIDL.
    static LPITEMIDLIST   CloneFirst(LPCITEMIDLIST pidl);
    // "dir1\dir2\file.txt" — every one of our segments joined together.
    static std::wstring   GetChainPath(LPCITEMIDLIST pidl);
    static LPITEMIDLIST   RemoveLast(LPCITEMIDLIST pidl);
};

// ─────────────────────────────────────────────────────────
// CShellFolder
// ─────────────────────────────────────────────────────────
class CShellFolder :
    public IShellFolder2,
    public IPersistFolder2,
    public IShellDetails
{
public:
    CShellFolder();
    explicit CShellFolder(CShellFolder* pParent,
                          LPCITEMIDLIST pidlAbs,
                          LPCITEMIDLIST pidlRel,
                          std::shared_ptr<IArchiveEngine> engine,
                          const std::wstring& archivePath);

    // ── IUnknown ─────────────────────────────────────────
    STDMETHODIMP QueryInterface(REFIID, void**) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;

    // ── IPersist ─────────────────────────────────────────
    STDMETHODIMP GetClassID(CLSID*) override;

    // ── IPersistFolder ────────────────────────────────────
    STDMETHODIMP Initialize(LPCITEMIDLIST pidl) override;

    // ── IPersistFolder2 ───────────────────────────────────
    STDMETHODIMP GetCurFolder(LPITEMIDLIST*) override;

    // ── IShellFolder ─────────────────────────────────────
    STDMETHODIMP ParseDisplayName(HWND, LPBC, LPOLESTR, ULONG*, LPITEMIDLIST*, ULONG*) override;
    STDMETHODIMP EnumObjects     (HWND, DWORD, IEnumIDList**) override;
    STDMETHODIMP BindToObject    (LPCITEMIDLIST, LPBC, REFIID, void**) override;
    STDMETHODIMP BindToStorage   (LPCITEMIDLIST, LPBC, REFIID, void**) override;
    STDMETHODIMP CompareIDs      (LPARAM, LPCITEMIDLIST, LPCITEMIDLIST) override;
    STDMETHODIMP CreateViewObject(HWND, REFIID, void**) override;
    STDMETHODIMP GetAttributesOf (UINT, LPCITEMIDLIST*, SFGAOF*) override;
    STDMETHODIMP GetUIObjectOf   (HWND, UINT, LPCITEMIDLIST*, REFIID, UINT*, void**) override;
    STDMETHODIMP GetDisplayNameOf(LPCITEMIDLIST, DWORD, STRRET*) override;
    STDMETHODIMP SetNameOf       (HWND, LPCITEMIDLIST, LPCOLESTR, DWORD, LPITEMIDLIST*) override;

    // ── IShellFolder2 ────────────────────────────────────
    STDMETHODIMP GetDefaultSearchGUID(GUID*) override;
    STDMETHODIMP EnumSearches        (IEnumExtraSearch**) override;
    STDMETHODIMP GetDefaultColumn    (DWORD, ULONG*, ULONG*) override;
    STDMETHODIMP GetDefaultColumnState(UINT, SHCOLSTATEF*) override;
    STDMETHODIMP GetDetailsEx        (LPCITEMIDLIST, const SHCOLUMNID*, VARIANT*) override;
    STDMETHODIMP GetDetailsOf        (LPCITEMIDLIST, UINT, SHELLDETAILS*) override;
    STDMETHODIMP MapColumnToSCID     (UINT, SHCOLUMNID*) override;

    // ── IShellDetails ────────────────────────────────────
    STDMETHODIMP ColumnClick (UINT col) override;

    // ── Internal helpers ─────────────────────────────────
    std::shared_ptr<IArchiveEngine> GetEngine() const { return m_engine; }
    const std::wstring& GetArchivePath() const { return m_archivePath; }
    const std::wstring& GetInternalPath() const { return m_internalPath; }
    // Fully qualified PIDL of this folder (the archive, plus any sub-folder
    // inside it). Needed to build a shell ID list for the data object.
    LPCITEMIDLIST GetAbsPidl() const { return m_pidlAbs; }

private:
    ~CShellFolder();

    void BuildInternalPath();

    long     m_cRef         = 1;
    LPITEMIDLIST m_pidlAbs  = nullptr;  // absolute PIDL to this folder
    LPITEMIDLIST m_pidlRel  = nullptr;  // relative PIDL (single item)

    CShellFolder*  m_pParent    = nullptr;
    std::wstring   m_archivePath;
    std::wstring   m_internalPath;  // path inside archive e.g. "src/utils/"

    std::shared_ptr<IArchiveEngine> m_engine;

    // Column definitions
    struct ColDef { const wchar_t* name; int width; SHCOLSTATEF state; };
    static const ColDef s_cols[];
    static constexpr UINT kNumCols = 7;
};

// ─────────────────────────────────────────────────────────
// CEnumIDList — enumerates child PIDLs of a folder
// ─────────────────────────────────────────────────────────
class CEnumIDList : public IEnumIDList
{
public:
    CEnumIDList(std::vector<LPITEMIDLIST> items)
        : m_items(std::move(items)), m_pos(0), m_cRef(1)
    { InterlockedIncrement(&g_cDllRefCount); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid,IID_IUnknown)||IsEqualIID(riid,IID_IEnumIDList))
        { *ppv=this; AddRef(); return S_OK; }
        *ppv=nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG n=InterlockedDecrement(&m_cRef);
        if(!n) delete this; return n;
    }
    STDMETHODIMP Next(ULONG celt, LPITEMIDLIST* rgelt, ULONG* pcFetched) override;
    STDMETHODIMP Skip(ULONG celt) override {
        m_pos = std::min(m_pos+celt, (ULONG)m_items.size()); return S_OK;
    }
    STDMETHODIMP Reset()  override { m_pos=0; return S_OK; }
    STDMETHODIMP Clone(IEnumIDList** ppEnum) override;

private:
    ~CEnumIDList() {
        for (auto p : m_items) ILFree(p);
        InterlockedDecrement(&g_cDllRefCount);
    }
    std::vector<LPITEMIDLIST> m_items;
    ULONG m_pos;
    long  m_cRef;
};
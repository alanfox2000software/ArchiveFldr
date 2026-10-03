// DataObject.cpp — see DataObject.h
#include "stdafx.h"
#include "DataObject.h"
#include "SysInfo.h"
#include "ShellFolder.h"
#include "ArchiveOps.h"

// ─────────────────────────────────────────────────────────
// Clipboard formats (registered once)
// ─────────────────────────────────────────────────────────
namespace {

struct Formats {
    CLIPFORMAT idList, descriptorW, contents, preferredEffect,
               performedEffect, logicalEffect, pasteSucceeded;
    Formats()
        : idList         ((CLIPFORMAT)RegisterClipboardFormatW(CFSTR_SHELLIDLIST))
        , descriptorW    ((CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW))
        , contents       ((CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILECONTENTS))
        , preferredEffect((CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT))
        , performedEffect((CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT))
        , logicalEffect  ((CLIPFORMAT)RegisterClipboardFormatW(CFSTR_LOGICALPERFORMEDDROPEFFECT))
        , pasteSucceeded ((CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PASTESUCCEEDED))
    {}
};

const Formats& CF()
{
    static Formats f;
    return f;
}

HGLOBAL AllocGlobal(const void* data, SIZE_T cb)
{
    HGLOBAL h = GlobalAlloc(GHND, cb);
    if (!h) return nullptr;
    if (void* p = GlobalLock(h))
    {
        if (data) memcpy(p, data, cb);
        GlobalUnlock(h);
        return h;
    }
    GlobalFree(h);
    return nullptr;
}

// ─────────────────────────────────────────────────────────
// Minimal IEnumFORMATETC over a fixed list
// ─────────────────────────────────────────────────────────
class CEnumFormatEtc final : public IEnumFORMATETC
{
public:
    explicit CEnumFormatEtc(std::vector<FORMATETC> list)
        : m_list(std::move(list)) { InterlockedIncrement(&g_cDllRefCount); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumFORMATETC))
        { *ppv = static_cast<IEnumFORMATETC*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override
    { ULONG n = InterlockedDecrement(&m_cRef); if (!n) delete this; return n; }

    STDMETHODIMP Next(ULONG celt, FORMATETC* rgelt, ULONG* pceltFetched) override
    {
        if (!rgelt) return E_POINTER;
        // pceltFetched may only be omitted when exactly one element was
        // asked for; otherwise the caller has no way to learn how many
        // of its array elements were written.
        if (celt != 1 && !pceltFetched) return E_INVALIDARG;

        ULONG n = 0;
        while (n < celt && m_pos < m_list.size()) rgelt[n++] = m_list[m_pos++];
        if (pceltFetched) *pceltFetched = n;
        return (n == celt) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG celt) override
    {
        // S_FALSE when the end arrives first — the caller uses that to
        // stop. Returning S_OK regardless said "skipped them all" from
        // a position past the end.
        const size_t left = m_list.size() - m_pos;
        if ((size_t)celt > left) { m_pos = m_list.size(); return S_FALSE; }
        m_pos += celt;
        return S_OK;
    }
    STDMETHODIMP Reset() override { m_pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumFORMATETC** ppEnum) override
    {
        if (!ppEnum) return E_POINTER;
        auto* p = new(std::nothrow) CEnumFormatEtc(m_list);
        if (!p) return E_OUTOFMEMORY;
        p->m_pos = m_pos;
        *ppEnum = p;
        return S_OK;
    }

private:
    ~CEnumFormatEtc() { InterlockedDecrement(&g_cDllRefCount); }
    std::vector<FORMATETC> m_list;
    size_t m_pos  = 0;
    long   m_cRef = 1;
};

} // namespace

// ─────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────
CArchiveDataObject::CArchiveDataObject()
{
    InterlockedIncrement(&g_cDllRefCount);
}

CArchiveDataObject::~CArchiveDataObject()
{
    for (auto& s : m_stored) ReleaseStgMedium(&s.sm);
    for (auto p : m_pidlItems) ILFree(p);
    if (m_pidlFolder) ILFree(m_pidlFolder);
    // Everything we extracted was a private copy for this transfer.
    ArchiveOps::RemoveTree(m_tempRoot);
    InterlockedDecrement(&g_cDllRefCount);
}

HRESULT CArchiveDataObject::Create(CShellFolder* folder, HWND owner, UINT cidl,
                                   LPCITEMIDLIST* apidl, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!folder || !cidl || !apidl) return E_INVALIDARG;

    auto engine = folder->GetEngine();
    if (!engine) return E_FAIL;
    // Explorer can request IDataObject directly for drag-out, without going
    // through ArchiveFldr's Copy command. Reopen a header-encrypted sibling
    // engine (normally from the verified in-process password cache) before
    // resolving PIDLs or staging any bytes.
    if (!ArchiveOps::EnsureOpenPassword(owner, engine))
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);

    auto* p = new(std::nothrow) CArchiveDataObject();
    if (!p) return E_OUTOFMEMORY;

    p->m_engine       = engine;
    p->m_archivePath  = folder->GetArchivePath();
    p->m_internalPath = folder->GetInternalPath();
    p->m_pidlFolder   = CPidlMgr::Clone(folder->GetAbsPidl());

    const std::wstring prefix = p->m_internalPath.empty()
        ? std::wstring()
        : (p->m_internalPath.back() == L'/' ? p->m_internalPath
                                            : p->m_internalPath + L'/');

    for (UINT i = 0; i < cidl; ++i)
    {
        if (!CPidlMgr::IsOurs(apidl[i])) continue;
        p->m_pidlItems.push_back(CPidlMgr::Clone(apidl[i]));

        ArchiveEntry e;
        if (!ArchiveOps::FindEntry(engine, p->m_internalPath,
                                   CPidlMgr::GetName(apidl[i]), e))
            continue;

        p->m_roots.push_back(e);

        std::vector<ArchiveEntry> flat;
        ArchiveOps::Flatten(engine, e, flat);
        for (auto& f : flat)
        {
            // Name the item relative to the folder the user copied FROM, so
            // "dir\sub\file.txt" is recreated at the drop target.
            std::wstring rel = f.fullPath;
            if (!prefix.empty() && rel.size() > prefix.size() &&
                _wcsnicmp(rel.c_str(), prefix.c_str(), prefix.size()) == 0)
                rel = rel.substr(prefix.size());
            p->m_items.push_back({ f, ArchiveOps::ToWin32(rel), L"" });
        }
    }

    if (p->m_roots.empty()) { p->Release(); return E_FAIL; }

    // Do not put up a password dialog from IDataObject::GetData. Explorer
    // calls GetData in the middle of an OLE transfer; a modal prompt there
    // can make the destination abandon the request and report the opaque
    // "Error Copying File or Folder: Unspecified error" even after a valid
    // password was entered. Stage the whole selection now, while
    // GetUIObjectOf still gives us the source window and before OLE starts
    // asking for FILECONTENTS / CF_HDROP.  This deliberately does not trust
    // ArchiveEntry::isEncrypted: some handler versions omit kpidEncrypted,
    // and the extraction callback itself is the final authority on whether
    // a password is required.
    for (auto& item : p->m_items)
    {
        if (!p->EnsureStaged(item, owner))
        {
            // A clean failure means the user dismissed the password prompt;
            // an engine error must reach Copy so it can display the useful
            // reason instead of the same opaque message for every fault.
            const HRESULT hr = engine->GetLastErrorText().empty()
                ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : E_FAIL;
            p->Release();
            return hr;
        }
    }

    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
}

// ─────────────────────────────────────────────────────────
// IUnknown
// ─────────────────────────────────────────────────────────
STDMETHODIMP CArchiveDataObject::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDataObject))
    { *ppv = static_cast<IDataObject*>(this); AddRef(); return S_OK; }
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CArchiveDataObject::AddRef()
{ return InterlockedIncrement(&m_cRef); }
STDMETHODIMP_(ULONG) CArchiveDataObject::Release()
{ ULONG n = InterlockedDecrement(&m_cRef); if (!n) delete this; return n; }

// ─────────────────────────────────────────────────────────
// Staging (extract before OLE transfer into one private temp folder)
// ─────────────────────────────────────────────────────────
bool CArchiveDataObject::EnsureTempRoot()
{
    if (!m_tempRoot.empty()) return true;
    m_tempRoot = ArchiveOps::MakeTempDir(
        PathFindFileNameW(m_archivePath.c_str()));
    return !m_tempRoot.empty();
}

bool CArchiveDataObject::EnsureStaged(Item& it, HWND promptOwner)
{
    if (!it.staged.empty())
        return GetFileAttributesW(it.staged.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!EnsureTempRoot()) return false;

    std::wstring produced;
    // Selections are normally staged before OLE starts (see Create).
    // Keep this path usable for defensive late calls too; use the active
    // window only when the caller could not provide the source archive view.
    if (!promptOwner) promptOwner = GetActiveWindow();
    if (!ArchiveOps::ExtractEntryPrompting(promptOwner, m_engine,
                                           it.entry, m_tempRoot, &produced))
        return false;
    it.staged = produced;
    return true;
}

// ─────────────────────────────────────────────────────────
// Renderers
// ─────────────────────────────────────────────────────────
HRESULT CArchiveDataObject::RenderIdList(STGMEDIUM* pmed)
{
    if (!m_pidlFolder || m_pidlItems.empty()) return E_FAIL;

    const UINT cidl   = (UINT)m_pidlItems.size();
    const UINT offCnt = (UINT)(sizeof(UINT) * (cidl + 2));   // cidl + aoffset[]

    SIZE_T cb = offCnt + ILGetSize(m_pidlFolder);
    for (auto p : m_pidlItems) cb += ILGetSize(p);

    HGLOBAL h = AllocGlobal(nullptr, cb);
    if (!h) return E_OUTOFMEMORY;

    auto* cida = (CIDA*)GlobalLock(h);
    if (!cida) { GlobalFree(h); return E_OUTOFMEMORY; }

    cida->cidl = cidl;
    BYTE* base = (BYTE*)cida;
    UINT  off  = offCnt;

    SIZE_T n = ILGetSize(m_pidlFolder);
    cida->aoffset[0] = off;
    memcpy(base + off, m_pidlFolder, n);
    off += (UINT)n;

    for (UINT i = 0; i < cidl; ++i)
    {
        n = ILGetSize(m_pidlItems[i]);
        cida->aoffset[i + 1] = off;
        memcpy(base + off, m_pidlItems[i], n);
        off += (UINT)n;
    }
    GlobalUnlock(h);

    pmed->tymed          = TYMED_HGLOBAL;
    pmed->hGlobal        = h;
    pmed->pUnkForRelease = nullptr;
    return S_OK;
}

HRESULT CArchiveDataObject::RenderDescriptor(STGMEDIUM* pmed)
{
    if (m_items.empty()) return E_FAIL;

    const SIZE_T cb = sizeof(FILEGROUPDESCRIPTORW) +
                      sizeof(FILEDESCRIPTORW) * (m_items.size() - 1);
    HGLOBAL h = AllocGlobal(nullptr, cb);
    if (!h) return E_OUTOFMEMORY;

    auto* fgd = (FILEGROUPDESCRIPTORW*)GlobalLock(h);
    if (!fgd) { GlobalFree(h); return E_OUTOFMEMORY; }

    fgd->cItems = (UINT)m_items.size();
    for (size_t i = 0; i < m_items.size(); ++i)
    {
        const Item& it = m_items[i];
        FILEDESCRIPTORW& fd = fgd->fgd[i];

        fd.dwFlags = FD_ATTRIBUTES | FD_WRITESTIME | FD_PROGRESSUI;
        fd.dwFileAttributes = it.entry.isDirectory ? FILE_ATTRIBUTE_DIRECTORY
                                                   : FILE_ATTRIBUTE_NORMAL;
        fd.ftLastWriteTime  = it.entry.modifiedTime;

        // Only claim a size the archive actually reported. A .bz2 or raw
        // .lz4 records none, and declaring FD_FILESIZE with the zero we
        // use for "unknown" tells the shell to expect an empty file.
        if (!it.entry.isDirectory && it.entry.sizeKnown)
        {
            fd.dwFlags |= FD_FILESIZE;
            fd.nFileSizeLow  = (DWORD)(it.entry.uncompressedSize & 0xFFFFFFFFull);
            fd.nFileSizeHigh = (DWORD)(it.entry.uncompressedSize >> 32);
        }
        wcsncpy_s(fd.cFileName, it.rel.c_str(), _TRUNCATE);
    }
    GlobalUnlock(h);

    pmed->tymed          = TYMED_HGLOBAL;
    pmed->hGlobal        = h;
    pmed->pUnkForRelease = nullptr;
    return S_OK;
}

HRESULT CArchiveDataObject::RenderContents(LONG index, STGMEDIUM* pmed)
{
    // Some callers pass lindex -1 when the descriptor holds a single file.
    if (index < 0 && m_items.size() == 1) index = 0;
    if (index < 0 || (size_t)index >= m_items.size()) return DV_E_LINDEX;
    Item& it = m_items[(size_t)index];

    if (it.entry.isDirectory)
    {
        // Directories carry no content; hand back an empty stream so a
        // caller that asks anyway does not fail the whole transfer.
        IStream* pEmpty = SHCreateMemStream(nullptr, 0);
        if (!pEmpty) return E_OUTOFMEMORY;
        pmed->tymed          = TYMED_ISTREAM;
        pmed->pstm           = pEmpty;
        pmed->pUnkForRelease = nullptr;
        return S_OK;
    }

    if (!EnsureStaged(it)) return E_FAIL;

    IStream* pStm = nullptr;
    // Late bound: SHCreateStreamOnFileEx only arrived in XP SP2, and the
    // helper falls back to SHCreateStreamOnFileW below that.
    HRESULT hr = SysInfo::OpenFileStreamRead(it.staged.c_str(), &pStm);
    if (FAILED(hr)) return hr;

    pmed->tymed          = TYMED_ISTREAM;
    pmed->pstm           = pStm;
    pmed->pUnkForRelease = nullptr;
    return S_OK;
}

HRESULT CArchiveDataObject::RenderHDrop(STGMEDIUM* pmed)
{
    if (m_roots.empty()) return E_FAIL;
    if (!EnsureTempRoot()) return E_FAIL;

    // CF_HDROP has to point at real files, so everything is extracted now.
    for (auto& it : m_items)
        if (!it.entry.isDirectory && !EnsureStaged(it)) return E_FAIL;

    std::wstring list;
    for (const auto& root : m_roots)
    {
        std::wstring disk = m_tempRoot + L"\\" + ArchiveOps::ToWin32(root.fullPath);
        if (root.isDirectory)
            SHCreateDirectoryExW(nullptr, disk.c_str(), nullptr);
        if (GetFileAttributesW(disk.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        list += disk;
        list += L'\0';
    }
    if (list.empty()) return E_FAIL;
    list += L'\0';

    const SIZE_T cb = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
    HGLOBAL h = AllocGlobal(nullptr, cb);
    if (!h) return E_OUTOFMEMORY;

    auto* df = (DROPFILES*)GlobalLock(h);
    if (!df) { GlobalFree(h); return E_OUTOFMEMORY; }
    df->pFiles = sizeof(DROPFILES);
    df->fWide  = TRUE;
    memcpy((BYTE*)df + sizeof(DROPFILES), list.data(),
           list.size() * sizeof(wchar_t));
    GlobalUnlock(h);

    pmed->tymed          = TYMED_HGLOBAL;
    pmed->hGlobal        = h;
    pmed->pUnkForRelease = nullptr;
    return S_OK;
}

HRESULT CArchiveDataObject::RenderDropEffect(STGMEDIUM* pmed)
{
    // Always a copy: ArchiveFldr cannot delete the item out of the archive, so
    // offering "move" would silently lose the user's data.
    DWORD effect = DROPEFFECT_COPY;
    HGLOBAL h = AllocGlobal(&effect, sizeof(effect));
    if (!h) return E_OUTOFMEMORY;
    pmed->tymed          = TYMED_HGLOBAL;
    pmed->hGlobal        = h;
    pmed->pUnkForRelease = nullptr;
    return S_OK;
}

// ─────────────────────────────────────────────────────────
// IDataObject
// ─────────────────────────────────────────────────────────
STDMETHODIMP CArchiveDataObject::GetData(FORMATETC* pfe, STGMEDIUM* pmed)
{
    if (!pfe || !pmed) return E_POINTER;
    ZeroMemory(pmed, sizeof(*pmed));

    // Anything the shell stored on us wins (e.g. CFSTR_PERFORMEDDROPEFFECT).
    for (auto& s : m_stored)
    {
        if (s.fe.cfFormat == pfe->cfFormat &&
            (s.fe.tymed & pfe->tymed) &&
            s.fe.dwAspect == pfe->dwAspect && s.fe.lindex == pfe->lindex)
        {
            if (s.sm.tymed == TYMED_HGLOBAL)
            {
                SIZE_T cb = GlobalSize(s.sm.hGlobal);
                void* src = GlobalLock(s.sm.hGlobal);
                HGLOBAL h = AllocGlobal(src, cb);
                if (src) GlobalUnlock(s.sm.hGlobal);
                if (!h) return E_OUTOFMEMORY;
                pmed->tymed   = TYMED_HGLOBAL;
                pmed->hGlobal = h;
                return S_OK;
            }
            return DV_E_FORMATETC;
        }
    }

    const Formats& f = CF();

    if (pfe->cfFormat == f.descriptorW && (pfe->tymed & TYMED_HGLOBAL))
        return RenderDescriptor(pmed);

    if (pfe->cfFormat == f.contents && (pfe->tymed & TYMED_ISTREAM))
        return RenderContents(pfe->lindex, pmed);

    if (pfe->cfFormat == CF_HDROP && (pfe->tymed & TYMED_HGLOBAL))
        return RenderHDrop(pmed);

    if (pfe->cfFormat == f.idList && (pfe->tymed & TYMED_HGLOBAL))
        return RenderIdList(pmed);

    if (pfe->cfFormat == f.preferredEffect && (pfe->tymed & TYMED_HGLOBAL))
        return RenderDropEffect(pmed);

    return DV_E_FORMATETC;
}

STDMETHODIMP CArchiveDataObject::GetDataHere(FORMATETC*, STGMEDIUM*)
{
    return E_NOTIMPL;
}

STDMETHODIMP CArchiveDataObject::QueryGetData(FORMATETC* pfe)
{
    if (!pfe) return E_POINTER;
    const Formats& f = CF();

    if (pfe->cfFormat == f.contents)
        return (pfe->tymed & TYMED_ISTREAM) ? S_OK : DV_E_TYMED;

    if (pfe->cfFormat == f.descriptorW || pfe->cfFormat == CF_HDROP ||
        pfe->cfFormat == f.idList      || pfe->cfFormat == f.preferredEffect)
        return (pfe->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_TYMED;

    for (auto& s : m_stored)
        if (s.fe.cfFormat == pfe->cfFormat) return S_OK;

    return DV_E_FORMATETC;
}

STDMETHODIMP CArchiveDataObject::GetCanonicalFormatEtc(FORMATETC*, FORMATETC* pOut)
{
    if (pOut) ZeroMemory(pOut, sizeof(*pOut));
    return E_NOTIMPL;
}

STDMETHODIMP CArchiveDataObject::SetData(FORMATETC* pfe, STGMEDIUM* pmed,
                                          BOOL fRelease)
{
    if (!pfe || !pmed) return E_POINTER;

    // The shell writes CFSTR_PERFORMEDDROPEFFECT / "Paste Succeeded" back on
    // the source object; refusing them makes drag & drop misbehave.
    for (auto it = m_stored.begin(); it != m_stored.end(); ++it)
    {
        if (it->fe.cfFormat == pfe->cfFormat && it->fe.lindex == pfe->lindex)
        {
            ReleaseStgMedium(&it->sm);
            m_stored.erase(it);
            break;
        }
    }

    Stored s{};
    s.fe = *pfe;
    if (fRelease)
    {
        s.sm = *pmed;                 // we take ownership
    }
    else
    {
        if (pmed->tymed != TYMED_HGLOBAL) return DV_E_TYMED;
        SIZE_T cb = GlobalSize(pmed->hGlobal);
        void*  p  = GlobalLock(pmed->hGlobal);
        HGLOBAL h = AllocGlobal(p, cb);
        if (p) GlobalUnlock(pmed->hGlobal);
        if (!h) return E_OUTOFMEMORY;
        s.sm.tymed   = TYMED_HGLOBAL;
        s.sm.hGlobal = h;
    }
    m_stored.push_back(s);
    return S_OK;
}

STDMETHODIMP CArchiveDataObject::EnumFormatEtc(DWORD dwDirection,
                                                IEnumFORMATETC** ppEnum)
{
    if (!ppEnum) return E_POINTER;
    *ppEnum = nullptr;
    if (dwDirection != DATADIR_GET) return E_NOTIMPL;

    const Formats& f = CF();
    std::vector<FORMATETC> list = {
        // Virtual-file formats first. Their streams read the files staged
        // before OLE began, so password UI never appears inside GetData().
        { f.descriptorW,     nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
        { f.contents,        nullptr, DVASPECT_CONTENT,  0, TYMED_ISTREAM },
        { f.idList,          nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
        { f.preferredEffect, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
        { CF_HDROP,          nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
    };

    auto* p = new(std::nothrow) CEnumFormatEtc(std::move(list));
    if (!p) return E_OUTOFMEMORY;
    *ppEnum = p;
    return S_OK;
}

STDMETHODIMP CArchiveDataObject::DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*)
{ return OLE_E_ADVISENOTSUPPORTED; }
STDMETHODIMP CArchiveDataObject::DUnadvise(DWORD)
{ return OLE_E_ADVISENOTSUPPORTED; }
STDMETHODIMP CArchiveDataObject::EnumDAdvise(IEnumSTATDATA**)
{ return OLE_E_ADVISENOTSUPPORTED; }

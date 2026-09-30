// ShellFolder.cpp
#include "stdafx.h"
#include "ShellFolder.h"
#include "ShellView.h"
#include "ContextMenu.h"
#include "DropTarget.h"
#include "ThumbnailProvider.h"
#include "GUIDs.h"
#include "Settings.h"

// ── Column table ──────────────────────────────────────────
const CShellFolder::ColDef CShellFolder::s_cols[CShellFolder::kNumCols] = {
    { L"Name",     220, SHCOLSTATE_TYPE_STR  | SHCOLSTATE_ONBYDEFAULT },
    { L"Size",      90, SHCOLSTATE_TYPE_INT  | SHCOLSTATE_ONBYDEFAULT },
    { L"Packed",    90, SHCOLSTATE_TYPE_INT  | SHCOLSTATE_ONBYDEFAULT },
    { L"Ratio",     60, SHCOLSTATE_TYPE_STR  | SHCOLSTATE_ONBYDEFAULT },
    { L"Method",    80, SHCOLSTATE_TYPE_STR  | SHCOLSTATE_ONBYDEFAULT },
    { L"Modified", 130, SHCOLSTATE_TYPE_DATE | SHCOLSTATE_ONBYDEFAULT },
    { L"CRC-32",    80, SHCOLSTATE_TYPE_STR  | SHCOLSTATE_SECONDARYUI },
};

// ─────────────────────────────────────────────────────────
// CPidlMgr
// ─────────────────────────────────────────────────────────
LPITEMIDLIST CPidlMgr::Create(const ArchiveEntry& e)
{
    UINT nameBytes = (UINT)((e.name.size()+1)*sizeof(WCHAR));
    UINT total     = offsetof(NSE_ITEMID,name) + nameBytes + sizeof(USHORT);
    LPITEMIDLIST pidl = (LPITEMIDLIST)CoTaskMemAlloc(total + sizeof(USHORT));
    if (!pidl) return nullptr;
    ZeroMemory(pidl, total + sizeof(USHORT));

    NSE_ITEMID* item = (NSE_ITEMID*)pidl;
    item->cb        = (USHORT)total;
    item->magic     = NSE_MAGIC;
    item->flags     = e.isDirectory ? NSE_FLAG_DIR : 0;
    item->crc32     = e.crc32;
    item->fileSize  = e.uncompressedSize;
    item->packedSize= e.compressedSize;
    item->mtime     = e.modifiedTime;
    memcpy(item->name, e.name.c_str(), nameBytes);

    // Terminating zero USHORT
    USHORT* term = (USHORT*)((BYTE*)pidl + total);
    *term = 0;
    return pidl;
}

LPITEMIDLIST CPidlMgr::Clone(LPCITEMIDLIST pidl)
{
    if (!pidl) return nullptr;
    SIZE_T sz = ILGetSize(pidl);
    LPITEMIDLIST p = (LPITEMIDLIST)CoTaskMemAlloc(sz);
    if (p) memcpy(p, pidl, sz);
    return p;
}

LPITEMIDLIST CPidlMgr::Concat(LPCITEMIDLIST a, LPCITEMIDLIST b)
{
    return ILCombine(a, b);
}

void CPidlMgr::Free(LPITEMIDLIST& pidl)
{
    if (pidl) { CoTaskMemFree(pidl); pidl = nullptr; }
}

bool CPidlMgr::IsOurs(LPCITEMIDLIST pidl)
{
    if (!pidl || !pidl->mkid.cb) return false;
    return ((NSE_ITEMID*)pidl)->magic == NSE_MAGIC;
}

const NSE_ITEMID* CPidlMgr::GetItem(LPCITEMIDLIST pidl)
{
    if (!IsOurs(pidl)) return nullptr;
    return (const NSE_ITEMID*)pidl;
}

std::wstring CPidlMgr::GetName(LPCITEMIDLIST pidl)
{
    auto* item = GetItem(pidl);
    return item ? item->name : L"";
}

bool CPidlMgr::IsDir(LPCITEMIDLIST pidl)
{
    auto* item = GetItem(pidl);
    return item ? (item->flags & NSE_FLAG_DIR) != 0 : false;
}

LPCITEMIDLIST CPidlMgr::GetLast(LPCITEMIDLIST pidl)
{
    return ILFindLastID(pidl);
}

// ─────────────────────────────────────────────────────────
// CShellFolder — constructors / destructor
// ─────────────────────────────────────────────────────────
CShellFolder::CShellFolder()
    : m_cRef(1), m_pidlAbs(nullptr), m_pidlRel(nullptr)
{
    InterlockedIncrement(&g_cDllRefCount);
}

CShellFolder::CShellFolder(CShellFolder* pParent,
                            LPCITEMIDLIST pidlAbs,
                            LPCITEMIDLIST pidlRel,
                            std::shared_ptr<IArchiveEngine> engine,
                            const std::wstring& archivePath)
    : m_cRef(1),
      m_pParent(pParent),
      m_archivePath(archivePath),
      m_engine(engine)
{
    m_pidlAbs = CPidlMgr::Clone(pidlAbs);
    m_pidlRel = CPidlMgr::Clone(pidlRel);
    BuildInternalPath();
    InterlockedIncrement(&g_cDllRefCount);
}

CShellFolder::~CShellFolder()
{
    CPidlMgr::Free(m_pidlAbs);
    CPidlMgr::Free(m_pidlRel);
    InterlockedDecrement(&g_cDllRefCount);
}

void CShellFolder::BuildInternalPath()
{
    // Walk the relative PIDL chain to reconstruct "dir1/dir2/"
    m_internalPath.clear();
    if (!m_pidlAbs) return;
    LPCITEMIDLIST cur = m_pidlAbs;
    while (cur && cur->mkid.cb) {
        if (CPidlMgr::IsOurs(cur) && CPidlMgr::IsDir(cur)) {
            m_internalPath += CPidlMgr::GetName(cur) + L"/";
        }
        cur = ILNext(cur);
    }
}

// ─────────────────────────────────────────────────────────
// IUnknown
// ─────────────────────────────────────────────────────────
STDMETHODIMP CShellFolder::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (IsEqualIID(riid, IID_IUnknown)        ||
        IsEqualIID(riid, IID_IShellFolder)     ||
        IsEqualIID(riid, IID_IShellFolder2))
        *ppv = static_cast<IShellFolder2*>(this);
    else if (IsEqualIID(riid, IID_IPersist)        ||
             IsEqualIID(riid, IID_IPersistFolder)  ||
             IsEqualIID(riid, IID_IPersistFolder2))
        *ppv = static_cast<IPersistFolder2*>(this);
    else if (IsEqualIID(riid, IID_IShellDetails))
        *ppv = static_cast<IShellDetails*>(this);
    else if (IsEqualIID(riid, IID_IDropTarget))
        *ppv = static_cast<IDropTarget*>(this);
    else
        return E_NOINTERFACE;

    AddRef(); return S_OK;
}
STDMETHODIMP_(ULONG) CShellFolder::AddRef()
    { return InterlockedIncrement(&m_cRef); }
STDMETHODIMP_(ULONG) CShellFolder::Release()
    { ULONG n=InterlockedDecrement(&m_cRef); if(!n) delete this; return n; }

// ─────────────────────────────────────────────────────────
// IPersist / IPersistFolder / IPersistFolder2
// ─────────────────────────────────────────────────────────
STDMETHODIMP CShellFolder::GetClassID(CLSID* pclsid)
{
    if (!pclsid) return E_POINTER;
    *pclsid = CLSID_ShellNSEFolder; return S_OK;
}
STDMETHODIMP CShellFolder::Initialize(LPCITEMIDLIST pidl)
{
    CPidlMgr::Free(m_pidlAbs);
    m_pidlAbs = CPidlMgr::Clone(pidl);
    // Try to recover archive path from Desktop.ini or registry
    wchar_t path[MAX_PATH*2] = {};
    if (SHGetPathFromIDListW(pidl, path)) {
        m_archivePath = path;
        m_engine = CreateArchiveEngine(m_archivePath);
        if (m_engine) m_engine->Open(m_archivePath);
    }
    return S_OK;
}
STDMETHODIMP CShellFolder::GetCurFolder(LPITEMIDLIST* ppidl)
{
    if (!ppidl) return E_POINTER;
    *ppidl = m_pidlAbs ? CPidlMgr::Clone(m_pidlAbs) : ILClone(nullptr);
    return S_OK;
}

// ─────────────────────────────────────────────────────────
// IShellFolder::ParseDisplayName
// ─────────────────────────────────────────────────────────
STDMETHODIMP CShellFolder::ParseDisplayName(
    HWND /*hwnd*/, LPBC /*pbc*/, LPOLESTR pszName,
    ULONG* pchEaten, LPITEMIDLIST* ppidl, ULONG* pdwAttributes)
{
    if (!pszName || !ppidl) return E_POINTER;
    *ppidl = nullptr;
    if (pchEaten) *pchEaten = 0;

    // Find entry by name in this folder
    if (!m_engine) return E_FAIL;
    auto entries = m_engine->List(m_internalPath);
    for (auto& e : entries) {
        if (_wcsicmp(e.name.c_str(), pszName) == 0) {
            *ppidl = CPidlMgr::Create(e);
            if (pchEaten) *pchEaten = (ULONG)wcslen(pszName);
            if (pdwAttributes && *ppidl)
                GetAttributesOf(1, (LPCITEMIDLIST*)ppidl, pdwAttributes);
            return *ppidl ? S_OK : E_OUTOFMEMORY;
        }
    }
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

// ─────────────────────────────────────────────────────────
// IShellFolder::EnumObjects
// ─────────────────────────────────────────────────────────
STDMETHODIMP CShellFolder::EnumObjects(HWND /*hwnd*/, DWORD grfFlags, IEnumIDList** ppEnum)
{
    if (!ppEnum) return E_POINTER;
    *ppEnum = nullptr;

    std::vector<LPITEMIDLIST> items;
    if (m_engine) {
        auto entries = m_engine->List(m_internalPath);
        for (auto& e : entries) {
            bool isDir  = e.isDirectory;
            bool show   = (isDir  && (grfFlags & SHCONTF_FOLDERS)) ||
                          (!isDir && (grfFlags & SHCONTF_NONFOLDERS));
            if (!show) continue;
            LPITEMIDLIST p = CPidlMgr::Create(e);
            if (p) items.push_back(p);
        }
    }

    *ppEnum = new(std::nothrow) CEnumIDList(std::move(items));
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

// ─────────────────────────────────────────────────────────
// IShellFolder::BindToObject — navigate into sub-folder
// ─────────────────────────────────────────────────────────
STDMETHODIMP CShellFolder::BindToObject(
    LPCITEMIDLIST pidl, LPBC /*pbc*/, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!CPidlMgr::IsOurs(pidl)) return E_INVALIDARG;
    if (!CPidlMgr::IsDir(pidl)) return E_INVALIDARG;

    LPITEMIDLIST pidlAbs = CPidlMgr::Concat(m_pidlAbs, pidl);
    auto* pSub = new(std::nothrow) CShellFolder(
        this, pidlAbs, pidl, m_engine, m_archivePath);
    ILFree(pidlAbs);
    if (!pSub) return E_OUTOFMEMORY;
    HRESULT hr = pSub->QueryInterface(riid, ppv);
    pSub->Release();
    return hr;
}
STDMETHODIMP CShellFolder::BindToStorage(
    LPCITEMIDLIST, LPBC, REFIID, void**) { return E_NOTIMPL; }

// ─────────────────────────────────────────────────────────
// IShellFolder::CompareIDs
// ─────────────────────────────────────────────────────────
STDMETHODIMP CShellFolder::CompareIDs(
    LPARAM lParam, LPCITEMIDLIST pidl1, LPCITEMIDLIST pidl2)
{
    if (!CPidlMgr::IsOurs(pidl1) || !CPidlMgr::IsOurs(pidl2))
        return E_INVALIDARG;

    auto* a = CPidlMgr::GetItem(pidl1);
    auto* b = CPidlMgr::GetItem(pidl2);
    if (!a || !b) return E_INVALIDARG;

    int col = (int)(lParam & 0xFFFF);
    int cmp = 0;

    // Directories always sort before files
    bool aDir = (a->flags & NSE_FLAG_DIR) != 0;
    bool bDir = (b->flags & NSE_FLAG_DIR) != 0;
    if (aDir != bDir) return MAKE_HRESULT(SEVERITY_SUCCESS,0, aDir?-1:1);

    switch (col) {
    case 0: cmp = _wcsicmp(a->name, b->name); break;
    case 1: cmp = (a->fileSize  < b->fileSize)  ? -1 : (a->fileSize  > b->fileSize)  ? 1 : 0; break;
    case 2: cmp = (a->packedSize< b->packedSize) ? -1 : (a->packedSize> b->packedSize)? 1 : 0; break;
    case 5: cmp = CompareFileTime(&a->mtime, &b->mtime); break;
    default: cmp = _wcsicmp(a->name, b->name); break;
    }
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)cmp);
}

// ─────────────────────────────────────────────────────────
// IShellFolder::CreateViewObject
// ─────────────────────────────────────────────────────────
STDMETHODIMP CShellFolder::CreateViewObject(HWND hwnd, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (IsEqualIID(riid, IID_IShellView)) {
        auto* pView = new(std::nothrow) CShellView(this, hwnd);
        if (!pView) return E_OUTOFMEMORY;
        HRESULT hr = pView->QueryInterface(riid, ppv);
        pView->Release(); return hr;
    }
    if (IsEqualIID(riid, IID_IDropTarget)) {
        AddRef(); *ppv = static_cast<IDropTarget*>(this);
        return S_OK;
    }
    return E_NOINTERFACE;
}

// ─────────────────────────────────────────────────────────
// IShellFolder::GetAttributesOf
// ─────────────────────────────────────────────────────────
STDMETHODIMP CShellFolder::GetAttributesOf(
    UINT cidl, LPCITEMIDLIST* apidl, SFGAOF* rgfInOut)
{
    if (!apidl || !rgfInOut) return E_POINTER;
    SFGAOF attrs = *rgfInOut;
    SFGAOF result = 0xFFFFFFFF;

    for (UINT i = 0; i < cidl; i++) {
        if (!CPidlMgr::IsOurs(apidl[i])) { result = 0; break; }
        bool isDir = CPidlMgr::IsDir(apidl[i]);
        SFGAOF a =
            SFGAO_CANCOPY | SFGAO_CANMOVE | SFGAO_CANDELETE |
            SFGAO_CANRENAME | SFGAO_HASPROPSHEET;
        if (isDir)
            a |= SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_BROWSABLE;
        else
            a |= SFGAO_STREAM;
        result &= a;
    }
    *rgfInOut = (result & attrs);
    return S_OK;
}

// ─────────────────────────────────────────────────────────
// IShellFolder::GetUIObjectOf
// ─────────────────────────────────────────────────────────
STDMETHODIMP CShellFolder::GetUIObjectOf(
    HWND hwnd, UINT cidl, LPCITEMIDLIST* apidl,
    REFIID riid, UINT* /*prgfInOut*/, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!cidl || !apidl) return E_INVALIDARG;

    if (IsEqualIID(riid, IID_IContextMenu)) {
        auto* p = new(std::nothrow) CContextMenu();
        if (!p) return E_OUTOFMEMORY;
        p->SetFolder(this, hwnd, cidl, apidl);
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release(); return hr;
    }
    if (IsEqualIID(riid, IID_IDropTarget)) {
        auto* p = new(std::nothrow) CDropTarget();
        if (!p) return E_OUTOFMEMORY;
        p->SetFolder(this);
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release(); return hr;
    }
    if (IsEqualIID(riid, IID_IExtractIcon)) {
        // Return our custom icon extractor
        // (simplified: return system shell icon)
        return SHCreateFileExtractIconW(
            CPidlMgr::IsDir(apidl[0]) ? L"folder" : m_archivePath.c_str(),
            FILE_ATTRIBUTE_NORMAL, riid, ppv);
    }
    return E_NOINTERFACE;
}

// ─────────────────────────────────────────────────────────
// IShellFolder::GetDisplayNameOf
// ─────────────────────────────────────────────────────────
STDMETHODIMP CShellFolder::GetDisplayNameOf(
    LPCITEMIDLIST pidl, DWORD /*uFlags*/, STRRET* pName)
{
    if (!pName) return E_POINTER;
    std::wstring name = CPidlMgr::GetName(pidl);
    pName->uType = STRRET_WSTR;
    return SHStrDupW(name.c_str(), &pName->pOleStr);
}

STDMETHODIMP CShellFolder::SetNameOf(
    HWND /*hwnd*/, LPCITEMIDLIST /*pidl*/,
    LPCOLESTR /*pszName*/, DWORD /*uFlags*/, LPITEMIDLIST* ppidlOut)
{
    if (ppidlOut) *ppidlOut = nullptr;
    // TODO: rename via archive engine
    return E_NOTIMPL;
}

// ─────────────────────────────────────────────────────────
// IShellFolder2
// ─────────────────────────────────────────────────────────
STDMETHODIMP CShellFolder::GetDefaultSearchGUID(GUID* p)
    { if(p) *p=GUID_NULL; return E_NOTIMPL; }
STDMETHODIMP CShellFolder::EnumSearches(IEnumExtraSearch** p)
    { if(p) *p=nullptr; return E_NOTIMPL; }
STDMETHODIMP CShellFolder::GetDefaultColumn(DWORD, ULONG* ps, ULONG* pd)
    { if(ps)*ps=0; if(pd)*pd=0; return S_OK; }
STDMETHODIMP CShellFolder::GetDefaultColumnState(UINT col, SHCOLSTATEF* pcs)
{
    if (!pcs) return E_POINTER;
    if (col >= kNumCols) return E_INVALIDARG;
    *pcs = s_cols[col].state; return S_OK;
}
STDMETHODIMP CShellFolder::GetDetailsEx(
    LPCITEMIDLIST pidl, const SHCOLUMNID* pscid, VARIANT* pv)
{
    if (!pidl||!pscid||!pv) return E_POINTER;
    SHELLDETAILS sd; sd.str.uType = STRRET_WSTR; sd.str.pOleStr = nullptr;
    // map SCID to column index (simplified)
    for (UINT i = 0; i < kNumCols; i++) {
        SHCOLUMNID scid; MapColumnToSCID(i, &scid);
        if (IsEqualPropertyKey(*pscid, scid)) {
            HRESULT hr = GetDetailsOf(pidl, i, &sd);
            if (FAILED(hr)) return hr;
            V_VT(pv) = VT_BSTR;
            V_BSTR(pv) = sd.str.pOleStr ?
                SysAllocString(sd.str.pOleStr) : SysAllocString(L"");
            CoTaskMemFree(sd.str.pOleStr);
            return S_OK;
        }
    }
    return E_FAIL;
}

STDMETHODIMP CShellFolder::GetDetailsOf(
    LPCITEMIDLIST pidl, UINT col, SHELLDETAILS* psd)
{
    if (!psd || col >= kNumCols) return E_INVALIDARG;
    psd->fmt = LVCFMT_LEFT;
    psd->cxChar = s_cols[col].width / 8;

    if (!pidl) {
        // Column header
        psd->str.uType = STRRET_WSTR;
        return SHStrDupW(s_cols[col].name, &psd->str.pOleStr);
    }

    auto* item = CPidlMgr::GetItem(pidl);
    if (!item) return E_INVALIDARG;

    wchar_t buf[128] = {};
    switch (col) {
    case 0: // Name
        psd->str.uType = STRRET_WSTR;
        return SHStrDupW(item->name, &psd->str.pOleStr);
    case 1: // Size
        if (item->flags & NSE_FLAG_DIR) wcscpy_s(buf,L"<DIR>");
        else StrFormatByteSizeW(item->fileSize, buf, 64);
        psd->fmt = LVCFMT_RIGHT; break;
    case 2: // Packed
        if (item->flags & NSE_FLAG_DIR) wcscpy_s(buf,L"");
        else StrFormatByteSizeW(item->packedSize, buf, 64);
        psd->fmt = LVCFMT_RIGHT; break;
    case 3: { // Ratio
        double r = item->fileSize > 0 ?
            100.0*(1.0-(double)item->packedSize/item->fileSize) : 0.0;
        swprintf_s(buf,128,L"%.0f%%", r);
        psd->fmt = LVCFMT_RIGHT; break; }
    case 4: // Method - retrieved from engine
        wcscpy_s(buf, L"Deflate"); break;
    case 5: { // Modified
        SYSTEMTIME st; FILETIME lft;
        FileTimeToLocalFileTime(&item->mtime, &lft);
        FileTimeToSystemTime(&lft, &st);
        swprintf_s(buf,128,L"%04d-%02d-%02d %02d:%02d",
            st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute);
        break; }
    case 6: // CRC
        if (!(item->flags & NSE_FLAG_DIR))
            swprintf_s(buf,128,L"%08X", item->crc32);
        break;
    }
    psd->str.uType = STRRET_WSTR;
    return SHStrDupW(buf, &psd->str.pOleStr);
}

STDMETHODIMP CShellFolder::MapColumnToSCID(UINT col, SHCOLUMNID* pscid)
{
    if (!pscid || col >= kNumCols) return E_INVALIDARG;

    // ── Only use real, SDK-defined PKEYs ─────────────────
    // Custom columns (Packed, Ratio, Method, CRC) use
    // PKEY_PropList_* or a custom FMTID with our own PID.
    // We define a private FMTID for our extension columns.

    // Our private FMTID for custom ShellNSE columns:
    // {B1A2C3D4-0000-0000-ABCD-AABBCCDDEEFF}
    static const GUID FMTID_ShellNSE = {
        0xB1A2C3D4, 0x0000, 0x0000,
        { 0xAB, 0xCD, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }
    };

    // PID values for our custom columns
    enum ShellNSE_PID : ULONG {
        PID_NSE_PACKED  = 2,   // Packed size
        PID_NSE_RATIO   = 3,   // Compression ratio
        PID_NSE_METHOD  = 4,   // Compression method
        PID_NSE_CRC     = 5,   // CRC-32
    };

    switch (col)
    {
    case 0: // Name — standard
        *pscid = PKEY_ItemNameDisplay;
        break;
    case 1: // Size — standard
        *pscid = PKEY_Size;
        break;
    case 2: // Packed size — custom
        pscid->fmtid = FMTID_ShellNSE;
        pscid->pid   = PID_NSE_PACKED;
        break;
    case 3: // Ratio — custom
        pscid->fmtid = FMTID_ShellNSE;
        pscid->pid   = PID_NSE_RATIO;
        break;
    case 4: // Method — custom
        pscid->fmtid = FMTID_ShellNSE;
        pscid->pid   = PID_NSE_METHOD;
        break;
    case 5: // Modified — standard
        *pscid = PKEY_DateModified;
        break;
    case 6: // CRC-32 — custom
        pscid->fmtid = FMTID_ShellNSE;
        pscid->pid   = PID_NSE_CRC;
        break;
    default:
        return E_INVALIDARG;
    }
    return S_OK;
}

STDMETHODIMP CShellFolder::ColumnClick(UINT /*col*/) { return S_FALSE; }

DWORD m_lastEffect = DROPEFFECT_NONE;

// ─────────────────────────────────────────────────────────
// IDropTarget (folder-level — accept drops FROM Explorer)
// ─────────────────────────────────────────────────────────
STDMETHODIMP CShellFolder::DragEnter(
    IDataObject* pObj, DWORD grfKey, POINTL pt, DWORD* pdwEffect)
{
    (void)pt;
    *pdwEffect = (grfKey & MK_CONTROL) ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
    FORMATETC fe{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    *pdwEffect = SUCCEEDED(pObj->QueryGetData(&fe))
        ? *pdwEffect : DROPEFFECT_NONE;
    m_lastEffect = *pdwEffect;   // ← save it
    return S_OK;
}
STDMETHODIMP CShellFolder::DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect)
{
    (void)grfKeyState;
    (void)pt;
    if (pdwEffect) *pdwEffect = m_lastEffect;
    return S_OK;
}
STDMETHODIMP CShellFolder::DragLeave() { return S_OK; }
STDMETHODIMP CShellFolder::Drop(IDataObject* pObj,DWORD,POINTL,DWORD* pdwEffect)
{
    *pdwEffect = DROPEFFECT_NONE;
    return DropFiles(pObj);
}

HRESULT CShellFolder::DropFiles(IDataObject* pObj)
{
    FORMATETC fe{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    STGMEDIUM sm{};
    RETURN_IF_FAILED(pObj->GetData(&fe, &sm));

    HDROP hDrop = (HDROP)GlobalLock(sm.hGlobal);
    if (!hDrop) { ReleaseStgMedium(&sm); return E_FAIL; }

    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    if (m_engine) {
        for (UINT i = 0; i < count; i++) {
            wchar_t path[MAX_PATH*2] = {};
            DragQueryFileW(hDrop, i, path, MAX_PATH*2);
            m_engine->AddFile(path, m_internalPath, nullptr);
        }
    }
    GlobalUnlock(sm.hGlobal);
    ReleaseStgMedium(&sm);
    // Notify Explorer to refresh
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST, m_pidlAbs, nullptr);
    return S_OK;
}

// ─────────────────────────────────────────────────────────
// CEnumIDList::Next
// ─────────────────────────────────────────────────────────
STDMETHODIMP CEnumIDList::Next(ULONG celt, LPITEMIDLIST* rgelt, ULONG* pcFetched)
{
    if (!rgelt) return E_POINTER;
    ULONG n = 0;
    while (n < celt && m_pos < (ULONG)m_items.size()) {
        rgelt[n++] = CPidlMgr::Clone(m_items[m_pos++]);
    }
    if (pcFetched) *pcFetched = n;
    return (n == celt) ? S_OK : S_FALSE;
}
STDMETHODIMP CEnumIDList::Clone(IEnumIDList** ppEnum)
{
    if (!ppEnum) return E_POINTER;
    std::vector<LPITEMIDLIST> copy;
    copy.reserve(m_items.size());
    for (auto p : m_items) copy.push_back(CPidlMgr::Clone(p));
    auto* pNew = new(std::nothrow) CEnumIDList(std::move(copy));
    if (!pNew) return E_OUTOFMEMORY;
    pNew->m_pos = m_pos;
    *ppEnum = pNew; return S_OK;
}
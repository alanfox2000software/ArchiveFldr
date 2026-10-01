// ShellFolder.cpp
#include "stdafx.h"
#include "ShellFolder.h"
#include "ShellView.h"
#include "ContextMenu.h"
#include "DropTarget.h"
#include "DataObject.h"
#include "ArchiveOps.h"
#include "ThumbnailProvider.h"
#include "GUIDs.h"
#include "Settings.h"

// ─────────────────────────────────────────────────────────
// CFolderViewCB — the view callback handed to the Shell's default folder
// view (DefView). It is how an extension states its LAYOUT: which view mode
// an archive opens in, and that the enumeration is cheap enough to run on
// the UI thread.
//
// Only messages whose parameter contract is unambiguous are handled; the
// rest fall through to E_NOTIMPL so DefView keeps its own behaviour.
// ─────────────────────────────────────────────────────────
namespace {

class CFolderViewCB final : public IShellFolderViewCB
{
public:
    CFolderViewCB() { InterlockedIncrement(&g_cDllRefCount); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_IShellFolderViewCB))
        { *ppv = static_cast<IShellFolderViewCB*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override
    { ULONG n = InterlockedDecrement(&m_cRef); if (!n) delete this; return n; }

    STDMETHODIMP MessageSFVCB(UINT uMsg, WPARAM /*wParam*/, LPARAM lParam) override
    {
        switch (uMsg)
        {
        case SFVM_DEFVIEWMODE:
            // An archive is a table of name/size/packed/ratio/date — open in
            // Details so those columns are visible without the user asking.
            if (lParam)
            {
                *reinterpret_cast<FOLDERVIEWMODE*>(lParam) = FVM_DETAILS;
                return S_OK;
            }
            break;

        case SFVM_BACKGROUNDENUM:
            // Listing comes from an already-parsed, in-memory table.
            return S_OK;

        case SFVM_COLUMNCLICK:
            return S_FALSE;        // let DefView do the sorting

        case SFVM_WINDOWCREATED:
            return S_OK;
        }
        return E_NOTIMPL;
    }

private:
    ~CFolderViewCB() { InterlockedDecrement(&g_cDllRefCount); }
    long m_cRef = 1;
};

} // namespace

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
    if (e.isEncrypted) item->flags |= NSE_FLAG_ENC;
    wcsncpy_s(item->method, e.compressionMethod.c_str(), _TRUNCATE);
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

std::wstring CPidlMgr::GetMethod(LPCITEMIDLIST pidl)
{
    auto* item = GetItem(pidl);
    if (!item) return L"";
    // Fixed-size field: make sure a full-length value still terminates.
    wchar_t buf[ARRAYSIZE(item->method) + 1] = {};
    memcpy(buf, item->method, sizeof(item->method));
    return buf;
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

LPITEMIDLIST CPidlMgr::CloneFirst(LPCITEMIDLIST pidl)
{
    if (!pidl || !pidl->mkid.cb) return nullptr;
    const USHORT cb = pidl->mkid.cb;
    auto* p = (LPITEMIDLIST)CoTaskMemAlloc(cb + sizeof(USHORT));
    if (!p) return nullptr;
    memcpy(p, pidl, cb);
    *(USHORT*)((BYTE*)p + cb) = 0;      // terminator
    return p;
}

std::wstring CPidlMgr::GetChainPath(LPCITEMIDLIST pidl)
{
    std::wstring path;
    for (LPCITEMIDLIST cur = pidl; cur && cur->mkid.cb; cur = ILNext(cur))
    {
        if (!IsOurs(cur)) continue;
        if (!path.empty()) path += L'\\';
        path += GetName(cur);
    }
    return path;
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
    // The shell hands us the fully qualified PIDL of our junction point —
    // i.e. the archive file itself, whether we were reached by browsing into
    // it (file-as-folder registration) or through a rooted view
    // (explorer.exe /e,::{CLSID},<archive>).
    CPidlMgr::Free(m_pidlAbs);
    m_pidlAbs = CPidlMgr::Clone(pidl);
    m_internalPath.clear();

    wchar_t path[MAX_PATH * 2] = {};
    if (!SHGetPathFromIDListW(pidl, path))
    {
        // SHGetPathFromIDList is limited to MAX_PATH and to "simple" file
        // system PIDLs; fall back to the modern name API before giving up.
        PWSTR psz = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &psz)) && psz)
        {
            wcsncpy_s(path, psz, _TRUNCATE);
            CoTaskMemFree(psz);
        }
    }

    if (!path[0])
    {
        // Last resort. A rooted view ("explorer.exe /e,::{CLSID},C:\x.7z")
        // can hand us a PIDL that is the CLSID junction with the archive
        // appended, which is not a plain file system PIDL. Its desktop
        // parsing name still carries the archive path, so dig it back out
        // instead of coming up empty and showing a blank window.
        PWSTR psz = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &psz)) && psz)
        {
            std::wstring s = psz;
            CoTaskMemFree(psz);

            size_t pos = std::wstring::npos;
            for (size_t i = 0; i + 2 < s.size(); ++i)          // "X:\"
                if (iswalpha(s[i]) && s[i + 1] == L':' &&
                    (s[i + 2] == L'\\' || s[i + 2] == L'/')) { pos = i; break; }
            if (pos == std::wstring::npos)                      // "\\server\share"
                pos = s.find(L"\\\\");

            if (pos != std::wstring::npos)
            {
                std::wstring cand = s.substr(pos);
                if (PathFileExistsW(cand.c_str()))
                    wcsncpy_s(path, cand.c_str(), _TRUNCATE);
            }
        }
    }

    if (path[0])
    {
        m_archivePath = path;
        m_engine = CreateArchiveEngine(m_archivePath);
        if (m_engine) m_engine->Open(m_archivePath);
    }
    return S_OK;
}
STDMETHODIMP CShellFolder::GetCurFolder(LPITEMIDLIST* ppidl)
{
    if (!ppidl) return E_POINTER;
    *ppidl = nullptr;
    // Documented contract: when the folder has not been initialized with a
    // PIDL, hand back NULL and S_FALSE. The default Shell view calls this
    // during creation and does not expect a NULL PIDL alongside S_OK.
    if (!m_pidlAbs) return S_FALSE;
    *ppidl = CPidlMgr::Clone(m_pidlAbs);
    return *ppidl ? S_OK : E_OUTOFMEMORY;
}

// ─────────────────────────────────────────────────────────
// IShellFolder::ParseDisplayName
// ─────────────────────────────────────────────────────────
STDMETHODIMP CShellFolder::ParseDisplayName(
    HWND hwnd, LPBC pbc, LPOLESTR pszName,
    ULONG* pchEaten, LPITEMIDLIST* ppidl, ULONG* pdwAttributes)
{
    if (!pszName || !ppidl) return E_POINTER;
    *ppidl = nullptr;
    if (pchEaten) *pchEaten = 0;
    if (!m_engine) return E_FAIL;

    // The shell hands over the whole remaining path, not just one segment
    // ("sub\inner\file.txt"), and expects us to walk it. GetDisplayNameOf
    // now hands out exactly such multi-segment parsing names, so they have
    // to parse back into a PIDL or Explorer cannot resolve its own
    // address bar / breadcrumb entries.
    const std::wstring input = pszName;
    const size_t sep         = input.find_first_of(L"\\/");
    const std::wstring first = (sep == std::wstring::npos) ? input : input.substr(0, sep);
    const std::wstring rest  = (sep == std::wstring::npos) ? std::wstring()
                                                           : input.substr(sep + 1);
    if (first.empty()) return E_INVALIDARG;

    auto entries = m_engine->List(m_internalPath);
    for (auto& e : entries)
    {
        if (_wcsicmp(e.name.c_str(), first.c_str()) != 0) continue;

        LPITEMIDLIST child = CPidlMgr::Create(e);
        if (!child) return E_OUTOFMEMORY;

        if (rest.empty())
        {
            *ppidl = child;
            if (pchEaten) *pchEaten = (ULONG)input.size();
            if (pdwAttributes)
                GetAttributesOf(1, (LPCITEMIDLIST*)ppidl, pdwAttributes);
            return S_OK;
        }

        if (!e.isDirectory)
        {
            ILFree(child);
            return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        }

        // Descend and let the sub-folder parse what is left.
        IShellFolder* pSub = nullptr;
        HRESULT hr = BindToObject(child, pbc, IID_IShellFolder, (void**)&pSub);
        if (FAILED(hr) || !pSub) { ILFree(child); return FAILED(hr) ? hr : E_FAIL; }

        LPITEMIDLIST tail = nullptr;
        ULONG tailEaten = 0;
        hr = pSub->ParseDisplayName(hwnd, pbc,
                                    const_cast<LPOLESTR>(rest.c_str()),
                                    &tailEaten, &tail, pdwAttributes);
        pSub->Release();

        if (SUCCEEDED(hr) && tail)
        {
            *ppidl = ILCombine(child, tail);
            ILFree(tail);
            if (pchEaten) *pchEaten = (ULONG)input.size();
        }
        ILFree(child);
        return *ppidl ? S_OK : (FAILED(hr) ? hr : E_OUTOFMEMORY);
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
    LPCITEMIDLIST pidl, LPBC pbc, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!CPidlMgr::IsOurs(pidl)) return E_INVALIDARG;
    if (!CPidlMgr::IsDir(pidl))  return E_INVALIDARG;

    // The shell may hand over several levels at once ("dir1\\dir2"). Bind the
    // first one and let the resulting folder deal with the remainder, so the
    // child always knows its own leaf item — getting this wrong leaves the
    // view pointing at the wrong directory inside the archive.
    LPCITEMIDLIST rest = ILNext(pidl);
    const bool    multi = rest && rest->mkid.cb;

    LPITEMIDLIST first = multi ? CPidlMgr::CloneFirst(pidl) : nullptr;
    LPCITEMIDLIST leaf = multi ? (LPCITEMIDLIST)first : pidl;
    if (multi && !first) return E_OUTOFMEMORY;

    LPITEMIDLIST pidlAbs = CPidlMgr::Concat(m_pidlAbs, leaf);
    auto* pSub = new(std::nothrow) CShellFolder(
        this, pidlAbs, leaf, m_engine, m_archivePath);
    ILFree(pidlAbs);
    if (first) ILFree(first);
    if (!pSub) return E_OUTOFMEMORY;

    HRESULT hr = multi ? pSub->BindToObject(rest, pbc, riid, ppv)
                       : pSub->QueryInterface(riid, ppv);
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

    // Equal so far: the PIDLs may be multi-level ("dir\sub\file"), and the
    // Shell requires a *total* ordering over complete ID lists — comparing
    // only the first SHITEMID makes distinct items look identical, which
    // shows up as duplicated or vanishing rows in the view.
    if (cmp == 0)
    {
        LPCITEMIDLIST next1 = ILNext(pidl1);
        LPCITEMIDLIST next2 = ILNext(pidl2);
        bool more1 = next1 && next1->mkid.cb != 0;
        bool more2 = next2 && next2->mkid.cb != 0;

        if (more1 && more2) return CompareIDs(lParam, next1, next2);
        if (more1 != more2) cmp = more1 ? 1 : -1;
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

    if (IsEqualIID(riid, IID_IShellView) || IsEqualIID(riid, IID_IShellView2)) {
        // Prefer the Shell's own default folder view (DefView). It is the
        // view Explorer expects to host, and it drives this folder through
        // the IShellFolder2 methods we already implement — columns, sorting,
        // selection, the item context menu, the details/preview panes and
        // keyboard handling all come for free, which a hand-rolled list
        // control cannot provide inside an Explorer frame.
        SFV_CREATE sfv = { sizeof(sfv) };
        sfv.pshf   = static_cast<IShellFolder*>(static_cast<IShellFolder2*>(this));
        sfv.psfvcb = new(std::nothrow) CFolderViewCB();   // view layout

        IShellView* pDefView = nullptr;
        HRESULT hr = SHCreateShellFolderView(&sfv, &pDefView);
        if (sfv.psfvcb) sfv.psfvcb->Release();            // the view keeps a ref
        if (SUCCEEDED(hr) && pDefView) {
            hr = pDefView->QueryInterface(riid, ppv);
            pDefView->Release();
            if (SUCCEEDED(hr)) return hr;
        }

        // Fallback: ShellNSE's built-in view implementation.
        auto* pView = new(std::nothrow) CShellView(this, hwnd);
        if (!pView) return E_OUTOFMEMORY;
        hr = pView->QueryInterface(riid, ppv);
        pView->Release(); return hr;
    }
    // Right-click on empty space in the view: the background menu belongs to
    // the folder, not to any item (Extract all, Paste, Refresh, Info...).
    if (IsEqualIID(riid, IID_IContextMenu)  ||
        IsEqualIID(riid, IID_IContextMenu2) ||
        IsEqualIID(riid, IID_IContextMenu3)) {
        auto* p = new(std::nothrow) CContextMenu();
        if (!p) return E_OUTOFMEMORY;
        p->SetBackground(this, hwnd);
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release(); return hr;
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
    if (!rgfInOut) return E_POINTER;

    // cidl == 0 asks about this folder itself. Explorer does this while
    // deciding whether the junction can be browsed, so it has to answer
    // SFGAO_FOLDER — returning E_POINTER here (the old behaviour) made the
    // shell give up on the archive.
    if (cidl == 0 || !apidl) {
        *rgfInOut &= (SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_BROWSABLE |
                      SFGAO_DROPTARGET | SFGAO_HASPROPSHEET);
        return S_OK;
    }

    SFGAOF attrs = *rgfInOut;
    SFGAOF result = 0xFFFFFFFF;

    // Only advertise what the engine behind this archive can actually do.
    // Claiming CANDELETE/CANRENAME on a read-only engine puts live Delete
    // and Rename commands in the menu that then silently do nothing.
    EngineCaps caps;
    if (m_engine) caps = m_engine->GetCaps();

    for (UINT i = 0; i < cidl; i++) {
        // A relative PIDL may hold several levels; the attributes describe
        // the item it ends at.
        LPCITEMIDLIST leaf = CPidlMgr::GetLast(apidl[i]);
        if (!CPidlMgr::IsOurs(leaf)) { result = 0; break; }
        bool isDir = CPidlMgr::IsDir(leaf);

        SFGAOF a = SFGAO_HASPROPSHEET;
        if (caps.canExtract) a |= SFGAO_CANCOPY;   // copy == extract a copy
        if (caps.canDelete)  a |= SFGAO_CANDELETE | SFGAO_CANMOVE;
        if (caps.canRename)  a |= SFGAO_CANRENAME;

        if (isDir)
            a |= SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_BROWSABLE |
                 SFGAO_STORAGEANCESTOR;
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
        p->SetSite(hwnd);
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release(); return hr;
    }
    // Copy (Ctrl+C) and drag-OUT of the archive. Without this the shell has
    // no way to ask for the bytes, so dragging an entry to the desktop did
    // nothing at all.
    if (IsEqualIID(riid, IID_IDataObject)) {
        return CArchiveDataObject::Create(this, cidl, apidl, riid, ppv);
    }
    if (IsEqualIID(riid, IID_IExtractIconW) ||
        IsEqualIID(riid, IID_IExtractIconA)) {
        // Hand the shell the icon that matches the item's own type: pass the
        // entry name so the association lookup keys off its extension.
        // (Passing the archive path, as before, drew every row — .txt, .exe,
        // folders — with the archive's icon.)
        LPCITEMIDLIST leaf = CPidlMgr::GetLast(apidl[0]);
        const bool isDir = CPidlMgr::IsDir(leaf);
        std::wstring name = CPidlMgr::GetName(leaf);
        if (name.empty()) name = isDir ? L"folder" : L"file";
        return SHCreateFileExtractIconW(name.c_str(),
            isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
            riid, ppv);
    }
    return E_NOINTERFACE;
}

// ─────────────────────────────────────────────────────────
// IShellFolder::GetDisplayNameOf
// ─────────────────────────────────────────────────────────
// "dir1/dir2/" → "dir1\dir2" (no trailing separator)
static std::wstring InternalPathToWin32(const std::wstring& internal)
{
    std::wstring s = internal;
    while (!s.empty() && s.back() == L'/') s.pop_back();
    for (auto& ch : s) if (ch == L'/') ch = L'\\';
    return s;
}

STDMETHODIMP CShellFolder::GetDisplayNameOf(
    LPCITEMIDLIST pidl, DWORD uFlags, STRRET* pName)
{
    if (!pName) return E_POINTER;
    ZeroMemory(pName, sizeof(*pName));
    pName->uType = STRRET_WSTR;

    // An empty PIDL means "this folder". Explorer asks for it to fill in the
    // window title and the address bar of a rooted view; returning an empty
    // string (the old behaviour) left the window nameless.
    if (!pidl || pidl->mkid.cb == 0)
    {
        std::wstring self;
        if (uFlags & SHGDN_FORPARSING)
        {
            self = m_archivePath;
            std::wstring inner = InternalPathToWin32(m_internalPath);
            if (!inner.empty()) self += L"\\" + inner;
        }
        else if (m_pidlRel && CPidlMgr::IsOurs(m_pidlRel))
        {
            // Leaf, not the first segment: m_pidlRel can hold several levels.
            self = CPidlMgr::GetName(CPidlMgr::GetLast(m_pidlRel));
        }
        else
        {
            self = PathFindFileNameW(m_archivePath.c_str()); // "archive.7z"
        }
        return SHStrDupW(self.c_str(), &pName->pOleStr);
    }

    if (!CPidlMgr::IsOurs(pidl)) return E_INVALIDARG;

    // The name shown in the view is the leaf's; a relative PIDL that spans
    // several levels still has to parse back as the whole chain.
    std::wstring name = CPidlMgr::GetName(CPidlMgr::GetLast(pidl));

    // A fully qualified parsing name (SHGDN_FORPARSING without
    // SHGDN_INFOLDER) must identify the item from the desktop down, the way
    // "C:\x.zip\sub\file.txt" does for a compressed folder.
    if ((uFlags & SHGDN_FORPARSING) && !(uFlags & SHGDN_INFOLDER))
    {
        std::wstring full = m_archivePath;
        std::wstring inner = InternalPathToWin32(m_internalPath);
        if (!inner.empty()) full += L"\\" + inner;
        std::wstring chain = CPidlMgr::GetChainPath(pidl);
        if (!chain.empty()) full += L"\\" + chain;
        name.swap(full);
    }

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
    VariantInit(pv);

    const NSE_ITEMID* item = CPidlMgr::GetItem(pidl);
    if (!item) return E_INVALIDARG;

    // Find which of our columns was asked for.
    UINT col = kNumCols;
    for (UINT i = 0; i < kNumCols; i++) {
        SHCOLUMNID scid; MapColumnToSCID(i, &scid);
        if (IsEqualPropertyKey(*pscid, scid)) { col = i; break; }
    }
    if (col >= kNumCols) return E_FAIL;

    // Hand back a TYPED value wherever one exists: the view sorts, groups
    // and filters on these, so a size returned as text sorts "10 KB" before
    // "9 KB" and a date cannot be grouped at all.
    const bool isDir = (item->flags & NSE_FLAG_DIR) != 0;
    switch (col)
    {
    case 1:                                   // Size
        if (isDir) return S_FALSE;
        V_VT(pv)  = VT_UI8;
        V_UI8(pv) = item->fileSize;
        return S_OK;

    case 2:                                   // Packed size
        if (isDir) return S_FALSE;
        V_VT(pv)  = VT_UI8;
        V_UI8(pv) = item->packedSize;
        return S_OK;

    case 5:                                   // Modified
    {
        SYSTEMTIME st{};
        DOUBLE     date = 0;
        if (!FileTimeToSystemTime(&item->mtime, &st) || st.wYear <= 1601)
            return S_FALSE;
        if (!SystemTimeToVariantTime(&st, &date)) return S_FALSE;
        V_VT(pv)   = VT_DATE;
        V_DATE(pv) = date;
        return S_OK;
    }

    default:
        break;
    }

    // Everything else is genuinely textual (name, ratio, method, CRC).
    SHELLDETAILS sd{}; sd.str.uType = STRRET_WSTR; sd.str.pOleStr = nullptr;
    HRESULT hr = GetDetailsOf(pidl, col, &sd);
    if (FAILED(hr)) return hr;
    V_VT(pv)   = VT_BSTR;
    V_BSTR(pv) = SysAllocString(sd.str.pOleStr ? sd.str.pOleStr : L"");
    CoTaskMemFree(sd.str.pOleStr);
    return S_OK;
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
        if (item->flags & NSE_FLAG_DIR) { psd->fmt = LVCFMT_RIGHT; break; }
        std::wstring r = ArchiveOps::FormatRatio(item->fileSize,
                                                 item->packedSize);
        wcsncpy_s(buf, r.c_str(), _TRUNCATE);
        psd->fmt = LVCFMT_RIGHT; break; }
    case 4: { // Method — carried in the item ID (see NSE_ITEMID::method)
        std::wstring m = CPidlMgr::GetMethod(pidl);
        if (m.empty() && !(item->flags & NSE_FLAG_DIR)) m = L"Store";
        wcsncpy_s(buf, m.c_str(), _TRUNCATE);
        break; }
    case 5: { // Modified
        SYSTEMTIME st{}; FILETIME lft{};
        if (FileTimeToLocalFileTime(&item->mtime, &lft) &&
            FileTimeToSystemTime(&lft, &st) && st.wYear > 1601)
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

// ─────────────────────────────────────────────────────────
// IDropTarget (folder-level — accept drops FROM Explorer)
// ─────────────────────────────────────────────────────────
STDMETHODIMP CShellFolder::DragEnter(
    IDataObject* pObj, DWORD /*grfKey*/, POINTL pt, DWORD* pdwEffect)
{
    (void)pt;
    if (!pdwEffect) return E_POINTER;
    // Dropping into an archive is always a COPY: the engine cannot promise
    // the data landed, so the source must never delete its originals.
    m_lastEffect = ArchiveDrop::EffectFor(pObj);
    *pdwEffect   = m_lastEffect;
    return S_OK;
}
STDMETHODIMP CShellFolder::DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect)
{
    (void)grfKeyState;
    (void)pt;
    if (pdwEffect) *pdwEffect = m_lastEffect;
    return S_OK;
}
STDMETHODIMP CShellFolder::DragLeave()
{
    m_lastEffect = DROPEFFECT_NONE;
    return S_OK;
}
STDMETHODIMP CShellFolder::Drop(IDataObject* pObj,DWORD,POINTL,DWORD* pdwEffect)
{
    if (!pdwEffect) return E_POINTER;
    HRESULT hr = ArchiveDrop::Perform(nullptr, this, pObj);
    *pdwEffect = (hr == S_OK) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    m_lastEffect = DROPEFFECT_NONE;
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
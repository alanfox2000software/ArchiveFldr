// DropTarget.cpp
#include "stdafx.h"
#include "DropTarget.h"
#include "ShellFolder.h"
#include "ArchiveEngine.h"
#include "ArchiveOps.h"
#include "ArchiveWriter.h"
#include "AddToArchiveDialog.h"
#include "GUIDs.h"

// ─────────────────────────────────────────────────────────
// Shared drop logic
// ─────────────────────────────────────────────────────────
namespace ArchiveDrop {

DWORD EffectFor(IDataObject* pdo)
{
    if (!pdo) return DROPEFFECT_NONE;

    static const CLIPFORMAT cfIdList =
        (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_SHELLIDLIST);

    FORMATETC hdrop { CF_HDROP,  nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    FORMATETC idl   { cfIdList,  nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };

    const bool hasFiles = SUCCEEDED(pdo->QueryGetData(&hdrop)) ||
                          SUCCEEDED(pdo->QueryGetData(&idl));

    // Copy only — see the note in DropTarget.h.
    return hasFiles ? DROPEFFECT_COPY : DROPEFFECT_NONE;
}

HRESULT Perform(HWND hwnd, CShellFolder* folder, IDataObject* pdo)
{
    if (!folder || !pdo) return E_INVALIDARG;

    auto engine = folder->GetEngine();
    if (!ArchiveOps::EnsureCanAdd(hwnd, engine))
        return S_FALSE;                       // explained to the user already

    std::vector<std::wstring> roots;
    if (!ArchiveOps::PathsFromDataObject(pdo, roots) || roots.empty())
        return S_FALSE;

    // Refuse to add the archive to itself — it would either deadlock the
    // engine or quietly grow forever.
    const std::wstring& self = folder->GetArchivePath();
    roots.erase(std::remove_if(roots.begin(), roots.end(),
        [&](const std::wstring& p) { return _wcsicmp(p.c_str(), self.c_str()) == 0; }),
        roots.end());
    if (roots.empty()) return S_FALSE;

    std::vector<ArchiveOps::AddItem> expanded;
    ArchiveOps::ExpandForAdd(roots, expanded);
    if (expanded.empty()) return S_FALSE;

    std::vector<ArchiveWriter::Item> items;
    ArchiveOps::BuildWriterItems(expanded, folder->GetInternalPath(), items);
    if (items.empty()) return S_FALSE;

    // Same window as pasting: the user confirms (or adjusts) level,
    // method and password before anything is written.
    AddToArchiveDialog::Request rq;
    rq.path       = self;
    rq.format     = engine->GetHandlerName();
    rq.lockFormat = true;
    rq.fileCount  = items.size();

    AddToArchiveDialog::Result res;
    if (!AddToArchiveDialog::Show(hwnd, rq, res)) return S_FALSE;

    HCURSOR prev = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring err;
    const bool ok = engine->AddItems(items, res.opt, res.path, nullptr, &err);
    SetCursor(prev);

    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSH,
                   folder->GetAbsPidl(), nullptr);

    if (!ok)
        MessageBoxW(hwnd,
            (L"The files could not be added to the archive.\n\n" + err).c_str(),
            L"ArchiveFldr", MB_ICONWARNING | MB_OK);
    else if (!err.empty())   // succeeded, but some sources were skipped
        MessageBoxW(hwnd, err.c_str(), L"ArchiveFldr", MB_ICONWARNING | MB_OK);

    return ok ? S_OK : S_FALSE;
}

} // namespace ArchiveDrop

// ─────────────────────────────────────────────────────────
// CDropTarget
// ─────────────────────────────────────────────────────────
CDropTarget::CDropTarget()  { InterlockedIncrement(&g_cDllRefCount); }
CDropTarget::~CDropTarget()
{
    if (m_pDataObj) m_pDataObj->Release();
    if (m_pFolder) { m_pFolder->Release(); m_pFolder = nullptr; }
    InterlockedDecrement(&g_cDllRefCount);
}

void CDropTarget::SetFolder(CShellFolder* pFolder)
{
    if (m_pFolder) m_pFolder->Release();
    m_pFolder = pFolder;
    if (m_pFolder) m_pFolder->AddRef();
}

STDMETHODIMP CDropTarget::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER; *ppv = nullptr;
    if (IsEqualIID(riid,IID_IUnknown)||IsEqualIID(riid,IID_IDropTarget))
        *ppv = static_cast<IDropTarget*>(this);
    else if (IsEqualIID(riid,IID_IPersist)||IsEqualIID(riid,IID_IPersistFile))
        *ppv = static_cast<IPersistFile*>(this);
    else
        return E_NOINTERFACE;

    AddRef(); return S_OK;
}
STDMETHODIMP_(ULONG) CDropTarget::AddRef()
    { return InterlockedIncrement(&m_cRef); }
STDMETHODIMP_(ULONG) CDropTarget::Release()
    { ULONG n=InterlockedDecrement(&m_cRef); if(!n) delete this; return n; }

// ─────────────────────────────────────────────────────────
// IPersistFile — how the shell tells a DropHandler which file it is
// standing in for. Called once, before the first DragEnter.
// ─────────────────────────────────────────────────────────
STDMETHODIMP CDropTarget::GetClassID(CLSID* pClassID)
{
    if (!pClassID) return E_POINTER;
    *pClassID = CLSID_ArchiveFldrDropTarget;
    return S_OK;
}

STDMETHODIMP CDropTarget::IsDirty()
{
    // Nothing here is ever edited, so there is never anything to save.
    return S_FALSE;
}

STDMETHODIMP CDropTarget::Load(LPCOLESTR pszFileName, DWORD /*dwMode*/)
{
    if (!pszFileName || !*pszFileName) return E_INVALIDARG;

    m_archive = pszFileName;

    // Build the same folder object the namespace extension uses, so a
    // drop onto an archive's icon and a drop into an open archive window
    // run through identical code — including the engine's "can this
    // format be written to?" check.
    LPITEMIDLIST pidl = ILCreateFromPathW(pszFileName);
    if (!pidl) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    auto* folder = new (std::nothrow) CShellFolder();
    if (!folder) { ILFree(pidl); return E_OUTOFMEMORY; }

    HRESULT hr = folder->Initialize(pidl);
    ILFree(pidl);

    if (SUCCEEDED(hr) && folder->GetEngine())
    {
        SetFolder(folder);     // takes its own reference
    }
    else
    {
        // No engine for this file: say so now rather than accepting a
        // drag and discarding it at the end.
        hr = FAILED(hr) ? hr : E_FAIL;
    }

    folder->Release();
    return hr;
}

STDMETHODIMP CDropTarget::Save(LPCOLESTR, BOOL)
{
    return E_NOTIMPL;          // a drop handler is never asked to save
}

STDMETHODIMP CDropTarget::SaveCompleted(LPCOLESTR)
{
    return S_OK;
}

STDMETHODIMP CDropTarget::GetCurFile(LPOLESTR* ppszFileName)
{
    if (!ppszFileName) return E_POINTER;
    *ppszFileName = nullptr;
    if (m_archive.empty()) return S_FALSE;

    const size_t cb = (m_archive.size() + 1) * sizeof(wchar_t);
    auto* out = static_cast<LPOLESTR>(CoTaskMemAlloc(cb));
    if (!out) return E_OUTOFMEMORY;
    memcpy(out, m_archive.c_str(), cb);
    *ppszFileName = out;
    return S_OK;
}

STDMETHODIMP CDropTarget::DragEnter(
    IDataObject* pObj, DWORD /*grfKey*/, POINTL, DWORD* pdwEffect)
{
    if (!pdwEffect) return E_POINTER;

    if (m_pDataObj) { m_pDataObj->Release(); m_pDataObj = nullptr; }
    m_pDataObj = pObj;
    if (m_pDataObj) m_pDataObj->AddRef();

    // Nothing to drop into: refuse the drag instead of showing a copy
    // cursor over a target that will discard the payload.
    if (!m_pFolder)
    {
        m_lastEffect = DROPEFFECT_NONE;
        *pdwEffect   = DROPEFFECT_NONE;
        return S_OK;
    }

    m_lastEffect = ArchiveDrop::EffectFor(pObj) & *pdwEffect;
    // The source may only be offering MOVE/LINK; we still take a copy.
    if (!m_lastEffect && ArchiveDrop::EffectFor(pObj))
        m_lastEffect = DROPEFFECT_COPY;

    *pdwEffect = m_lastEffect;
    return S_OK;
}

STDMETHODIMP CDropTarget::DragOver(DWORD /*grfKey*/, POINTL, DWORD* pdwEffect)
{
    if (!pdwEffect) return E_POINTER;
    *pdwEffect = m_lastEffect;
    return S_OK;
}

STDMETHODIMP CDropTarget::DragLeave()
{
    if (m_pDataObj) { m_pDataObj->Release(); m_pDataObj = nullptr; }
    m_lastEffect = DROPEFFECT_NONE;
    return S_OK;
}

STDMETHODIMP CDropTarget::Drop(
    IDataObject* pObj, DWORD /*grfKey*/, POINTL, DWORD* pdwEffect)
{
    if (!pdwEffect) return E_POINTER;

    HRESULT hr = ArchiveDrop::Perform(m_hwnd, m_pFolder,
                                      pObj ? pObj : m_pDataObj);

    // Report what really happened: NONE keeps the source from assuming the
    // payload was consumed.
    *pdwEffect = (hr == S_OK) ? DROPEFFECT_COPY : DROPEFFECT_NONE;

    if (m_pDataObj) { m_pDataObj->Release(); m_pDataObj = nullptr; }
    m_lastEffect = DROPEFFECT_NONE;
    return S_OK;
}

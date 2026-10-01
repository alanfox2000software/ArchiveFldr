// DropTarget.cpp
#include "stdafx.h"
#include "DropTarget.h"
#include "ShellFolder.h"
#include "ArchiveEngine.h"
#include "ArchiveOps.h"
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

    std::vector<ArchiveOps::AddItem> items;
    ArchiveOps::ExpandForAdd(roots, items);
    if (items.empty()) return S_FALSE;

    HCURSOR prev = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const std::wstring dir = folder->GetInternalPath();

    bool ok = true;
    for (const auto& it : items)
        ok = engine->AddFile(it.src, ArchiveOps::TargetDirFor(dir, it), nullptr) && ok;

    SetCursor(prev);

    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSH,
                   folder->GetAbsPidl(), nullptr);

    if (!ok)
        MessageBoxW(hwnd,
            L"Some files could not be added to the archive.",
            L"ArchiveFldr", MB_ICONWARNING | MB_OK);

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
    { *ppv=static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CDropTarget::AddRef()
    { return InterlockedIncrement(&m_cRef); }
STDMETHODIMP_(ULONG) CDropTarget::Release()
    { ULONG n=InterlockedDecrement(&m_cRef); if(!n) delete this; return n; }

STDMETHODIMP CDropTarget::DragEnter(
    IDataObject* pObj, DWORD /*grfKey*/, POINTL, DWORD* pdwEffect)
{
    if (!pdwEffect) return E_POINTER;

    if (m_pDataObj) { m_pDataObj->Release(); m_pDataObj = nullptr; }
    m_pDataObj = pObj;
    if (m_pDataObj) m_pDataObj->AddRef();

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

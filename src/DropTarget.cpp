// DropTarget.cpp
#include "stdafx.h"
#include "DropTarget.h"
#include "ShellFolder.h"
#include "ArchiveEngine.h"
#include "GUIDs.h"

CDropTarget::CDropTarget()  { InterlockedIncrement(&g_cDllRefCount); }
CDropTarget::~CDropTarget()
{
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
    { *ppv=this; AddRef(); return S_OK; }
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CDropTarget::AddRef()
    { return InterlockedIncrement(&m_cRef); }
STDMETHODIMP_(ULONG) CDropTarget::Release()
    { ULONG n=InterlockedDecrement(&m_cRef); if(!n) delete this; return n; }

bool CDropTarget::CanAccept(IDataObject* pObj) const
{
    FORMATETC fe{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    return SUCCEEDED(pObj->QueryGetData(&fe));
}

DWORD CDropTarget::ComputeEffect(DWORD grfKeyState) const
{
    if (!m_canDrop) return DROPEFFECT_NONE;
    return (grfKeyState & MK_CONTROL) ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
}

STDMETHODIMP CDropTarget::DragEnter(
    IDataObject* pObj, DWORD grfKey, POINTL, DWORD* pdwEffect)
{
    m_canDrop   = CanAccept(pObj);
    *pdwEffect  = ComputeEffect(grfKey);
    m_lastEffect= *pdwEffect;
    return S_OK;
}

STDMETHODIMP CDropTarget::DragOver(DWORD grfKey, POINTL, DWORD* pdwEffect)
{
    *pdwEffect  = ComputeEffect(grfKey);
    m_lastEffect= *pdwEffect;
    return S_OK;
}

STDMETHODIMP CDropTarget::DragLeave()
{
    m_canDrop = false;
    return S_OK;
}

STDMETHODIMP CDropTarget::Drop(
    IDataObject* pObj, DWORD grfKey, POINTL, DWORD* pdwEffect)
{
    *pdwEffect = DROPEFFECT_NONE;
    if (!m_canDrop || !CanAccept(pObj)) return S_OK;
    *pdwEffect = ComputeEffect(grfKey);
    return PerformDrop(pObj);
}

HRESULT CDropTarget::PerformDrop(IDataObject* pObj)
{
    FORMATETC fe{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    STGMEDIUM sm{};
    RETURN_IF_FAILED(pObj->GetData(&fe, &sm));

    HDROP hDrop = (HDROP)GlobalLock(sm.hGlobal);
    if (!hDrop) { ReleaseStgMedium(&sm); return E_FAIL; }

    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    if (m_pFolder) {
        auto engine = m_pFolder->GetEngine();
        const auto& internalPath = m_pFolder->GetInternalPath();
        if (engine) {
            for (UINT i = 0; i < count; i++) {
                wchar_t path[MAX_PATH*2] = {};
                DragQueryFileW(hDrop, i, path, MAX_PATH*2);
                engine->AddFile(path, internalPath, nullptr);
            }
        }
    }
    GlobalUnlock(sm.hGlobal);
    ReleaseStgMedium(&sm);
    return S_OK;
}
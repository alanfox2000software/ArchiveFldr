// DropTarget.cpp — reject all incoming drops while preserving the legacy CLSID
#include "stdafx.h"
#include "DropTarget.h"
#include "GUIDs.h"

CDropTarget::CDropTarget()
{
    InterlockedIncrement(&g_cDllRefCount);
}

CDropTarget::~CDropTarget()
{
    InterlockedDecrement(&g_cDllRefCount);
}

STDMETHODIMP CDropTarget::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDropTarget))
        *ppv = static_cast<IDropTarget*>(this);
    else if (IsEqualIID(riid, IID_IPersist) || IsEqualIID(riid, IID_IPersistFile))
        *ppv = static_cast<IPersistFile*>(this);
    else
        return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) CDropTarget::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_cRef);
}

STDMETHODIMP_(ULONG) CDropTarget::Release()
{
    const ULONG n = (ULONG)InterlockedDecrement(&m_cRef);
    if (!n) delete this;
    return n;
}

STDMETHODIMP CDropTarget::DragEnter(
    IDataObject*, DWORD, POINTL, DWORD* effect)
{
    if (!effect) return E_POINTER;
    *effect = DROPEFFECT_NONE;
    return S_OK;
}

STDMETHODIMP CDropTarget::DragOver(DWORD, POINTL, DWORD* effect)
{
    if (!effect) return E_POINTER;
    *effect = DROPEFFECT_NONE;
    return S_OK;
}

STDMETHODIMP CDropTarget::DragLeave()
{
    return S_OK;
}

STDMETHODIMP CDropTarget::Drop(
    IDataObject*, DWORD, POINTL, DWORD* effect)
{
    if (!effect) return E_POINTER;
    *effect = DROPEFFECT_NONE;
    return S_OK;
}

STDMETHODIMP CDropTarget::GetClassID(CLSID* clsid)
{
    if (!clsid) return E_POINTER;
    *clsid = CLSID_ArchiveFldrDropTarget;
    return S_OK;
}

STDMETHODIMP CDropTarget::IsDirty()
{
    return S_FALSE;
}

STDMETHODIMP CDropTarget::Load(LPCOLESTR fileName, DWORD)
{
    if (!fileName || !*fileName) return E_INVALIDARG;
    m_archive = fileName;
    return S_OK;
}

STDMETHODIMP CDropTarget::Save(LPCOLESTR, BOOL)
{
    return E_NOTIMPL;
}

STDMETHODIMP CDropTarget::SaveCompleted(LPCOLESTR)
{
    return S_OK;
}

STDMETHODIMP CDropTarget::GetCurFile(LPOLESTR* fileName)
{
    if (!fileName) return E_POINTER;
    *fileName = nullptr;
    if (m_archive.empty()) return S_FALSE;

    const size_t bytes = (m_archive.size() + 1) * sizeof(wchar_t);
    auto* copy = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
    if (!copy) return E_OUTOFMEMORY;
    memcpy(copy, m_archive.c_str(), bytes);
    *fileName = copy;
    return S_OK;
}

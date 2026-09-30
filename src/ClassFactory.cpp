// ClassFactory.cpp
#include "stdafx.h"
#include "ClassFactory.h"
#include "ShellFolder.h"
#include "ContextMenu.h"
#include "IconOverlay.h"
#include "DropTarget.h"
#include "ThumbnailProvider.h"
#include "PreviewHandler.h"
#include "PropertySheet.h"

// ── IUnknown ─────────────────────────────────────────────
STDMETHODIMP CClassFactory::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_IClassFactory))
    {
        *ppv = static_cast<IClassFactory*>(this);
        AddRef(); return S_OK;
    }
    *ppv = nullptr; return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CClassFactory::AddRef()  { return InterlockedIncrement(&m_cRef); }
STDMETHODIMP_(ULONG) CClassFactory::Release()
{
    ULONG n = InterlockedDecrement(&m_cRef);
    if (n == 0) delete this;
    return n;
}

// ── IClassFactory ────────────────────────────────────────
STDMETHODIMP CClassFactory::CreateInstance(IUnknown* pUnkOuter,
                                            REFIID    riid,
                                            void**    ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (pUnkOuter) return CLASS_E_NOAGGREGATION;
    return m_fnCreate(riid, ppv);
}
STDMETHODIMP CClassFactory::LockServer(BOOL fLock)
{
    fLock ? InterlockedIncrement(&g_cLockCount)
          : InterlockedDecrement(&g_cLockCount);
    return S_OK;
}

// ── Static Creators ───────────────────────────────────────
HRESULT CClassFactory::CreateShellFolder(REFIID riid, LPVOID* ppv)
{
    auto* p = new(std::nothrow) CShellFolder();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release(); return hr;
}
HRESULT CClassFactory::CreateContextMenu(REFIID riid, LPVOID* ppv)
{
    auto* p = new(std::nothrow) CContextMenu();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release(); return hr;
}
HRESULT CClassFactory::CreateIconOverlay(REFIID riid, LPVOID* ppv)
{
    auto* p = new(std::nothrow) CIconOverlay();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release(); return hr;
}
HRESULT CClassFactory::CreateDropTarget(REFIID riid, LPVOID* ppv)
{
    auto* p = new(std::nothrow) CDropTarget();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release(); return hr;
}
HRESULT CClassFactory::CreateThumbnailProvider(REFIID riid, LPVOID* ppv)
{
    auto* p = new(std::nothrow) CThumbnailProvider();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release(); return hr;
}
HRESULT CClassFactory::CreatePreviewHandler(REFIID riid, LPVOID* ppv)
{
    auto* p = new(std::nothrow) CPreviewHandler();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release(); return hr;
}
HRESULT CClassFactory::CreatePropertySheet(REFIID riid, LPVOID* ppv)
{
    auto* p = new(std::nothrow) CPropertySheet();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release(); return hr;
}
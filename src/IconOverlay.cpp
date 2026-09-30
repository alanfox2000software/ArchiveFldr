// IconOverlay.cpp
#include "stdafx.h"
#include "IconOverlay.h"
#include "GUIDs.h"

CIconOverlay::CIconOverlay()  { InterlockedIncrement(&g_cDllRefCount); }
CIconOverlay::~CIconOverlay() { InterlockedDecrement(&g_cDllRefCount); }

STDMETHODIMP CIconOverlay::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER; *ppv = nullptr;
    if (IsEqualIID(riid,IID_IUnknown)||
        IsEqualIID(riid,IID_IShellIconOverlayIdentifier))
    { *ppv=this; AddRef(); return S_OK; }
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CIconOverlay::AddRef()
    { return InterlockedIncrement(&m_cRef); }
STDMETHODIMP_(ULONG) CIconOverlay::Release()
    { ULONG n=InterlockedDecrement(&m_cRef); if(!n) delete this; return n; }

STDMETHODIMP CIconOverlay::IsMemberOf(LPCWSTR pwszPath, DWORD /*dwAttrib*/)
{
    if (!pwszPath) return S_FALSE;
    // Only overlay files that are supported archives
    LPCWSTR ext = PathFindExtensionW(pwszPath);
    return IsArchiveExtension(ext) ? S_OK : S_FALSE;
}

STDMETHODIMP CIconOverlay::GetOverlayInfo(
    LPWSTR pwszIconFile, int cchMax,
    int*   pIndex, DWORD* pdwFlags)
{
    if (!pwszIconFile||!pIndex||!pdwFlags) return E_POINTER;
    // Return our DLL as the icon source (icon index IDI_OVERLAY_ARCHIVE = 203)
    GetModuleFileNameW(g_hDllInstance, pwszIconFile, cchMax);
    *pIndex   = 2;   // third icon in DLL (0-based)
    *pdwFlags = ISIOI_ICONFILE | ISIOI_ICONINDEX;
    return S_OK;
}

STDMETHODIMP CIconOverlay::GetPriority(int* pPriority)
{
    // 0 = highest, 100 = lowest
    // Use 50 to not override other important overlays (git, OneDrive, etc.)
    if (!pPriority) return E_POINTER;
    *pPriority = 50; return S_OK;
}
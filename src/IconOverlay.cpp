// IconOverlay.cpp
#include "stdafx.h"
#include "IconOverlay.h"
#include "GUIDs.h"
#include "Settings.h"
#include "resource.h"

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
    if (!Settings::Get().showIconOverlay) return S_FALSE;
    // Only overlay files that are supported archives
    LPCWSTR ext = PathFindExtensionW(pwszPath);
    return IsArchiveExtension(ext) ? S_OK : S_FALSE;
}

// Find a value for pIndex that really does yield an icon out of this module.
//
// Get this wrong and the shell still reserves the overlay slot and paints
// the empty placeholder it uses for a missing icon -- a blank page stuck in
// the corner of every archive, which is what shipped while the resource
// script had no ICON statements at all.
static int ResolveOverlayIconIndex(const wchar_t* module)
{
    static int  s_cached = INT_MIN;      // INT_MIN = "no usable icon"
    static bool s_done   = false;
    if (s_done) return s_cached;

    // Positional index first (see the index table in resource.rc), then the
    // negative-resource-id form that shell icon locations also accept.
    const int candidates[] = { 3, -IDI_OVERLAY_ARCHIVE };
    for (int candidate : candidates)
    {
        HICON large = nullptr, small = nullptr;
        UINT  got   = ExtractIconExW(module, candidate, &large, &small, 1);
        const bool ok = (got != (UINT)-1) && (large || small);
        if (large) DestroyIcon(large);
        if (small) DestroyIcon(small);
        if (ok) { s_cached = candidate; break; }
    }
    s_done = true;
    return s_cached;
}

STDMETHODIMP CIconOverlay::GetOverlayInfo(
    LPWSTR pwszIconFile, int cchMax,
    int*   pIndex, DWORD* pdwFlags)
{
    if (!pwszIconFile||!pIndex||!pdwFlags) return E_POINTER;
    if (!Settings::Get().showIconOverlay) return E_FAIL;

    wchar_t module[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_hDllInstance, module, ARRAYSIZE(module)))
        return E_FAIL;

    const int index = ResolveOverlayIconIndex(module);
    if (index == INT_MIN) return E_FAIL;    // no icon: draw nothing at all
    if (cchMax <= (int)wcslen(module)) return E_FAIL;

    wcscpy_s(pwszIconFile, (size_t)cchMax, module);
    *pIndex   = index;
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
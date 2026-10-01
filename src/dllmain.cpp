// dllmain.cpp — DLL Entry Point & COM Export Functions
#include "stdafx.h"
#include <initguid.h>
#include "GUIDs.h"
#include "ClassFactory.h"
#include "Registry.h"
#include "Settings.h"

// ── Module-level globals ─────────────────────────────────
HINSTANCE g_hDllInstance = nullptr;
long      g_cDllRefCount = 0;
long      g_cLockCount   = 0;

// ─────────────────────────────────────────────────────────
// DllMain
// ─────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID /*lpReserved*/)
{
    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:
        g_hDllInstance = hModule;
        DisableThreadLibraryCalls(hModule);
        // Init GDI+
        {
            Gdiplus::GdiplusStartupInput gsi;
            ULONG_PTR token;
            Gdiplus::GdiplusStartup(&token, &gsi, nullptr);
        }
        // Init COM controls
        {
            INITCOMMONCONTROLSEX icc{sizeof(icc),
                ICC_WIN95_CLASSES | ICC_BAR_CLASSES |
                ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES |
                ICC_UPDOWN_CLASS};
            InitCommonControlsEx(&icc);
        }
        // Load settings
        Settings::Get().Load();
        break;

    case DLL_PROCESS_DETACH:
        Settings::Get().Save();
        Gdiplus::GdiplusShutdown(0);
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}

// ─────────────────────────────────────────────────────────
// DllGetClassObject — Called by COM to get our class factory
// ─────────────────────────────────────────────────────────
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    // Map CLSID → creator function
    struct Entry {
        const CLSID* clsid;
        HRESULT(*create)(REFIID, LPVOID*);
    };

    static const Entry kEntries[] = {
        { &CLSID_ShellNSEFolder,      CClassFactory::CreateShellFolder      },
        { &CLSID_ShellNSEContextMenu, CClassFactory::CreateContextMenu      },
        { &CLSID_ShellNSEIconOverlay, CClassFactory::CreateIconOverlay      },
        { &CLSID_ShellNSEDropTarget,  CClassFactory::CreateDropTarget       },
        { &CLSID_ShellNSEThumbnail,   CClassFactory::CreateThumbnailProvider},
        { &CLSID_ShellNSEPreview,     CClassFactory::CreatePreviewHandler   },
        { &CLSID_ShellNSEPropSheet,   CClassFactory::CreatePropertySheet    },
    };

    for (auto& e : kEntries) {
        if (IsEqualCLSID(rclsid, *e.clsid)) {
            auto* pCF = new(std::nothrow) CClassFactory(e.create);
            if (!pCF) return E_OUTOFMEMORY;
            HRESULT hr = pCF->QueryInterface(riid, ppv);
            pCF->Release();
            return hr;
        }
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

// ─────────────────────────────────────────────────────────
// DllCanUnloadNow — Safe to unload if no outstanding objects
// ─────────────────────────────────────────────────────────
STDAPI DllCanUnloadNow()
{
    return (g_cDllRefCount == 0 && g_cLockCount == 0) ? S_OK : S_FALSE;
}

// ─────────────────────────────────────────────────────────
// DllRegisterServer — Register all COM objects & extensions
// ─────────────────────────────────────────────────────────
STDAPI DllRegisterServer()
{
    wchar_t dllPath[MAX_PATH] = {};
    GetModuleFileNameW(g_hDllInstance, dllPath, MAX_PATH);

    HRESULT hr = CRegistry::RegisterAll(dllPath);
    if (SUCCEEDED(hr)) {
        // Notify Explorer to refresh shell
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
    return hr;
}

// ─────────────────────────────────────────────────────────
// DllUnregisterServer — Remove all registrations
// ─────────────────────────────────────────────────────────
STDAPI DllUnregisterServer()
{
    HRESULT hr = CRegistry::UnregisterAll();
    if (SUCCEEDED(hr))
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return hr;
}

// ─────────────────────────────────────────────────────────
// DllInstall — Called by regsvr32 /i (install) /u /i (uninstall)
// ─────────────────────────────────────────────────────────
STDAPI DllInstall(BOOL bInstall, LPCWSTR /*pszCmdLine*/)
{
    return bInstall ? DllRegisterServer() : DllUnregisterServer();
}
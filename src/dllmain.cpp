// dllmain.cpp — DLL Entry Point & COM Export Functions
#include "stdafx.h"
#include <initguid.h>
#include "GUIDs.h"
#include "Sdk7z.h"           // emits storage for the 7z-engine GUIDs (see Sdk7z.h)
#include "ClassFactory.h"
#include "Registry.h"
#include "Settings.h"

// ── Module-level globals ─────────────────────────────────
HINSTANCE g_hDllInstance = nullptr;
long      g_cDllRefCount = 0;
long      g_cLockCount   = 0;

// ─────────────────────────────────────────────────────────
// Lazy subsystem start-up
//
// None of this may happen in DllMain. DllMain runs under the loader lock,
// and GdiplusStartup creates a background thread and waits for it to come
// up — that thread cannot finish loading while we hold the lock, so the
// process deadlocks inside LoadLibrary. The symptom is spectacular and
// hard to attribute: every host that loads this DLL (the thumbnail
// surrogate "DllHost.exe /Processid:{AB8902B4-...}", regsvr32, Explorer
// itself) can hang forever, so the hung processes pile up, each one
// holding the DLL and its 7z backend in memory.
//
// Magic statics give us thread-safe one-time init at the point of use,
// and these are called from drawing/UI code, never from the loader.
// ─────────────────────────────────────────────────────────
bool EnsureGdiPlus()
{
    struct Starter {
        ULONG_PTR token = 0;
        bool      ok    = false;
        Starter() {
            Gdiplus::GdiplusStartupInput gsi;
            ok = (Gdiplus::GdiplusStartup(&token, &gsi, nullptr) == Gdiplus::Ok);
        }
        // Deliberately no destructor: GdiplusShutdown during process exit
        // would run under the loader lock as well, and the OS reclaims
        // everything anyway.
    };
    static Starter s_starter;
    return s_starter.ok;
}

void EnsureCommonControls()
{
    struct Starter {
        Starter() {
            INITCOMMONCONTROLSEX icc{ sizeof(icc),
                ICC_WIN95_CLASSES | ICC_BAR_CLASSES |
                ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES |
                ICC_UPDOWN_CLASS };
            InitCommonControlsEx(&icc);
        }
    };
    static Starter s_starter;
}

// ─────────────────────────────────────────────────────────
// DllMain
//
// Does the bare minimum the loader allows: record the module handle and
// switch off thread notifications. Everything else — GDI+, common
// controls, reading settings out of the registry — is deferred to the
// code that needs it. See the note above.
// ─────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID /*lpReserved*/)
{
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_hDllInstance = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    // DLL_PROCESS_DETACH used to save settings here. That wrote the
    // registry from inside the loader lock during process teardown, in
    // every process that ever loaded this DLL — and once settings became
    // lazily loaded it would have written defaults over the real values.
    // Settings are saved when the user changes them instead.
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
        { &CLSID_ArchiveFldrFolder,      CClassFactory::CreateShellFolder      },
        { &CLSID_ArchiveFldrContextMenu, CClassFactory::CreateContextMenu      },
        { &CLSID_ArchiveFldrDropTarget,  CClassFactory::CreateDropTarget       },
#ifndef ARCHIVEFLDR_NO_VISTA_HANDLERS
        { &CLSID_ArchiveFldrThumbnail,   CClassFactory::CreateThumbnailProvider},
        { &CLSID_ArchiveFldrPreview,     CClassFactory::CreatePreviewHandler   },
#endif // ARCHIVEFLDR_NO_VISTA_HANDLERS
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
//
// The command line is how an unattended install asks for the same
// thing the buttons in the settings program do:
//
//   regsvr32 /i ArchiveFldr.64.dll                everything
//   regsvr32 /i:base ArchiveFldr.64.dll           no context menu
//   regsvr32 /i:contextmenu ArchiveFldr.64.dll    with context menu
//
// It is recorded as a setting and then acted on by RegisterAll, rather
// than passed down as an argument, because that is where the answer has
// to live anyway: the other bitness, and the next re-registration,
// both read it from there. Per bitness, naturally — this DLL can only
// speak for its own build.
//
// Anything else on the command line is a full install, which is what
// every earlier build did with any command line at all.
// ─────────────────────────────────────────────────────────
STDAPI DllInstall(BOOL bInstall, LPCWSTR pszCmdLine)
{
    if (!bInstall)
        return DllUnregisterServer();

    if (pszCmdLine && *pszCmdLine)
    {
        Settings& s = Settings::Get();
        const bool off = _wcsicmp(pszCmdLine, L"base") == 0 ||
                         _wcsicmp(pszCmdLine, L"nocontextmenu") == 0;
        const bool on  = _wcsicmp(pszCmdLine, L"contextmenu") == 0;
        if (off || on)
        {
            s.CtxMenuHere() = on;
            s.Save();
        }
    }
    return DllRegisterServer();
}
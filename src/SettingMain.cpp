// SettingMain.cpp — entry point for ArchiveFldrSetting.exe
//
// Two jobs, decided by the command line:
//
//   (no arguments)        show the settings window
//   /open "<path>"        browse that archive in File Explorer
//
// The second one exists because Windows' "choose an app" list is a list of
// *applications*. ArchiveFldr's code is a shell extension DLL, and a DLL
// can never be a row in that list, so the file types register their open
// verb against this executable. Handed a path, it does the one thing the
// verb promises and gets out of the way.
//
// The settings UI used to live inside the shell extension, which meant
// Explorer loaded the whole dialog — six pages, a tree view, the common
// controls — into every process that so much as right-clicked a file, and
// kept it resident for as long as the shell cached the DLL. A settings
// window has no business inside a shell extension, so it is its own
// program now. ArchiveFldr launches it and gets out of the way.
//
// Registration still writes to HKLM, so the Register / Unregister buttons
// need an elevated process. This runs asInvoker: starting it normally is
// enough to read and change preferences, and the registration buttons
// report the failure rather than the window refusing to open at all.

#include "stdafx.h"
#include <initguid.h>   // emits storage for the CLSIDs below, as dllmain.cpp
#include "GUIDs.h"      // does for the shell extension
#include "SettingsDialog.h"
#include "Settings.h"

// The shell extension gets these from DllMain; a plain program has to
// supply its own. Common controls must be up before the tree view and
// the up-down controls on the settings pages are created.
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

namespace {

// Written out rather than as a lambda: WNDENUMPROC is __stdcall, and a
// captureless lambda only converts to a plain function pointer, which is
// __cdecl on x86. The two builds disagree about who cleans up the stack.
BOOL CALLBACK FindOurWindow(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    wchar_t title[128] = {};
    if (!GetWindowTextW(h, title, ARRAYSIZE(title))) return TRUE;
    if (wcsstr(title, L"ArchiveFldr") == nullptr)     return TRUE;
    if (!IsWindowVisible(h))                          return TRUE;

    *reinterpret_cast<HWND*>(lp) = h;
    return FALSE;
}

// ── /open ─────────────────────────────────────────────────
// A file browses as a folder only when the file type's ProgID carries the
// CLSID junction of a namespace extension. That junction belongs to
// whoever owns the type, so if another archiver owns .7z there is no way
// to force our view onto it — and pretending otherwise would open the
// other program's window while claiming to be ours.
bool JunctionIsOurs(LPCITEMIDLIST pidlAbs)
{
    if (!pidlAbs) return false;

    IShellFolder* desktop = nullptr;
    if (FAILED(SHGetDesktopFolder(&desktop)) || !desktop) return false;

    bool ours = false;
    IShellFolder* target = nullptr;
    if (SUCCEEDED(desktop->BindToObject(pidlAbs, nullptr, IID_IShellFolder,
                                        (void**)&target)) && target)
    {
        IPersist* persist = nullptr;
        if (SUCCEEDED(target->QueryInterface(IID_IPersist, (void**)&persist))
            && persist)
        {
            CLSID clsid{};
            if (SUCCEEDED(persist->GetClassID(&clsid)))
                ours = IsEqualCLSID(clsid, CLSID_ArchiveFldrFolder) != FALSE;
            persist->Release();
        }
        target->Release();
    }
    desktop->Release();
    return ours;
}

void OfferDefaultApps(const std::wstring& ext)
{
    std::wstring msg = L"Another program currently owns the ";
    msg += ext.empty() ? std::wstring(L"archive") : ext;
    msg += L" file type, so Windows hands the file to that program instead "
           L"of ArchiveFldr.\n\n"
           L"To change it, open Settings > Apps > Default apps, search for "
           L"ArchiveFldr, and point the file type at it.\n\n"
           L"Open Default apps now?";

    if (MessageBoxW(nullptr, msg.c_str(), L"ArchiveFldr",
                    MB_ICONINFORMATION | MB_YESNO) != IDYES)
        return;

    HINSTANCE rc = ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps",
                                 nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)rc <= 32)
        ShellExecuteW(nullptr, L"open", L"control.exe",
            L"/name Microsoft.DefaultPrograms /page pageFileAssoc",
            nullptr, SW_SHOWNORMAL);
}

int OpenArchiveWindow(const std::wstring& raw)
{
    // Relative paths happen when the verb is invoked from a command line.
    std::wstring path = raw;
    {
        std::wstring full(32768, L'\0');
        DWORD n = GetFullPathNameW(raw.c_str(), (DWORD)full.size(),
                                   full.data(), nullptr);
        if (n && n < full.size()) { full.resize(n); path = full; }
    }

    if (!PathFileExistsW(path.c_str()))
    {
        MessageBoxW(nullptr,
            (L"ArchiveFldr cannot find this file:\n\n" + path).c_str(),
            L"ArchiveFldr", MB_ICONERROR | MB_OK);
        return 1;
    }

    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (!pidl)
    {
        MessageBoxW(nullptr,
            (L"Windows could not resolve this path:\n\n" + path).c_str(),
            L"ArchiveFldr", MB_ICONERROR | MB_OK);
        return 1;
    }

    // A folder is just a folder — open it and say nothing about junctions.
    if (!PathIsDirectoryW(path.c_str()) && !JunctionIsOurs(pidl))
    {
        ILFree(pidl);
        LPCWSTR dot = PathFindExtensionW(path.c_str());
        OfferDefaultApps((dot && *dot) ? dot : L"");
        return 1;
    }

    const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
    if (SUCCEEDED(hr)) return 0;

    MessageBoxW(nullptr,
        L"ArchiveFldr could not open a view of this archive.\n\n"
        L"Make sure the extension is registered (run, as administrator):\n"
        L"    regsvr32 ArchiveFldr.64.dll",
        L"ArchiveFldr", MB_ICONERROR | MB_OK);
    return 1;
}

// The first argument that is not a switch is the file to open. Being
// liberal here costs nothing and makes dropping an archive on the
// executable work the same way the registered verb does.
std::wstring TargetFromCommandLine()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return L"";

    std::wstring target;
    for (int i = 1; i < argc && target.empty(); ++i)
    {
        const wchar_t* a = argv[i];
        if (!a || !*a) continue;
        if (a[0] == L'/' || a[0] == L'-') continue;   // /open and anything else
        target = a;
    }
    LocalFree(argv);
    return target;
}

// Already running? Show that window instead of stacking up duplicates.
bool FocusExistingInstance()
{
    // The dialog has no fixed class name, so match on a named mutex plus a
    // broadcast-free window search over top-level windows of this session.
    HWND found = nullptr;
    EnumWindows(FindOurWindow, reinterpret_cast<LPARAM>(&found));

    if (!found) return false;
    if (IsIconic(found)) ShowWindow(found, SW_RESTORE);
    SetForegroundWindow(found);
    return true;
}

} // namespace

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE,
                      _In_ LPWSTR, _In_ int)
{
    // Opening an archive is a one-shot job with no window of our own, so
    // it runs before the single-instance check: two archives opened at
    // once are two Explorer windows, not two settings dialogs.
    const std::wstring target = TargetFromCommandLine();
    if (!target.empty())
    {
        HRESULT hrOpen = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                 COINIT_DISABLE_OLE1DDE);
        const int rc = OpenArchiveWindow(target);
        if (SUCCEEDED(hrOpen)) CoUninitialize();
        UNREFERENCED_PARAMETER(hInstance);
        return rc;
    }

    // One instance at a time. The mutex is the authority; the window search
    // above is only how the existing one gets raised.
    HANDLE once = CreateMutexW(nullptr, FALSE, L"Local\\ArchiveFldrSettings");
    const bool alreadyRunning =
        once && GetLastError() == ERROR_ALREADY_EXISTS;
    if (alreadyRunning && FocusExistingInstance())
    {
        CloseHandle(once);
        return 0;
    }

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                            COINIT_DISABLE_OLE1DDE);
    EnsureCommonControls();

    // Settings::Get() loads on first touch; do it here so a failure to read
    // the registry surfaces before the window appears rather than midway.
    Settings::Get();

    CSettingsDialog dlg;
    dlg.Show(nullptr);

    if (SUCCEEDED(hrCom)) CoUninitialize();
    if (once) CloseHandle(once);

    UNREFERENCED_PARAMETER(hInstance);
    return 0;
}

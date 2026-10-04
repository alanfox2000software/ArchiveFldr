// SettingMain.cpp — entry point for ArchiveFldrSetting.exe
//
// Preferences, and nothing else. Archives are opened by the shell
// extension in ArchiveFldr.dll, through the namespace extension Explorer
// browses into — the same way the built-in zip folder works. A separate
// process has no part in that and was only ever in the way.
//
// The settings UI used to live inside the shell extension, which meant
// Explorer loaded the whole dialog into every process that so much as
// right-clicked a file, and kept it resident for as long as the shell
// cached the DLL. A settings window has no business inside a shell
// extension, so it is its own program now. ArchiveFldr launches it and
// gets out of the way.
//
// It runs elevated. Everything it writes is machine-wide — preferences
// under HKLM\\SOFTWARE\\ArchiveFldr, and a shell extension registered
// for every account — so the manifest asks for administrator and the
// prompt comes once, at launch, instead of a save failing later.

#include "stdafx.h"
#include <initguid.h>   // emits storage for the CLSIDs below, as dllmain.cpp
#include "GUIDs.h"      // does for the shell extension
#include "SettingsDialog.h"
#include "Settings.h"
#include "Lang.h"

// The shell extension gets these from DllMain; a plain program has to
// supply its own. Common controls must be up before the tab strip and
// the list views on the settings pages are created.
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

// Already running? Show that window instead of stacking up duplicates.
//
// timeoutMs covers the race where the other instance has created the mutex
// but has not created its window yet. Failure to find the window does not
// authorize another settings process; the named mutex remains authoritative.
bool FocusExistingInstance(DWORD timeoutMs = 0)
{
    // The dialog has no fixed class name, so match on a named mutex plus a
    // broadcast-free window search over top-level windows of this session.
    const DWORD deadline = GetTickCount() + timeoutMs;
    for (;;)
    {
        HWND found = nullptr;
        EnumWindows(FindOurWindow, reinterpret_cast<LPARAM>(&found));

        if (found)
        {
            if (IsIconic(found)) ShowWindow(found, SW_RESTORE);
            SetForegroundWindow(found);
            return true;
        }

        // Signed comparison: survives the tick count wrapping round.
        if ((LONG)(GetTickCount() - deadline) >= 0) return false;
        Sleep(100);
    }
}

} // namespace

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE,
                      _In_ LPWSTR, _In_ int)
{
    // One instance at a time. The mutex is the authority; the window search
    // above is only how the existing one gets raised.
    HANDLE once = CreateMutexW(nullptr, FALSE, L"Local\\ArchiveFldrSettings");
    if (!once)
        return static_cast<int>(GetLastError()); // cannot enforce exclusivity

    const bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;
    if (alreadyRunning)
    {
        // Best effort: the first instance may still be creating its dialog,
        // or it may be unresponsive. Either way, never start a second process.
        FocusExistingInstance(3000);
        CloseHandle(once);
        return 0;
    }

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                            COINIT_DISABLE_OLE1DDE);
    EnsureCommonControls();

    // Settings::Get() loads on first touch; do it here so a failure to read
    // the registry surfaces before the window appears rather than midway.
    Settings::Get();

    // The chosen language, before a single window exists — the dialog
    // templates are in English and Lang::Apply overwrites them as each
    // page is created, so loading later would show a flash of English.
    Lang::Load(Settings::Get().language);

    CSettingsDialog dlg;
    dlg.Show(nullptr);

    if (SUCCEEDED(hrCom)) CoUninitialize();
    if (once) CloseHandle(once);

    UNREFERENCED_PARAMETER(hInstance);
    return 0;
}

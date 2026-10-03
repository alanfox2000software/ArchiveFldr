// SettingsDialog.cpp — see SettingsDialog.h
#include "stdafx.h"
#include "SettingsDialog.h"
#include "Settings.h"
#include "Formats.h"
#include "Registry.h"
#include "GUIDs.h"
#include "Lang.h"
#include <shlobj.h>

// ─────────────────────────────────────────────────────────
// Language keys that are not control ids
//
// Rows in a list control have no window of their own, so Lang::Apply
// cannot reach them. They get ids of their own in the 2000 block and are
// looked up by hand. Keep these in step with Lang\en.txt.
// ─────────────────────────────────────────────────────────
enum : UINT {
    LNG_CTX_OPEN      = 2000,
    LNG_CTX_EXTRACT   = 2001,
    LNG_CTX_EXTHERE   = 2002,
    LNG_CTX_TEST      = 2003,
    LNG_CTX_ADD       = 2004,
    LNG_CTX_ADDHERE   = 2005,
    LNG_CTX_EMAIL     = 2006,
    LNG_CTX_INFO      = 2007,
    LNG_CTX_SETTINGS  = 2008,

    // 2020–2022 belonged to the removed System page.

    LNG_COL_LANGUAGE  = 2023,
    LNG_COL_LANG_EN   = 2024,

    LNG_STATE_YES     = 2040,
    LNG_STATE_NO      = 2041,
    LNG_STATE_PARTIAL = 2042,
    LNG_INSTALL_NOTE  = 2043,
    LNG_INSTALL_OK    = 2044,
    LNG_INSTALL_FAIL  = 2045,
    LNG_UNINSTALL_OK  = 2046,
    LNG_BROWSE_WORK   = 2047,
    LNG_RESTART_SHELL = 2048,
    LNG_STATE_CTXMENU = 2049,
    LNG_STATE_CTXONLY = 2050,
};

namespace {

// ── small helpers ────────────────────────────────────────

HINSTANCE SelfInstance()
{
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&SelfInstance, &h);
    return (HINSTANCE)h;
}

// ── page hosting ─────────────────────────────────────────
//
// A page is a child dialog parked over the tab control's display
// rectangle. It is a *sibling* of that tab control, not a child of it,
// so the two compete for the same pixels and whichever sits higher in
// the sibling z-order wins.
//
// That is what left every page blank. Place() asked for HWND_TOP and
// passed SWP_NOZORDER in the same call — and SWP_NOZORDER tells
// SetWindowPos to ignore hWndInsertAfter altogether, so the request was
// a no-op. The pages were created, sized and shown correctly; the tab
// control simply painted its empty body straight over the top of them,
// which looks exactly like a page that was never created at all.
//
// Everything below is shared by all four pages rather than copied into
// each, so a fix cannot land in four of them and miss the fifth.

HWND CreatePage(UINT dlgId, HWND parent, DLGPROC proc, void* self)
{
    HWND page = CreateDialogParamW(SelfInstance(), MAKEINTRESOURCEW(dlgId),
                                   parent, proc, (LPARAM)self);
    if (!page)
    {
        // A page that fails to create is indistinguishable from one that
        // is merely hidden, so say so instead of leaving a blank panel
        // and no explanation.
        const DWORD err = GetLastError();
        wchar_t msg[192];
        swprintf_s(msg, L"Could not create settings page %u.\r\n"
                        L"Windows reported error %lu.", dlgId, err);
        MessageBoxW(parent, msg, L"ArchiveFldr", MB_OK | MB_ICONERROR);
        return nullptr;
    }

    // A themed tab body is not COLOR_3DFACE. Without this the page shows
    // as a flat grey rectangle on top of the lighter tab background.
    EnableThemeDialogTexture(page, ETDT_ENABLETAB);
    return page;
}

void PlacePage(HWND page, const RECT& rc)
{
    if (!page) return;
    SetWindowPos(page, HWND_TOP, rc.left, rc.top,
                 rc.right - rc.left, rc.bottom - rc.top, SWP_NOACTIVATE);
}

void ShowPageWindow(HWND page, bool show)
{
    if (!page) return;
    if (!show) { ShowWindow(page, SW_HIDE); return; }

    // Raise and show in one call. The lift above the tab control has to
    // be repeated on every switch: hiding and re-showing a window does
    // not restore its z-order, and the tab control is repainted
    // underneath each time the selection changes.
    SetWindowPos(page, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

std::wstring ExeDir()
{
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, ARRAYSIZE(path))) return L"";
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return L"";
    slash[1] = 0;
    return path;
}

// The pages live on the Options dialog, which keeps the C++ object in
// DWLP_USER. Any control change routes through here to light up Apply.
void MarkDirty(HWND page)
{
    HWND owner = GetParent(page);
    if (!owner) return;
    auto* dlg = (CSettingsDialog*)GetWindowLongPtrW(owner, DWLP_USER);
    if (dlg) dlg->EnableApply(true);
}

void AddColumn(HWND list, int index, const wchar_t* text, int width, int fmt = LVCFMT_LEFT)
{
    LVCOLUMNW c{};
    c.mask    = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
    c.fmt     = fmt;
    c.cx      = width;
    c.iSubItem= index;
    c.pszText = (LPWSTR)text;
    ListView_InsertColumn(list, index, &c);
}

int AddRow(HWND list, int index, const wchar_t* text)
{
    LVITEMW it{};
    it.mask     = LVIF_TEXT;
    it.iItem    = index;
    it.pszText  = (LPWSTR)text;
    return ListView_InsertItem(list, &it);
}

void SetSub(HWND list, int row, int col, const wchar_t* text)
{
    ListView_SetItemText(list, row, col, (LPWSTR)text);
}

// Both shell extension DLLs, by the names the build produces.
//
// They no longer have to be in the same folder. Each platform builds
// into <Config>\x64 or <Config>\x32, so the settings program
// has to step sideways to find its opposite number — and it must, or
// Install silently stops registering the other bitness and 32-bit
// hosts lose the extension.
//
// Beside the EXE still comes first: that is a single-folder install,
// which is what gets shipped.
std::wstring DllPath(bool x64)
{
    const wchar_t* name = x64 ? L"ArchiveFldr.64.dll" : L"ArchiveFldr.32.dll";

    const std::wstring here = ExeDir();
    if (here.empty()) return name;
    if (PathFileExistsW((here + name).c_str())) return here + name;

    std::wstring parent = here;
    parent.pop_back();
    const size_t slash = parent.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return here + name;
    parent.erase(slash + 1);

    const std::wstring sibling =
        parent + (x64 ? L"x64\\" : L"x32\\") + name;
    if (PathFileExistsW(sibling.c_str())) return sibling;

    // Nothing found. Return the beside-the-EXE spelling so the caller's
    // "not present" message names the place people will look first.
    return here + name;
}

bool DllPresent(bool x64)
{
    return PathFileExistsW(DllPath(x64).c_str());
}

// Is one of our COM servers registered for this bitness, and does its
// InProcServer32 still point at a file that exists?
bool ServerRegistered(const CLSID& server, bool x64)
{
    wchar_t clsid[64] = {};
    if (!StringFromGUID2(server, clsid, ARRAYSIZE(clsid))) return false;

    const std::wstring key =
        std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + clsid +
        L"\\InProcServer32";

    HKEY hk = nullptr;
    const REGSAM view = x64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0,
                      KEY_QUERY_VALUE | view, &hk) != ERROR_SUCCESS)
        return false;

    // Read one element short of a zeroed buffer, so a REG_SZ stored
    // without its terminator cannot run PathFileExistsW off the end.
    wchar_t buf[MAX_PATH * 2 + 1] = {};
    DWORD sz = sizeof(buf) - sizeof(wchar_t);
    const bool got = RegQueryValueExW(hk, nullptr, nullptr, nullptr,
                                      (BYTE*)buf, &sz) == ERROR_SUCCESS;
    RegCloseKey(hk);
    return got && buf[0] && PathFileExistsW(buf);
}

bool DllRegistered(bool x64)
{
    return ServerRegistered(CLSID_ArchiveFldrFolder, x64);
}

bool ContextMenuRegistered(bool x64)
{
    return ServerRegistered(CLSID_ArchiveFldrContextMenu, x64);
}

// regsvr32 for a given bitness. From a 64-bit process System32 is the
// 64-bit tree and SysWOW64 the 32-bit one; from a 32-bit process on a
// 32-bit Windows there is only System32. Either way this picks a
// regsvr32 whose bitness matches the DLL, which is the whole point —
// the wrong one fails with "the module is not compatible".
bool RunRegsvr32(bool x64, bool unregister, DWORD* exitCode,
                 const wchar_t* part = nullptr)
{
    wchar_t win[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(win, ARRAYSIZE(win))) return false;

    std::wstring tool = win;
    if (!tool.empty() && tool.back() != L'\\') tool += L'\\';
#ifdef _WIN64
    tool += x64 ? L"System32\\regsvr32.exe" : L"SysWOW64\\regsvr32.exe";
#else
    if (x64) return false;                 // a 32-bit build never does this
    tool += L"System32\\regsvr32.exe";
#endif
    if (!PathFileExistsW(tool.c_str())) return false;

    std::wstring args = L"\"" + tool + L"\" /s ";
    if (unregister) args += L"/u ";
    if (part && *part) args += std::wstring(L"/n /i:") + part + L" ";
    args += L"\"" + DllPath(x64) + L"\"";

    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> cmd(args.begin(), args.end());
    cmd.push_back(L'\0');

    if (!CreateProcessW(tool.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;

    WaitForSingleObject(pi.hProcess, 60 * 1000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exitCode) *exitCode = code;
    return code == 0;
}

std::wstring L(UINT id, const wchar_t* fallback)
{
    return Lang::Str(id, fallback);
}

} // namespace

// ═════════════════════════════════════════════════════════
// CPageArchiveFldr
// ═════════════════════════════════════════════════════════
namespace {

struct CtxRow
{
    UINT          lang;
    const wchar_t* fallback;
    bool Settings::* field;
};

// Exactly the entries CContextMenu::QueryContextMenu can add, in the
// order it adds them, labelled with the same language ids — so a row
// here reads exactly as the menu entry it governs. A tick removes a
// real menu item, which is the only honest way to present this list.
const CtxRow kCtxRows[] = {
    { LNG_CTX_OPEN,     L"Open archive",            &Settings::ctxOpenInShell   },
    { LNG_CTX_EXTRACT,  L"Extract files...",        &Settings::ctxExtract       },
    { LNG_CTX_EXTHERE,  L"Extract Here",            &Settings::ctxExtractHere   },
    { LNG_CTX_TEST,     L"Test archive",            &Settings::ctxTestArchive   },
    { LNG_CTX_ADD,      L"Add to archive...",       &Settings::ctxAddToArchive  },
    { LNG_CTX_ADDHERE,  L"Add to \"%s\"",           &Settings::ctxCompressHere  },
    { LNG_CTX_EMAIL,    L"Compress and email...",   &Settings::ctxCompressEmail },
    { LNG_CTX_INFO,     L"Archive information",     &Settings::ctxArchiveInfo   },
    { LNG_CTX_SETTINGS, L"ArchiveFldr settings...", &Settings::ctxSettings      },
};

// Row text. "Add to ..." names the archive the menu would really
// create, using the default format from the General settings, so the
// row is a preview rather than a description of one.
std::wstring CtxRowText(int i)
{
    const CtxRow& r = kCtxRows[i];
    if (r.lang != LNG_CTX_ADDHERE)
        return L(r.lang, r.fallback);
    return Lang::Format1(r.lang, r.fallback,
                         L"archive." + Settings::Get().defaultFormat);
}

} // namespace

HWND CPageArchiveFldr::Create(HWND parent)
{
    m_hwnd = CreatePage(IDD_PAGE_ARCHIVEFLDR, parent, DlgProc, this);
    return m_hwnd;
}

void CPageArchiveFldr::Show(bool show)
{
    ShowPageWindow(m_hwnd, show);
}

void CPageArchiveFldr::Place(const RECT& rc)
{
    PlacePage(m_hwnd, rc);
}

void CPageArchiveFldr::FillItems()
{
    m_list = GetDlgItem(m_hwnd, IDC_LIST_CTXITEMS);
    if (!m_list) return;

    ListView_SetExtendedListViewStyle(m_list,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);

    RECT rc{};
    GetClientRect(m_list, &rc);
    AddColumn(m_list, 0, L"", rc.right - rc.left - GetSystemMetrics(SM_CXVSCROLL));

    for (int i = 0; i < (int)ARRAYSIZE(kCtxRows); ++i)
        AddRow(m_list, i, CtxRowText(i).c_str());
}

void CPageArchiveFldr::SyncEnabled()
{
    const bool on = IsDlgButtonChecked(m_hwnd, IDC_CHK_INTEGRATE) == BST_CHECKED;

    // The two per-bitness ticks are the registration itself -- applied
    // by registering that DLL's context menu handler and nothing else
    // -- so all they need is the DLL to be there. Deliberately not
    // "and the base install too": the menu stands on its own, which is
    // what lets "Uninstall 64-bit" take the browsing half away without
    // stranding a menu nobody can then untick.
#ifdef _WIN64
    const bool can64 = DllPresent(true);
#else
    const bool can64 = false;   // 32-bit build: no 64-bit shell to integrate with
#endif
    const bool can32 = DllPresent(false);

    EnableWindow(GetDlgItem(m_hwnd, IDC_CHK_INTEGRATE_64), on && can64);
    EnableWindow(GetDlgItem(m_hwnd, IDC_CHK_INTEGRATE_32), on && can32);
    EnableWindow(GetDlgItem(m_hwnd, IDC_CHK_CASCADED),  on);
    EnableWindow(GetDlgItem(m_hwnd, IDC_CHK_MENUICONS), on);
    EnableWindow(GetDlgItem(m_hwnd, IDC_LBL_CTXITEMS),  on);
    EnableWindow(GetDlgItem(m_hwnd, IDC_LIST_CTXITEMS), on);
}

void CPageArchiveFldr::Load()
{
    const Settings& s = Settings::Get();
    CheckDlgButton(m_hwnd, IDC_CHK_INTEGRATE, s.showContextMenu ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(m_hwnd, IDC_CHK_CASCADED,  s.ctxUseSubMenu   ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(m_hwnd, IDC_CHK_MENUICONS, s.ctxMenuIcons    ? BST_CHECKED : BST_UNCHECKED);

    CheckDlgButton(m_hwnd, IDC_CHK_INTEGRATE_64, s.ctxMenu64 ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(m_hwnd, IDC_CHK_INTEGRATE_32, s.ctxMenu32 ? BST_CHECKED : BST_UNCHECKED);
#ifdef _WIN64
    m_was64 = ContextMenuRegistered(true);
#else
    m_was64 = false;
#endif
    m_was32 = ContextMenuRegistered(false);

    if (m_list)
        for (int i = 0; i < (int)ARRAYSIZE(kCtxRows); ++i)
            ListView_SetCheckState(m_list, i, s.*(kCtxRows[i].field));

    SyncEnabled();
    m_dirty = false;
}

// Just the two integration ticks, re-read from settings that something
// else changed -- an install or an uninstall on the Settings page. The
// rest of the page is left alone on purpose: edits the user has made
// but not applied yet are still theirs.
void CPageArchiveFldr::ReloadIntegration()
{
    if (!m_hwnd) return;
    const Settings& s = Settings::Get();

    CheckDlgButton(m_hwnd, IDC_CHK_INTEGRATE_64, s.ctxMenu64 ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(m_hwnd, IDC_CHK_INTEGRATE_32, s.ctxMenu32 ? BST_CHECKED : BST_UNCHECKED);
#ifdef _WIN64
    m_was64 = ContextMenuRegistered(true);
#else
    m_was64 = false;
#endif
    m_was32 = ContextMenuRegistered(false);

    SyncEnabled();
}

void CPageArchiveFldr::Save()
{
    // No page, no answers. Reading a check box out of a window that was
    // never created returns "unticked", and these ticks now decide
    // whether the context menu stays registered -- a page that failed
    // to create must not be read as the user clearing them.
    if (!m_hwnd) return;

    Settings& s = Settings::Get();
    s.showContextMenu = IsDlgButtonChecked(m_hwnd, IDC_CHK_INTEGRATE) == BST_CHECKED;
    s.ctxUseSubMenu   = IsDlgButtonChecked(m_hwnd, IDC_CHK_CASCADED)  == BST_CHECKED;
    s.ctxMenuIcons    = IsDlgButtonChecked(m_hwnd, IDC_CHK_MENUICONS) == BST_CHECKED;

    s.ctxMenu32 = IsDlgButtonChecked(m_hwnd, IDC_CHK_INTEGRATE_32) == BST_CHECKED;
#ifdef _WIN64
    s.ctxMenu64 = IsDlgButtonChecked(m_hwnd, IDC_CHK_INTEGRATE_64) == BST_CHECKED;
#else
    // A 32-bit settings program leaves the 64-bit flag exactly as it
    // found it: it cannot register that DLL, so it has no business
    // speaking for it.
#endif

    if (m_list)
        for (int i = 0; i < (int)ARRAYSIZE(kCtxRows); ++i)
            s.*(kCtxRows[i].field) = ListView_GetCheckState(m_list, i) != FALSE;

    m_dirty = false;
}

// ── ApplyIntegration ─────────────────────────────────────
//
// The two per-bitness ticks are not a preference the DLL reads and
// acts on later: they decide whether the context menu handler is
// registered at all, so applying one means going to the registry.
//
// Only the menu, though -- regsvr32 /n /i:contextmenu, which reaches
// DllInstall and nothing else. A full re-registration would quietly
// reinstall a base extension the user had just uninstalled, and a full
// /u would take one away that they had not.
//
// regsvr32 rather than calling CRegistry here: the CLSID half of the
// registration lives in the 32- or 64-bit view of the registry, and the
// only reliable way into the other view is a process of that bitness.
// It is the same tool the Install buttons use.
// ─────────────────────────────────────────────────────────
void CPageArchiveFldr::ApplyIntegration()
{
    if (!m_hwnd) return;

    Settings& s = Settings::Get();
    std::wstring report;
    bool allOk = true;
    bool ran   = false;

    auto one = [&](bool x64, bool want, bool was) {
        if (want == was)      return;   // this bitness did not move
        if (!DllPresent(x64)) return;   // and there is nothing to register
        ran = true;
        DWORD code = 0;
        const bool ok = RunRegsvr32(x64, !want, &code, L"contextmenu");
        allOk = allOk && ok;
        if (!ok)
        {
            // Do not leave a checked box claiming registration succeeded.
            (x64 ? s.ctxMenu64 : s.ctxMenu32) = was;
            report += std::wstring(x64 ? L"64-bit" : L"32-bit") +
                      L": failed (regsvr32 exit code " +
                      std::to_wstring((int)code) + L")\r\n";
        }
    };

    HCURSOR prev = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
#ifdef _WIN64
    one(true, s.ctxMenu64, m_was64);
#endif
    one(false, s.ctxMenu32, m_was32);
    SetCursor(prev);

    // Quiet when it worked -- this runs under Apply, which is not the
    // place for a dialog saying nothing happened -- and loud when it
    // did not, because the tick would otherwise claim something that
    // is not true.
    if (ran && !allOk)
    {
        s.Save();
        CheckDlgButton(m_hwnd, IDC_CHK_INTEGRATE_64,
                       s.ctxMenu64 ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(m_hwnd, IDC_CHK_INTEGRATE_32,
                       s.ctxMenu32 ? BST_CHECKED : BST_UNCHECKED);
        MessageBoxW(m_hwnd,
            (L(LNG_INSTALL_FAIL, L"Some parts could not be changed.") +
             L"\r\n\r\n" + report).c_str(),
            L"ArchiveFldr", MB_OK | MB_ICONWARNING);
    }

    if (ran)
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    m_was64 = s.ctxMenu64;
    m_was32 = s.ctxMenu32;
    SyncEnabled();
}

void CPageArchiveFldr::Retranslate()
{
    if (!m_hwnd) return;
    Lang::Apply(m_hwnd, IDD_PAGE_ARCHIVEFLDR);
    if (!m_list) return;
    for (int i = 0; i < (int)ARRAYSIZE(kCtxRows); ++i)
        SetSub(m_list, i, 0, CtxRowText(i).c_str());
}

INT_PTR CALLBACK CPageArchiveFldr::DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    CPageArchiveFldr* p = nullptr;
    if (msg == WM_INITDIALOG)
    {
        p = (CPageArchiveFldr*)lp;
        SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)p);
        p->m_hwnd = hDlg;
        p->FillItems();
        p->Retranslate();     // Lang::Apply plus the list row captions
        p->Load();
        return TRUE;
    }

    p = (CPageArchiveFldr*)GetWindowLongPtrW(hDlg, DWLP_USER);
    if (!p) return FALSE;

    switch (msg)
    {
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED)
        {
            if (LOWORD(wp) == IDC_CHK_INTEGRATE) p->SyncEnabled();
            p->m_dirty = true;
            MarkDirty(hDlg);
            return TRUE;
        }
        break;

    case WM_NOTIFY:
    {
        auto* nm = (LPNMHDR)lp;
        if (nm->idFrom == IDC_LIST_CTXITEMS && nm->code == LVN_ITEMCHANGED)
        {
            auto* lv = (LPNMLISTVIEW)lp;
            // Only a state change that moves the check box counts; the
            // selection moving around is not an edit.
            if ((lv->uChanged & LVIF_STATE) &&
                ((lv->uOldState ^ lv->uNewState) & LVIS_STATEIMAGEMASK))
            {
                p->m_dirty = true;
                MarkDirty(hDlg);
            }
        }
        break;
    }
    }
    return FALSE;
}

// ═════════════════════════════════════════════════════════
// CPageFolders
// ═════════════════════════════════════════════════════════
HWND CPageFolders::Create(HWND parent)
{
    m_hwnd = CreatePage(IDD_PAGE_FOLDERS, parent, DlgProc, this);
    return m_hwnd;
}

void CPageFolders::Show(bool show)
{
    ShowPageWindow(m_hwnd, show);
}

void CPageFolders::Place(const RECT& rc)
{
    PlacePage(m_hwnd, rc);
}

void CPageFolders::SyncEnabled()
{
    const bool spec = IsDlgButtonChecked(m_hwnd, IDC_RAD_TEMP_SPEC) == BST_CHECKED;
    EnableWindow(GetDlgItem(m_hwnd, IDC_EDIT_WORKDIR), spec);
    EnableWindow(GetDlgItem(m_hwnd, IDC_BTN_WORKDIR),  spec);
}

void CPageFolders::Browse()
{
    wchar_t current[MAX_PATH] = {};
    GetDlgItemTextW(m_hwnd, IDC_EDIT_WORKDIR, current, ARRAYSIZE(current));

    BROWSEINFOW bi{};
    const std::wstring title = L(LNG_BROWSE_WORK, L"Choose the working folder:");
    bi.hwndOwner = m_hwnd;
    bi.lpszTitle = title.c_str();
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;

    wchar_t picked[MAX_PATH] = {};
    if (SHGetPathFromIDListW(pidl, picked))
    {
        SetDlgItemTextW(m_hwnd, IDC_EDIT_WORKDIR, picked);
        m_dirty = true;
        MarkDirty(m_hwnd);
    }
    CoTaskMemFree(pidl);
}

void CPageFolders::Load()
{
    const Settings& s = Settings::Get();
    const bool spec = (s.WorkDir() == WorkDirMode::Specified);
    CheckDlgButton(m_hwnd, IDC_RAD_TEMP_SYSTEM, spec ? BST_UNCHECKED : BST_CHECKED);
    CheckDlgButton(m_hwnd, IDC_RAD_TEMP_SPEC,   spec ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemTextW(m_hwnd, IDC_EDIT_WORKDIR, s.tempDirPath.c_str());
    SyncEnabled();
    m_dirty = false;
}

void CPageFolders::Save()
{
    Settings& s = Settings::Get();
    s.useTempDir = IsDlgButtonChecked(m_hwnd, IDC_RAD_TEMP_SPEC) == BST_CHECKED;

    wchar_t buf[MAX_PATH * 2] = {};
    GetDlgItemTextW(m_hwnd, IDC_EDIT_WORKDIR, buf, ARRAYSIZE(buf));
    s.tempDirPath = buf;
    m_dirty = false;
}

void CPageFolders::Retranslate()
{
    if (m_hwnd) Lang::Apply(m_hwnd, IDD_PAGE_FOLDERS);
}

INT_PTR CALLBACK CPageFolders::DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    CPageFolders* p = nullptr;
    if (msg == WM_INITDIALOG)
    {
        p = (CPageFolders*)lp;
        SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)p);
        p->m_hwnd = hDlg;
        Lang::Apply(hDlg, IDD_PAGE_FOLDERS);
        p->Load();
        return TRUE;
    }

    p = (CPageFolders*)GetWindowLongPtrW(hDlg, DWLP_USER);
    if (!p) return FALSE;

    if (msg == WM_COMMAND)
    {
        switch (LOWORD(wp))
        {
        case IDC_BTN_WORKDIR:
            if (HIWORD(wp) == BN_CLICKED) { p->Browse(); return TRUE; }
            break;

        case IDC_RAD_TEMP_SYSTEM:
        case IDC_RAD_TEMP_SPEC:
            if (HIWORD(wp) == BN_CLICKED)
            {
                p->SyncEnabled();
                p->m_dirty = true;
                MarkDirty(hDlg);
                return TRUE;
            }
            break;

        case IDC_EDIT_WORKDIR:
            if (HIWORD(wp) == EN_CHANGE)
            {
                p->m_dirty = true;
                MarkDirty(hDlg);
                return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

// ═════════════════════════════════════════════════════════
// CPageInstall
// ═════════════════════════════════════════════════════════
HWND CPageInstall::Create(HWND parent)
{
    m_hwnd = CreatePage(IDD_PAGE_SETTINGS, parent, DlgProc, this);
    return m_hwnd;
}

void CPageInstall::Show(bool show)
{
    ShowPageWindow(m_hwnd, show);
}

void CPageInstall::Place(const RECT& rc)
{
    PlacePage(m_hwnd, rc);
}

void CPageInstall::RefreshState()
{
    if (!m_hwnd) return;

    std::wstring text;
    auto line = [&](bool x64) {
        const wchar_t* which = x64 ? L"64-bit" : L"32-bit";
        if (!DllPresent(x64))
        {
            text += std::wstring(which) + L": " +
                    PathFindFileNameW(DllPath(x64).c_str()) +
                    L" not found next to this program\r\n";
            return;
        }
        // Four answers, because the two halves are installed
        // separately: the browsing extension, the right-click menu,
        // both, or neither.
        const bool reg = DllRegistered(x64);
        const bool ctx = ContextMenuRegistered(x64);
        std::wstring state;
        if (reg && ctx)
            state = L(LNG_STATE_CTXMENU, L"registered, with context menu");
        else if (reg)
            state = L(LNG_STATE_YES, L"registered");
        else if (ctx)
            state = L(LNG_STATE_CTXONLY, L"context menu only");
        else
            state = L(LNG_STATE_NO, L"not registered");
        text += std::wstring(which) + L": " + state + L"\r\n";
    };

#ifdef _WIN64
    line(true);
    line(false);
#else
    line(false);
#endif

    SetDlgItemTextW(m_hwnd, IDC_LBL_INSTALL_STATE, text.c_str());

    SetDlgItemTextW(m_hwnd, IDC_LBL_INSTALL_NOTE,
        L(LNG_INSTALL_NOTE,
          L"Install registers one build of the shell extension so Explorer "
          L"can browse archives as folders, and lists ArchiveFldr in "
          L"Settings > Default apps for its archive file types. Uninstall "
          L"takes that same half back out, leaving the files on disk.\r\n\r\n"
          L"Neither touches the right-click menu: that is switched on and "
          L"off per bitness on the ArchiveFldr page.\r\n\r\n"
          L"Explorer caches shell extensions, so sign out and back in — or "
          L"restart Explorer — for a change to take effect everywhere.")
        .c_str());

    // One pair of buttons per bitness, live only while that DLL is
    // there: both directions go through its own regsvr32, so the file
    // has to exist even to be unregistered. A 32-bit settings program
    // never offers the 64-bit pair -- it runs on a 32-bit Windows, and
    // could not start a 64-bit regsvr32 even if a 64-bit DLL were
    // sitting beside it.
#ifdef _WIN64
    const bool have64 = DllPresent(true);
#else
    const bool have64 = false;
#endif
    const bool have32 = DllPresent(false);

    EnableWindow(GetDlgItem(m_hwnd, IDC_BTN_INSTALL_64),   have64);
    EnableWindow(GetDlgItem(m_hwnd, IDC_BTN_INSTALL_32),   have32);
    EnableWindow(GetDlgItem(m_hwnd, IDC_BTN_UNINSTALL_64), have64);
    EnableWindow(GetDlgItem(m_hwnd, IDC_BTN_UNINSTALL_32), have32);
}

void CPageInstall::Run(bool install, bool x64)
{
    // Both directions are per bitness and both are the base extension
    // only: browsing archives as folders, thumbnails, preview, and the
    // Default apps entry. The right-click menu is the ArchiveFldr
    // page's business and neither button here disturbs it, so
    // uninstalling the browsing half leaves an integrated menu
    // standing until its tick is cleared.
    Settings& s = Settings::Get();

    // Is the other build still registered? Everything shared -- the
    // file types, and the Default apps entry with them -- stays while
    // it is, so this is also the question of whether the stored
    // "list in Default apps" answer still has an owner.
    bool otherStillThere = false;   // 32-bit Windows: there is no other
#ifdef _WIN64
    otherStillThere = DllRegistered(!x64);
#endif

    // The settings have to be on disk before regsvr32 starts:
    // registration reads the format choices and Default-apps flag
    // straight out of HKLM.
    if (install)
    {
        s.registerAsDefaultApp = true;
    }
    else if (!otherStillThere)
    {
        // The last build goes, and the Default apps entry with it.
        s.registerAsDefaultApp = false;
    }
    s.Save();

    DWORD code = 0;
    HCURSOR prev = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    // /n /i:base — the base half, through DllInstall. Not a plain
    // register or /u, either of which would take the context menu with
    // it.
    const bool ok = DllPresent(x64) &&
                    RunRegsvr32(x64, !install, &code, L"base");
    SetCursor(prev);

    const std::wstring report =
        std::wstring(x64 ? L"64-bit" : L"32-bit") + L": " +
        (ok ? std::wstring(L"OK")
            : (!DllPresent(x64)
                   ? std::wstring(PathFindFileNameW(DllPath(x64).c_str())) +
                     L" not found"
                   : L"failed (regsvr32 exit code " +
                     std::to_wstring((int)code) + L")")) +
        L"\r\n";

    const std::wstring head = ok
        ? (install ? L(LNG_INSTALL_OK,   L"ArchiveFldr was installed.")
                   : L(LNG_UNINSTALL_OK, L"ArchiveFldr was removed."))
        : L(LNG_INSTALL_FAIL, L"Some parts could not be changed.");

    MessageBoxW(m_hwnd, (head + L"\r\n\r\n" + report).c_str(), L"ArchiveFldr",
                MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));

    // Tell the shell the association table moved, so open icons and verbs
    // refresh without a sign-out where the shell is willing.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    RefreshState();

    // What is installed has just changed, and the ArchiveFldr page's
    // integration ticks are a view of that same state.
    if (HWND owner = GetParent(m_hwnd))
        if (auto* dlg = (CSettingsDialog*)GetWindowLongPtrW(owner, DWLP_USER))
            dlg->OnInstallChanged();
}

void CPageInstall::Load() { RefreshState(); }

void CPageInstall::Retranslate()
{
    if (!m_hwnd) return;
    Lang::Apply(m_hwnd, IDD_PAGE_SETTINGS);
    RefreshState();
}

INT_PTR CALLBACK CPageInstall::DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    CPageInstall* p = nullptr;
    if (msg == WM_INITDIALOG)
    {
        p = (CPageInstall*)lp;
        SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)p);
        p->m_hwnd = hDlg;
        Lang::Apply(hDlg, IDD_PAGE_SETTINGS);
        p->RefreshState();
        return TRUE;
    }

    p = (CPageInstall*)GetWindowLongPtrW(hDlg, DWLP_USER);
    if (!p) return FALSE;

    if (msg == WM_COMMAND && HIWORD(wp) == BN_CLICKED)
    {
        if (LOWORD(wp) == IDC_BTN_INSTALL_32)   { p->Run(true,  false); return TRUE; }
        if (LOWORD(wp) == IDC_BTN_INSTALL_64)   { p->Run(true,  true);  return TRUE; }
        if (LOWORD(wp) == IDC_BTN_UNINSTALL_32) { p->Run(false, false); return TRUE; }
        if (LOWORD(wp) == IDC_BTN_UNINSTALL_64) { p->Run(false, true);  return TRUE; }
    }
    return FALSE;
}

// ═════════════════════════════════════════════════════════
// CPageLanguage
// ═════════════════════════════════════════════════════════
HWND CPageLanguage::Create(HWND parent)
{
    m_hwnd = CreatePage(IDD_PAGE_LANGUAGE, parent, DlgProc, this);
    return m_hwnd;
}

void CPageLanguage::Show(bool show)
{
    ShowPageWindow(m_hwnd, show);
}

void CPageLanguage::Place(const RECT& rc)
{
    PlacePage(m_hwnd, rc);
}

void CPageLanguage::Load()
{
    m_list = GetDlgItem(m_hwnd, IDC_LIST_LANG);
    if (!m_list) return;

    ListView_SetExtendedListViewStyle(m_list, LVS_EX_FULLROWSELECT);
    ListView_DeleteAllItems(m_list);
    while (ListView_DeleteColumn(m_list, 0)) {}

    AddColumn(m_list, 0, L(LNG_COL_LANGUAGE, L"Language").c_str(),  160);
    AddColumn(m_list, 1, L(LNG_COL_LANG_EN,  L"English name").c_str(), 150);

    m_codes.clear();
    const std::wstring cur = Settings::Get().language;

    int row = 0, sel = 0;
    for (const auto& e : Lang::Available())
    {
        AddRow(m_list, row, e.native.c_str());
        SetSub(m_list, row, 1, e.english.c_str());
        m_codes.push_back(e.code);
        if (_wcsicmp(e.code.c_str(), cur.c_str()) == 0) sel = row;
        ++row;
    }

    if (!m_codes.empty())
    {
        ListView_SetItemState(m_list, sel, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(m_list, sel, FALSE);
    }
    m_dirty = false;
}

std::wstring CPageLanguage::Selected() const
{
    if (!m_list) return Settings::Get().language;
    const int sel = ListView_GetNextItem(m_list, -1, LVNI_SELECTED);
    if (sel < 0 || (size_t)sel >= m_codes.size())
        return Settings::Get().language;
    return m_codes[(size_t)sel];
}

void CPageLanguage::Save()
{
    Settings::Get().language = Selected();
    m_dirty = false;
}

void CPageLanguage::Retranslate()
{
    if (!m_hwnd) return;
    Lang::Apply(m_hwnd, IDD_PAGE_LANGUAGE);
    Load();
}

INT_PTR CALLBACK CPageLanguage::DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    UNREFERENCED_PARAMETER(wp);   // this page listens to the list, not to commands

    CPageLanguage* p = nullptr;
    if (msg == WM_INITDIALOG)
    {
        p = (CPageLanguage*)lp;
        SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)p);
        p->m_hwnd = hDlg;
        Lang::Apply(hDlg, IDD_PAGE_LANGUAGE);
        p->Load();
        return TRUE;
    }

    p = (CPageLanguage*)GetWindowLongPtrW(hDlg, DWLP_USER);
    if (!p) return FALSE;

    if (msg == WM_NOTIFY)
    {
        auto* nm = (LPNMHDR)lp;
        if (nm->idFrom == IDC_LIST_LANG && nm->code == LVN_ITEMCHANGED)
        {
            auto* lv = (LPNMLISTVIEW)lp;
            if ((lv->uChanged & LVIF_STATE) &&
                (lv->uNewState & LVIS_SELECTED) &&
                !(lv->uOldState & LVIS_SELECTED))
            {
                p->m_dirty = true;
                MarkDirty(hDlg);
            }
        }
    }
    return FALSE;
}

// ═════════════════════════════════════════════════════════
// CSettingsDialog
// ═════════════════════════════════════════════════════════
CSettingsDialog::CSettingsDialog()  = default;
CSettingsDialog::~CSettingsDialog() = default;

bool CSettingsDialog::Show(HWND parent)
{
    return DialogBoxParamW(SelfInstance(), MAKEINTRESOURCEW(IDD_OPTIONS),
                           parent, DlgProc, (LPARAM)this) == IDOK;
}

void CSettingsDialog::EnableApply(bool en)
{
    if (m_hDlg) EnableWindow(GetDlgItem(m_hDlg, IDC_BTN_APPLY), en);
}

void CSettingsDialog::PlacePages()
{
    if (!m_hTab) return;

    RECT rc{};
    GetWindowRect(m_hTab, &rc);
    MapWindowPoints(nullptr, m_hDlg, (LPPOINT)&rc, 2);
    TabCtrl_AdjustRect(m_hTab, FALSE, &rc);

    for (auto& pg : m_pages) pg->Place(rc);
}

void CSettingsDialog::ShowPage(int idx)
{
    if (idx < 0 || idx >= (int)m_pages.size()) return;
    for (int i = 0; i < (int)m_pages.size(); ++i)
        m_pages[(size_t)i]->Show(i == idx);
    m_cur = idx;
}

void CSettingsDialog::Retranslate()
{
    Lang::Apply(m_hDlg, IDD_OPTIONS);
    for (auto& pg : m_pages) pg->Retranslate();

    // Tab captions are keyed on each page's dialog id.
    for (int i = 0; i < (int)m_pages.size(); ++i)
    {
        std::wstring t = Lang::Str(m_pages[(size_t)i]->DialogId(),
                                   m_pages[(size_t)i]->Title());
        TCITEMW ti{};
        ti.mask    = TCIF_TEXT;
        ti.pszText = (LPWSTR)t.c_str();
        TabCtrl_SetItem(m_hTab, i, &ti);
    }
}

void CSettingsDialog::OnInit(HWND hDlg)
{
    m_hDlg = hDlg;
    SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)this);

    HINSTANCE inst = SelfInstance();
    if (HANDLE sm = LoadImageW(inst, MAKEINTRESOURCEW(IDI_ARCHIVEFLDR),
                               IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                               GetSystemMetrics(SM_CYSMICON), 0))
        SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)sm);
    if (HANDLE big = LoadImageW(inst, MAKEINTRESOURCEW(IDI_ARCHIVEFLDR),
                                IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                GetSystemMetrics(SM_CYICON), 0))
        SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)big);

    m_hTab = GetDlgItem(hDlg, IDC_TAB_PAGES);

    // These pages coordinate after registration or language changes, so
    // keep non-owning pointers before the vector takes ownership.
    auto lang = std::make_unique<CPageLanguage>();
    m_langPage = lang.get();
    auto ctx = std::make_unique<CPageArchiveFldr>();
    m_ctxPage = ctx.get();
    auto setup = std::make_unique<CPageInstall>();
    m_installPage = setup.get();

    m_pages.push_back(std::move(ctx));
    m_pages.push_back(std::make_unique<CPageFolders>());
    m_pages.push_back(std::move(setup));
    m_pages.push_back(std::move(lang));

    for (int i = 0; i < (int)m_pages.size(); ++i)
    {
        std::wstring t = Lang::Str(m_pages[(size_t)i]->DialogId(),
                                   m_pages[(size_t)i]->Title());
        TCITEMW ti{};
        ti.mask    = TCIF_TEXT;
        ti.pszText = (LPWSTR)t.c_str();
        TabCtrl_InsertItem(m_hTab, i, &ti);

        m_pages[(size_t)i]->Create(hDlg);
    }

    Lang::Apply(hDlg, IDD_OPTIONS);
    PlacePages();
    ShowPage(0);
    EnableApply(false);
}

bool CSettingsDialog::OnApply()
{
    const std::wstring before = Settings::Get().language;

    for (auto& pg : m_pages) pg->Save();
    Settings::Get().Save();

    // Associations changed? Rewrite the per-extension OpenWithProgids so
    // the stored per-format choices take effect without a full
    // re-register. This part is unconditional: being offered under "Open
    // with" has nothing to do with being listed in Default apps, and a
    // user who never asked for the second still expects the first.
    CRegistry::RefreshOpenWithProgids();

    // The Default apps registration follows the stored preference, the
    // same way DllRegisterServer does. Applying it unconditionally used
    // to re-publish the Capabilities key every time OK was pressed —
    // including after the user had turned that off, which quietly undid
    // their choice. Needs the DLL's own path, which is what the
    // Capabilities icon points at.
#ifdef _WIN64
    const std::wstring dll = DllPath(true);
#else
    const std::wstring dll = DllPath(false);
#endif
    if (Settings::Get().registerAsDefaultApp)
    {
        if (PathFileExistsW(dll.c_str()))
            CRegistry::RegisterCapabilities(dll.c_str());
    }
    else
    {
        CRegistry::UnregisterCapabilities();
    }

    // The per-bitness context-menu checkboxes control real COM/shell
    // registration. Apply them only after Save(), because DllInstall reads
    // those persisted flags in its own regsvr32 process.
    if (m_ctxPage)     m_ctxPage->ApplyIntegration();
    if (m_installPage) m_installPage->Load();

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    if (_wcsicmp(before.c_str(), Settings::Get().language.c_str()) != 0)
    {
        Lang::Load(Settings::Get().language);
        Retranslate();
    }

    for (auto& pg : m_pages) pg->ClearDirty();
    EnableApply(false);
    return true;
}

void CSettingsDialog::OnInstallChanged()
{
    if (m_ctxPage) m_ctxPage->ReloadIntegration();
}

void CSettingsDialog::OnOK()
{
    OnApply();
    EndDialog(m_hDlg, IDOK);
}

void CSettingsDialog::OnCancel()
{
    EndDialog(m_hDlg, IDCANCEL);
}

INT_PTR CALLBACK CSettingsDialog::DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    CSettingsDialog* p = nullptr;
    if (msg == WM_INITDIALOG)
    {
        p = (CSettingsDialog*)lp;
        p->OnInit(hDlg);
        return TRUE;
    }
    p = (CSettingsDialog*)GetWindowLongPtrW(hDlg, DWLP_USER);
    if (!p) return FALSE;
    return p->WndProc(hDlg, msg, wp, lp);
}

INT_PTR CSettingsDialog::WndProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    UNREFERENCED_PARAMETER(hDlg);   // the handler works off m_hDlg / m_hTab

    switch (msg)
    {
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_BTN_OK:
        case IDOK:     OnOK();     return TRUE;
        case IDC_BTN_CANCEL:
        case IDCANCEL: OnCancel(); return TRUE;
        case IDC_BTN_APPLY: OnApply(); return TRUE;
        }
        break;

    case WM_NOTIFY:
    {
        auto* nm = (LPNMHDR)lp;
        if (nm->idFrom == IDC_TAB_PAGES && nm->code == TCN_SELCHANGE)
        {
            ShowPage(TabCtrl_GetCurSel(m_hTab));
            return TRUE;
        }
        break;
    }

    case WM_CLOSE:
        OnCancel();
        return TRUE;
    }
    return FALSE;
}

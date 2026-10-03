// AddToArchiveDialog.cpp — see AddToArchiveDialog.h
#include "stdafx.h"
#include "AddToArchiveDialog.h"
#include "Lang.h"
#include "Settings.h"
#include "../res/resource.h"
#include <commdlg.h>

namespace {

HINSTANCE SelfInstance()
{
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&SelfInstance, &h);
    return (HINSTANCE)h;
}

void EnsureLangLoaded()
{
    static bool done = false;
    if (done) return;
    done = true;
    Lang::Load(Settings::Get().language);
}

// The level steps every archiver shows, and the -mx value each means.
struct LevelDef { const wchar_t* label; int value; };
const LevelDef kLevels[] = {
    { L"Store",   0 },
    { L"Fastest", 1 },
    { L"Fast",    3 },
    { L"Normal",  5 },
    { L"Maximum", 7 },
    { L"Ultra",   9 },
};

// Threads offered. 0 = let 7-Zip decide from the machine.
struct ThreadDef { const wchar_t* label; int value; };
const ThreadDef kThreads[] = {
    { L"Auto", 0 },
    { L"1",    1 },
    { L"2",    2 },
    { L"4",    4 },
    { L"8",    8 },
};

struct State
{
    const AddToArchiveDialog::Request* rq;
    AddToArchiveDialog::Result*        out;
    std::vector<std::wstring>          formats;   // combo row -> handler name
};

std::wstring GetText(HWND hDlg, int id)
{
    HWND h = GetDlgItem(hDlg, id);
    const int len = GetWindowTextLengthW(h);
    std::wstring s((size_t)len, L'\0');
    if (len) GetWindowTextW(h, &s[0], len + 1);
    return s;
}

std::wstring CurrentFormat(HWND hDlg, const State* st)
{
    const int sel = (int)SendDlgItemMessageW(hDlg, IDC_CMB_ADD_FORMAT,
                                             CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)st->formats.size())
        return st->rq->format;
    return st->formats[(size_t)sel];
}

void FillCombo(HWND hDlg, int id, const std::vector<std::wstring>& rows,
               int select)
{
    HWND h = GetDlgItem(hDlg, id);
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    for (const auto& r : rows)
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)r.c_str());
    if (!rows.empty())
        SendMessageW(h, CB_SETCURSEL, (WPARAM)select, 0);
    EnableWindow(h, !rows.empty());
}

// Re-fill everything that depends on the chosen format: methods,
// encryption methods, the 7z-only ticks, and the path's extension.
void SyncFormat(HWND hDlg, State* st)
{
    const std::wstring fmt = CurrentFormat(hDlg, st);
    const bool is7z = _wcsicmp(fmt.c_str(), L"7z") == 0;

    FillCombo(hDlg, IDC_CMB_ADD_METHOD, ArchiveWriter::MethodsFor(fmt), 0);

    const auto enc = ArchiveWriter::EncryptionMethodsFor(fmt);
    FillCombo(hDlg, IDC_CMB_ADD_ENCMETHOD, enc, 0);

    // A format with no cipher gets its password boxes switched off, so
    // a password typed there cannot be silently ignored.
    const bool canEncrypt = !enc.empty();
    EnableWindow(GetDlgItem(hDlg, IDC_EDIT_ADD_PW),     canEncrypt);
    EnableWindow(GetDlgItem(hDlg, IDC_EDIT_ADD_PW2),    canEncrypt);
    EnableWindow(GetDlgItem(hDlg, IDC_CHK_ADD_SHOWPW),  canEncrypt);
    if (!canEncrypt)
    {
        SetDlgItemTextW(hDlg, IDC_EDIT_ADD_PW,  L"");
        SetDlgItemTextW(hDlg, IDC_EDIT_ADD_PW2, L"");
    }

    EnableWindow(GetDlgItem(hDlg, IDC_CHK_ADD_SOLID),    is7z);
    EnableWindow(GetDlgItem(hDlg, IDC_CHK_ADD_ENCNAMES), is7z && canEncrypt);
    if (!is7z)
    {
        CheckDlgButton(hDlg, IDC_CHK_ADD_SOLID,    BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_CHK_ADD_ENCNAMES, BST_UNCHECKED);
    }

    // Swap the path's extension to match, but only the extension: the
    // user's name and folder are theirs.
    std::wstring path = GetText(hDlg, IDC_EDIT_ADD_PATH);
    if (!path.empty())
    {
        const std::wstring newExt = ArchiveWriter::DefaultExtensionFor(fmt);
        const wchar_t* oldExt = PathFindExtensionW(path.c_str());
        if (oldExt && *oldExt)
            path.resize((size_t)(oldExt - path.c_str()));
        path += newExt;
        SetDlgItemTextW(hDlg, IDC_EDIT_ADD_PATH, path.c_str());
    }
}

void ApplyShowPassword(HWND hDlg)
{
    const bool show =
        IsDlgButtonChecked(hDlg, IDC_CHK_ADD_SHOWPW) == BST_CHECKED;
    const wchar_t ch = show ? 0 : L'\x25CF';
    SendDlgItemMessageW(hDlg, IDC_EDIT_ADD_PW,  EM_SETPASSWORDCHAR, ch, 0);
    SendDlgItemMessageW(hDlg, IDC_EDIT_ADD_PW2, EM_SETPASSWORDCHAR, ch, 0);
    InvalidateRect(GetDlgItem(hDlg, IDC_EDIT_ADD_PW),  nullptr, TRUE);
    InvalidateRect(GetDlgItem(hDlg, IDC_EDIT_ADD_PW2), nullptr, TRUE);
}

void Browse(HWND hDlg, State* st)
{
    const std::wstring fmt = CurrentFormat(hDlg, st);
    const std::wstring ext = ArchiveWriter::DefaultExtensionFor(fmt);

    wchar_t file[MAX_PATH * 2] = {};
    wcsncpy_s(file, GetText(hDlg, IDC_EDIT_ADD_PATH).c_str(), _TRUNCATE);

    // "zip archive (*.zip)\0*.zip\0All files\0*.*\0"
    std::wstring filter = fmt + L" archive (*" + ext + L")";
    filter.push_back(L'\0');
    filter += L"*" + ext;
    filter.push_back(L'\0');
    filter += L"All files";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');

    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner    = hDlg;
    ofn.lpstrFilter  = filter.c_str();
    ofn.lpstrFile    = file;
    ofn.nMaxFile     = ARRAYSIZE(file);
    ofn.lpstrDefExt  = ext.empty() ? nullptr : ext.c_str() + 1;
    ofn.Flags        = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
                       OFN_NOREADONLYRETURN;
    if (GetSaveFileNameW(&ofn))
        SetDlgItemTextW(hDlg, IDC_EDIT_ADD_PATH, file);
}

bool TakeResult(HWND hDlg, State* st)
{
    AddToArchiveDialog::Result& r = *st->out;

    r.path = GetText(hDlg, IDC_EDIT_ADD_PATH);
    if (r.path.empty())
    {
        MessageBoxW(hDlg,
            Lang::Str(2063, L"Choose where the archive should be written.").c_str(),
            L"ArchiveFldr", MB_ICONWARNING | MB_OK);
        return false;
    }

    const std::wstring pw  = GetText(hDlg, IDC_EDIT_ADD_PW);
    const std::wstring pw2 = GetText(hDlg, IDC_EDIT_ADD_PW2);
    if (pw != pw2)
    {
        MessageBoxW(hDlg,
            Lang::Str(2061, L"The two passwords do not match.").c_str(),
            L"ArchiveFldr", MB_ICONWARNING | MB_OK);
        SetFocus(GetDlgItem(hDlg, IDC_EDIT_ADD_PW2));
        return false;
    }

    r.opt = ArchiveWriter::Options{};
    r.opt.format = CurrentFormat(hDlg, st);

    int sel = (int)SendDlgItemMessageW(hDlg, IDC_CMB_ADD_LEVEL, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < (int)ARRAYSIZE(kLevels)) r.opt.level = kLevels[sel].value;

    sel = (int)SendDlgItemMessageW(hDlg, IDC_CMB_ADD_METHOD, CB_GETCURSEL, 0, 0);
    {
        const auto methods = ArchiveWriter::MethodsFor(r.opt.format);
        if (sel >= 0 && sel < (int)methods.size()) r.opt.method = methods[(size_t)sel];
    }

    r.opt.password = pw;
    if (!pw.empty())
    {
        sel = (int)SendDlgItemMessageW(hDlg, IDC_CMB_ADD_ENCMETHOD, CB_GETCURSEL, 0, 0);
        const auto enc = ArchiveWriter::EncryptionMethodsFor(r.opt.format);
        if (sel >= 0 && sel < (int)enc.size()) r.opt.encMethod = enc[(size_t)sel];
    }

    r.opt.solid        = IsDlgButtonChecked(hDlg, IDC_CHK_ADD_SOLID)    == BST_CHECKED;
    r.opt.encryptNames = IsDlgButtonChecked(hDlg, IDC_CHK_ADD_ENCNAMES) == BST_CHECKED
                         && !pw.empty();

    sel = (int)SendDlgItemMessageW(hDlg, IDC_CMB_ADD_THREADS, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < (int)ARRAYSIZE(kThreads)) r.opt.threads = kThreads[sel].value;

    return true;
}

INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* st = (State*)GetWindowLongPtrW(hDlg, DWLP_USER);

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        st = (State*)lp;
        SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)st);
        Lang::Apply(hDlg, IDD_ADDTOARCHIVE);

        // Caption carries the count, so "what is about to happen" is in
        // the title bar: "Add 3 files to archive".
        if (st->rq->fileCount)
        {
            std::wstring cap = Lang::Format1(2060,
                L"Add to Archive \u2014 %s file(s)",
                std::to_wstring(st->rq->fileCount));
            SetWindowTextW(hDlg, cap.c_str());
        }

        SetDlgItemTextW(hDlg, IDC_EDIT_ADD_PATH, st->rq->path.c_str());

        // Formats: the one the archive already is when locked, the
        // writable multi-file containers otherwise.
        st->formats.clear();
        if (st->rq->lockFormat)
        {
            st->formats.push_back(st->rq->format);
        }
        else
        {
            // Stable, familiar order; only what this 7z.dll can write.
            for (const wchar_t* f : { L"zip", L"7z", L"tar", L"wim" })
                if (ArchiveWriter::FormatIsWritable(f))
                    st->formats.push_back(f);
            if (st->formats.empty() && !st->rq->format.empty())
                st->formats.push_back(st->rq->format);
        }
        int fmtSel = 0;
        for (size_t i = 0; i < st->formats.size(); ++i)
            if (_wcsicmp(st->formats[i].c_str(), st->rq->format.c_str()) == 0)
                { fmtSel = (int)i; break; }
        FillCombo(hDlg, IDC_CMB_ADD_FORMAT, st->formats, fmtSel);
        if (st->rq->lockFormat)
            EnableWindow(GetDlgItem(hDlg, IDC_CMB_ADD_FORMAT), FALSE);

        // Levels: default from the saved preference.
        {
            std::vector<std::wstring> rows;
            for (const auto& l : kLevels)
                rows.push_back(Lang::Str(2070 + (UINT)(&l - kLevels), l.label));
            int sel = 3;   // Normal
            const int want = (int)Settings::Get().defaultCompLevel;
            for (int i = 0; i < (int)ARRAYSIZE(kLevels); ++i)
                if (kLevels[i].value == want) { sel = i; break; }
            FillCombo(hDlg, IDC_CMB_ADD_LEVEL, rows, sel);
        }

        // Threads.
        {
            std::vector<std::wstring> rows;
            for (const auto& t : kThreads) rows.push_back(t.label);
            FillCombo(hDlg, IDC_CMB_ADD_THREADS, rows, 0);
        }

        if (Settings::Get().createSolidArchive)
            CheckDlgButton(hDlg, IDC_CHK_ADD_SOLID, BST_CHECKED);
        if (Settings::Get().encryptFileNames)
            CheckDlgButton(hDlg, IDC_CHK_ADD_ENCNAMES, BST_CHECKED);

        SyncFormat(hDlg, st);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_CMB_ADD_FORMAT:
            if (HIWORD(wp) == CBN_SELCHANGE) SyncFormat(hDlg, st);
            return TRUE;

        case IDC_BTN_ADD_BROWSE:
            if (HIWORD(wp) == BN_CLICKED && st) Browse(hDlg, st);
            return TRUE;

        case IDC_CHK_ADD_SHOWPW:
            if (HIWORD(wp) == BN_CLICKED) ApplyShowPassword(hDlg);
            return TRUE;

        case IDOK:
            if (st && TakeResult(hDlg, st)) EndDialog(hDlg, IDOK);
            return TRUE;

        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

} // anonymous namespace

namespace AddToArchiveDialog {

bool Show(HWND parent, const Request& rq, Result& out)
{
    EnsureLangLoaded();
    State st{ &rq, &out, {} };
    const INT_PTR rc = DialogBoxParamW(SelfInstance(),
                                       MAKEINTRESOURCEW(IDD_ADDTOARCHIVE),
                                       parent, DlgProc, (LPARAM)&st);
    return rc == IDOK;
}

} // namespace AddToArchiveDialog

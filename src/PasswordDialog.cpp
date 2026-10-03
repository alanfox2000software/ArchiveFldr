// PasswordDialog.cpp — see PasswordDialog.h
#include "stdafx.h"
#include "PasswordDialog.h"
#include "Lang.h"
#include "Settings.h"
#include "../res/resource.h"

namespace {

// The module this code is linked into (the shell DLL or the settings
// exe) — the dialog template has to come from the same place.
HINSTANCE SelfInstance()
{
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&SelfInstance, &h);
    return (HINSTANCE)h;
}

// The shell DLL never loads a language on its own; do it once, lazily,
// so the prompt comes up translated when a translation exists.
void EnsureLangLoaded()
{
    static bool done = false;
    if (done) return;
    done = true;
    Lang::Load(Settings::Get().language);
}

struct Params
{
    const std::wstring* what;     // archive (file) name for the caption
    const std::wstring* reason;   // one-line explanation, may be empty
    std::wstring*       out;      // receives the password on OK
    bool                confirm;  // show + require the Confirm box
};

// Password strength, 0..100, same rough scale every archiver uses:
// length does most of the work, variety the rest.
int StrengthOf(const std::wstring& pw)
{
    if (pw.empty()) return 0;
    int score = (int)pw.size() * 7;
    if (score > 60) score = 60;
    bool lower = false, upper = false, digit = false, other = false;
    for (wchar_t c : pw)
    {
        if (iswlower(c)) lower = true;
        else if (iswupper(c)) upper = true;
        else if (iswdigit(c)) digit = true;
        else other = true;
    }
    score += (lower + upper + digit + other) * 10;
    return score > 100 ? 100 : score;
}

void UpdateStrength(HWND hDlg)
{
    wchar_t buf[256] = {};
    GetDlgItemTextW(hDlg, IDC_EDIT_PASSWORD, buf, ARRAYSIZE(buf));
    const int s = StrengthOf(buf);
    SendDlgItemMessageW(hDlg, IDC_PROGRESS_STRENGTH, PBM_SETPOS, s, 0);

    const wchar_t* label =
        s == 0  ? L"" :
        s < 40  ? L"weak" :
        s < 70  ? L"reasonable" : L"strong";
    SetDlgItemTextW(hDlg, IDC_STATIC_STRENGTH, label);
    SecureZeroMemory(buf, sizeof(buf));
}

void ApplyShowPassword(HWND hDlg)
{
    const bool show =
        IsDlgButtonChecked(hDlg, IDC_CHK_SHOW_PASSWORD) == BST_CHECKED;
    const wchar_t ch = show ? 0 : L'\x25CF';   // ● — the Vista+ dot
    SendDlgItemMessageW(hDlg, IDC_EDIT_PASSWORD,   EM_SETPASSWORDCHAR, ch, 0);
    SendDlgItemMessageW(hDlg, IDC_EDIT_CONFIRM_PW, EM_SETPASSWORDCHAR, ch, 0);
    InvalidateRect(GetDlgItem(hDlg, IDC_EDIT_PASSWORD),   nullptr, TRUE);
    InvalidateRect(GetDlgItem(hDlg, IDC_EDIT_CONFIRM_PW), nullptr, TRUE);
}

INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* p = (Params*)GetWindowLongPtrW(hDlg, DWLP_USER);

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        p = (Params*)lp;
        SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)p);
        Lang::Apply(hDlg, IDD_PASSWORD);

        if (p->what && !p->what->empty())
            SetWindowTextW(hDlg, (Lang::Str(IDD_PASSWORD, L"Enter Password") +
                                  L" \u2014 " + *p->what).c_str());
        if (p->reason && !p->reason->empty())
            SetDlgItemTextW(hDlg, IDC_LBL_PW_REASON, p->reason->c_str());

        if (!p->confirm)
        {
            // Asking for an EXISTING archive's password: no second box
            // and no strength meter — nothing is being chosen, only
            // recalled, and grading a password that is already set
            // would be noise.
            ShowWindow(GetDlgItem(hDlg, IDC_LBL_CONFIRM_PW),     SW_HIDE);
            ShowWindow(GetDlgItem(hDlg, IDC_EDIT_CONFIRM_PW),    SW_HIDE);
            ShowWindow(GetDlgItem(hDlg, IDC_LBL_STRENGTH),       SW_HIDE);
            ShowWindow(GetDlgItem(hDlg, IDC_PROGRESS_STRENGTH),  SW_HIDE);
            ShowWindow(GetDlgItem(hDlg, IDC_STATIC_STRENGTH),    SW_HIDE);
        }
        // "Encrypt file names" is a writer's option; the Add to Archive
        // dialog owns it. Here it would either be ignored or mislead.
        ShowWindow(GetDlgItem(hDlg, IDC_CHK_ENCRYPT_HEADER), SW_HIDE);

        SendDlgItemMessageW(hDlg, IDC_PROGRESS_STRENGTH, PBM_SETRANGE32, 0, 100);
        UpdateStrength(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_EDIT_PASSWORD));
        return FALSE;   // focus set by hand
    }

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_EDIT_PASSWORD:
            if (HIWORD(wp) == EN_CHANGE) UpdateStrength(hDlg);
            return TRUE;

        case IDC_CHK_SHOW_PASSWORD:
            if (HIWORD(wp) == BN_CLICKED) ApplyShowPassword(hDlg);
            return TRUE;

        case IDOK:
        {
            if (!p) return TRUE;
            wchar_t pw[256] = {}, pw2[256] = {};
            GetDlgItemTextW(hDlg, IDC_EDIT_PASSWORD, pw, ARRAYSIZE(pw));
            if (p->confirm)
            {
                GetDlgItemTextW(hDlg, IDC_EDIT_CONFIRM_PW, pw2, ARRAYSIZE(pw2));
                if (wcscmp(pw, pw2) != 0)
                {
                    MessageBoxW(hDlg,
                        Lang::Str(2061, L"The two passwords do not match.").c_str(),
                        L"ArchiveFldr", MB_ICONWARNING | MB_OK);
                    SecureZeroMemory(pw,  sizeof(pw));
                    SecureZeroMemory(pw2, sizeof(pw2));
                    SetFocus(GetDlgItem(hDlg, IDC_EDIT_CONFIRM_PW));
                    return TRUE;
                }
            }
            if (p->out) *p->out = pw;
            SecureZeroMemory(pw,  sizeof(pw));
            SecureZeroMemory(pw2, sizeof(pw2));
            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool Run(HWND parent, const std::wstring& what, const std::wstring& reason,
         std::wstring& out, bool confirm)
{
    EnsureLangLoaded();
    Params p{ &what, &reason, &out, confirm };
    const INT_PTR rc = DialogBoxParamW(SelfInstance(),
                                       MAKEINTRESOURCEW(IDD_PASSWORD),
                                       parent, DlgProc, (LPARAM)&p);
    return rc == IDOK;
}

} // anonymous namespace

namespace PasswordDialog {

bool Ask(HWND parent, const std::wstring& what, const std::wstring& reason,
         std::wstring& passwordOut)
{
    return Run(parent, what, reason, passwordOut, /*confirm=*/false);
}

bool AskNew(HWND parent, const std::wstring& what, std::wstring& passwordOut)
{
    return Run(parent, what,
               Lang::Str(2062, L"Choose a password for the archive."),
               passwordOut, /*confirm=*/true);
}

} // namespace PasswordDialog

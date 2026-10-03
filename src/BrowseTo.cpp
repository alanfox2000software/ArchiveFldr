// BrowseTo.cpp — see BrowseTo.h
//
// ─────────────────────────────────────────────────────────────────────────
// Two ways to get this wrong, both of which look identical to the user:
// a double-click that does nothing.
//
// 1. SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0) is the "show in folder"
//    idiom, not the "open it" one. Its documentation is explicit: "If cidl
//    is zero, then pidlFolder must point to a fully specified ITEMIDLIST
//    describing a single item to select. This function opens the parent
//    folder and selects that item." Handed an archive that is meant to open
//    AS a folder, it opens the directory the archive sits in.
//
// 2. ShellExecuteEx is asynchronous. From the SEE_MASK_NOASYNC
//    documentation: "applications that exit immediately after calling
//    ShellExecuteEx should specify this flag." A one-shot program that
//    opens a window and returns is exactly that application. Without the
//    flag the call reports success, the process exits, and the queued
//    operation dies with it — nothing opens, and nothing says why.
// ─────────────────────────────────────────────────────────────────────────
#include "stdafx.h"
#include "BrowseTo.h"

bool ShellBrowseToFolder(HWND hwnd, PCIDLIST_ABSOLUTE pidlAbs)
{
    if (!pidlAbs) return false;

    // How the shell opens every other folder: the Folder class's own open
    // verb, which runs %SystemRoot%\Explorer.exe /idlist,%I,%L. Going
    // through it means the user's "open each folder in the same window"
    // preference applies here the way it does for a zip, instead of this
    // code deciding for them.
    //
    // SEE_MASK_CLASSNAME is what makes it safe. Resolve the verb through
    // the item's own file association instead and an archive whose default
    // app is ArchiveFldr would launch this program, which would call this
    // function, for as long as the machine held out.
    {
        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.fMask    = SEE_MASK_IDLIST | SEE_MASK_CLASSNAME | SEE_MASK_NOASYNC;
        sei.hwnd     = hwnd;
        sei.lpIDList = const_cast<void*>(static_cast<const void*>(pidlAbs));
        sei.lpClass  = L"Folder";
        sei.lpVerb   = L"open";
        sei.nShow    = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei) && (INT_PTR)sei.hInstApp > 32)
            return true;
    }

    // A shell with no Folder\shell\open verb to invoke, or a verb that
    // refused. Asking for exactly one child that is the empty ID list means
    // "this folder, with nothing selected" — the same function as above,
    // used the way that opens rather than reveals. It does its work inline,
    // so it survives this process exiting straight afterwards.
    ITEMIDLIST empty{};
    PCUITEMID_CHILD children[1] = { (PCUITEMID_CHILD)&empty };
    if (SUCCEEDED(SHOpenFolderAndSelectItems(pidlAbs, 1, children, 0)))
        return true;

    // Last resort: hand Explorer the parsing name on its command line.
    // /e, treats what follows as a folder to browse, so it does not go back
    // through the file association and cannot re-enter this program.
    wchar_t parsing[MAX_PATH] = {};
    if (SHGetPathFromIDListW(pidlAbs, parsing) && parsing[0])
    {
        std::wstring args = std::wstring(L"/e,\"") + parsing + L"\"";
        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.fMask        = SEE_MASK_NOASYNC;
        sei.hwnd         = hwnd;
        sei.lpVerb       = L"open";
        sei.lpFile       = L"explorer.exe";
        sei.lpParameters = args.c_str();
        sei.nShow        = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei) && (INT_PTR)sei.hInstApp > 32)
            return true;
    }

    return false;
}

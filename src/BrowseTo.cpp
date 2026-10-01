// BrowseTo.cpp — see BrowseTo.h
//
// ─────────────────────────────────────────────────────────────────────────
// Why this is not one line
//
// The call that looks right is
//
//     SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
//
// and it is the wrong one. Its documentation is explicit: "If cidl is zero,
// then pidlFolder must point to a fully specified ITEMIDLIST describing a
// single item to select. This function opens the parent folder and selects
// that item." It is the "show in folder" idiom.
//
// Handed an archive that is supposed to open *as* a folder, it opens the
// directory the archive sits in and highlights the file — a second Explorer
// window that looks, to anyone who just double-clicked, exactly like nothing
// happening.
// ─────────────────────────────────────────────────────────────────────────
#include "stdafx.h"
#include "BrowseTo.h"

bool ShellBrowseToFolder(HWND hwnd, PCIDLIST_ABSOLUTE pidlAbs)
{
    if (!pidlAbs) return false;

    // How the shell itself opens a folder: the Folder class's open verb,
    //     HKCR\Folder\shell\open\command = %SystemRoot%\Explorer.exe /idlist,%I,%L
    // which is the same command the built-in zip folder is opened with, so
    // the window reuse the user configured applies here too.
    //
    // SEE_MASK_CLASSNAME is what makes it safe. Without it the verb is
    // resolved through the item's own file association — and for an archive
    // that association is the program making this call, which would launch
    // itself until something gave out.
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask    = SEE_MASK_IDLIST | SEE_MASK_CLASSNAME;
    sei.hwnd     = hwnd;
    sei.lpIDList = const_cast<void*>(static_cast<const void*>(pidlAbs));
    sei.lpClass  = L"Folder";
    sei.lpVerb   = L"open";
    sei.nShow    = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei))
        return true;

    // Fallback for a shell with no Folder\shell\open verb to invoke. Asking
    // for one child that is the empty ID list means "this folder, nothing
    // selected", which is the documented way to open a folder with
    // SHOpenFolderAndSelectItems rather than reveal it.
    ITEMIDLIST empty{};
    PCUITEMID_CHILD children[1] = { (PCUITEMID_CHILD)&empty };
    return SUCCEEDED(SHOpenFolderAndSelectItems(pidlAbs, 1, children, 0));
}

// ArchiveFldrOpen.exe — the "open" verb for every ArchiveFldr type.
//
// Why this exists, given that the namespace extension does the opening
// and a helper program was explicitly not wanted in that path:
//
// Windows identifies an entry in "Open with" / "pick a default app" by
// the base name of the executable in its open command, and merges any
// handlers that share one. Our command was
//
//     %windir%\Explorer.exe /idlist,%I,%L
//
// which is byte for byte what the built-in CompressedFolder handler
// uses. So ArchiveFldr was enumerated out of OpenWithProgids, resolved
// to Explorer.exe, found to be "the same application" as the handler
// already in the list, and folded into the single "File Explorer" row.
// It could never appear under its own name, and no amount of
// FriendlyAppName, DefaultIcon or Application metadata could change
// that: the row those values would have decorated did not exist.
//
// An executable name of our own is the only thing that separates the
// two. That is all this program is for.
//
// It opens nothing itself and contains no archive code. It turns the
// path it is handed into a pidl and calls ShellBrowseToFolder — the
// same function behind the context menu's "Open with ArchiveFldr", so
// a double-click and that menu item now go down one code path instead
// of two. The namespace extension still does every bit of the work,
// and there is no window, no prompt and no elevation between a
// double-click and the archive.
//
// Sharing that function rather than writing another one is the whole
// lesson of this file's history. BrowseTo.cpp already knew that
// ShellExecuteEx is asynchronous and that a program which exits
// immediately must pass SEE_MASK_NOASYNC or the queued operation dies
// with the process — which is precisely why the first version of this
// file opened nothing at all.

#include "stdafx.h"
#include "BrowseTo.h"

static void BrowseArchive(const wchar_t* path)
{
    if (!path || !*path) return;

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path, nullptr, &pidl, 0, nullptr)) || !pidl)
        return;

    // No owner window: this program has none, and ShellBrowseToFolder
    // only uses it to parent whatever error UI the shell decides to
    // show. nullptr keeps a failure quiet, which is the one thing that
    // must not change — a dialog in front of a double-click is worse
    // than no window at all.
    ShellBrowseToFolder(nullptr, pidl);

    CoTaskMemFree(pidl);
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // argv[0] is this program. The open verb hands over one archive at
    // a time, but a multi-select or a drop can arrive as several.
    for (int i = 1; i < argc; ++i)
        BrowseArchive(argv[i]);

    CoUninitialize();
    LocalFree(argv);
    return 0;
}

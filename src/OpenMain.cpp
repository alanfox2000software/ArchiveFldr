// ArchiveFldrOpen.exe — the "open" verb for every ArchiveFldr type.
//
// Why this exists, given that the namespace extension does the opening
// and a helper program was explicitly not wanted in that path:
//
// Windows identifies an entry in "Open with" / "pick a default app" by
// the *base name of the executable* in its open command, and merges any
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
// path it is handed into a pidl and gives it straight back to the
// shell's Folder class — the very code path the old registry command
// reached directly — so the namespace extension still does every bit of
// the work, and there is no window, no prompt and no elevation between
// a double-click and the archive.

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

static void BrowseArchive(const wchar_t* path)
{
    if (!path || !*path) return;

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path, nullptr, &pidl, 0, nullptr)) || !pidl)
        return;

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask    = SEE_MASK_IDLIST | SEE_MASK_CLASSNAME | SEE_MASK_FLAG_NO_UI;
    sei.lpIDList = pidl;

    // Name the Folder class explicitly. Left to itself the shell would
    // look the extension up, find this program again, and the two would
    // bounce off each other forever. Asking for Folder's open verb is
    // what the registry command used to say out loud.
    sei.lpClass  = L"Folder";
    sei.lpVerb   = L"open";
    sei.nShow    = SW_SHOWNORMAL;

    // SEE_MASK_FLAG_NO_UI: a failure here must stay silent. A message
    // box in front of a double-click is the one thing this must never
    // do.
    ShellExecuteExW(&sei);

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

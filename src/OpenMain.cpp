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
#include <stdio.h>

static void Trace(const wchar_t* what, HRESULT hr)
{
    // Silent by design, but not invisible. If an archive ever fails to
    // open again, DebugView will say which of the three attempts was
    // reached and what it returned, instead of leaving "nothing
    // happens" as the only evidence.
    wchar_t msg[256] = {};
    swprintf_s(msg, L"[ArchiveFldrOpen] %s -> 0x%08X\n", what, (unsigned)hr);
    OutputDebugStringW(msg);
}

// Ask Explorer to browse INTO the archive.
//
// One call, and its result is deliberately not acted on.
//
// SHOpenFolderAndSelectItems opens the window first and selects
// afterwards, and the empty child pidl below names an item that does
// not exist, so the selection fails and a failure HRESULT comes back
// from a call that has already done its job. Every published use of
// this trick ignores the return value, and the first version of this
// file found out why the hard way: it treated the failure as "did
// nothing", fell through to two more attempts, and each of those
// opened another window.
//
// So there is no cascade here any more. A second mechanism cannot be
// tried after this one without risking a second window, because there
// is no way to ask whether the first succeeded.
static void BrowseTo(PCIDLIST_ABSOLUTE pidl)
{
    // The empty child pidl is the known way to say "this folder, with
    // nothing selected inside it". The obvious spelling, cidl = 0, does
    // the opposite -- Microsoft documents it as opening the PARENT and
    // selecting the item in it.
    ITEMIDLIST idNull = {};
    PCUITEMID_CHILD pidlNull[1] = { static_cast<PCUITEMID_CHILD>(&idNull) };

    const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 1, pidlNull, 0);
    Trace(L"SHOpenFolderAndSelectItems", hr);   // reported, not obeyed
}

static void BrowseArchive(const wchar_t* path)
{
    if (!path || !*path) return;

    PIDLIST_ABSOLUTE pidl = nullptr;
    const HRESULT hr = SHParseDisplayName(path, nullptr, &pidl, 0, nullptr);
    if (FAILED(hr) || !pidl) { Trace(L"SHParseDisplayName", hr); return; }

    BrowseTo(pidl);
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

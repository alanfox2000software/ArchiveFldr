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
// Three ways, because the obvious one is wrong and the next one is not
// guaranteed. Getting this right matters more than it looks: every one
// of them fails silently, so a bad choice here is a double-click that
// does nothing at all, with no error to go on.
static bool BrowseTo(PCIDLIST_ABSOLUTE pidl)
{
    // 1. Open the item AS a folder. The empty child pidl is the known
    //    way to say "this folder, nothing selected inside it" --
    //    passing cidl = 0 instead would open the PARENT and select the
    //    archive in it, which is the documented behaviour and the
    //    opposite of what is wanted.
    ITEMIDLIST idNull = {};
    PCUITEMID_CHILD pidlNull[1] = { static_cast<PCUITEMID_CHILD>(&idNull) };
    HRESULT hr = SHOpenFolderAndSelectItems(pidl, 1, pidlNull, 0);
    Trace(L"SHOpenFolderAndSelectItems", hr);
    if (SUCCEEDED(hr)) return true;

    // 2. The "explore" verb. Deliberately not "open": open is the verb
    //    that brought us here, so asking for it again is how a handler
    //    invites the shell to call it back forever. Nothing registers
    //    an explore verb on our ProgIDs, so this resolves to the Folder
    //    class and stops there.
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask    = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI;
    sei.lpIDList = const_cast<PIDLIST_ABSOLUTE>(pidl);
    sei.lpVerb   = L"explore";
    sei.nShow    = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) { Trace(L"explore verb", S_OK); return true; }
    Trace(L"explore verb failed, GetLastError",
          HRESULT_FROM_WIN32(GetLastError()));

    // 3. The Folder class's open verb, named outright. This was the
    //    only attempt the first version made, and on its own it did
    //    nothing -- hence the two above it.
    sei.fMask  |= SEE_MASK_CLASSNAME;
    sei.lpClass = L"Folder";
    sei.lpVerb  = L"open";
    if (ShellExecuteExW(&sei)) { Trace(L"Folder open verb", S_OK); return true; }
    Trace(L"Folder open verb failed, GetLastError",
          HRESULT_FROM_WIN32(GetLastError()));
    return false;
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

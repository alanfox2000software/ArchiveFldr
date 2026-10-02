// Registry.cpp
// Fixed registration so ContextMenu (and other shellex handlers) work on Windows 11
// when the extension default ProgID is ArchiveFldr.* (Explorer reads shellex from ProgID,
// not from .zip alone). Also registers under SystemFileAssociations.

#include "stdafx.h"
#include "Registry.h"
#include "Formats.h"
#include "Settings.h"
#include "SysInfo.h"
#include "GUIDs.h"

// ShellEx handler category GUIDs
static constexpr wchar_t kIThumbnailProvider[] =
    L"{E357FCCD-A995-4576-B01F-234630154E96}";
static constexpr wchar_t kIPreviewHandler[] =
    L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";

// CATID_BrowsableShellExt — a namespace extension must advertise this
// category before Explorer will browse into it from a rooted view
// (explorer.exe /e,::{CLSID},<object>) or from a file junction.
static constexpr wchar_t kCatidBrowsableShellExt[] =
    L"{00021490-0000-0000-C000-000000000046}";

static constexpr wchar_t kRegKeyPreviewHandlers[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers";

// Private value used to remember whoever owned a file-as-folder junction
// before we took it over (e.g. Windows 11's built-in ArchiveFolder), so that
// DllUnregisterServer can hand it back instead of leaving the type broken.
static constexpr wchar_t kBackupValueName[] = L"ArchiveFldr.PreviousCLSID";

// ── Low-level helpers ─────────────────────────────────────
std::wstring CRegistry::ClsidToStr(const CLSID& clsid)
{
    wchar_t buf[64] = {};
    StringFromGUID2(clsid, buf, 64);
    return buf;
}

HRESULT CRegistry::SetRegStr(HKEY root, const wchar_t* path,
                              const wchar_t* name, const wchar_t* value)
{
    HKEY hk = nullptr;
    LONG rc = RegCreateKeyExW(root, path, 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_WRITE,
                              nullptr, &hk, nullptr);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);

    rc = RegSetValueExW(
        hk, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value),
        static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    return HRESULT_FROM_WIN32(rc);
}

// ArchiveFldr now carries its own icon (res\\resource.ico, resource id 1),
// so DefaultIcon points into this DLL. ",0" means "the first icon in the
// module", which is that one. Falls back to the stock compressed-folder
// icon only if the DLL path is somehow unusable: an index with nothing
// behind it does not degrade gracefully — the shell paints its empty
// placeholder, which is how a blank page once ended up badged onto
// archives.
static std::wstring OwnIcon(const std::wstring& dllPath)
{
    if (dllPath.empty() || !PathFileExistsW(dllPath.c_str())) return L"";
    return dllPath + L",0";
}

static std::wstring SystemIcon(const wchar_t* dllName, int index)
{
    wchar_t sys[MAX_PATH] = {};
    if (!GetSystemDirectoryW(sys, ARRAYSIZE(sys))) return L"";
    std::wstring path = sys;
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    path += dllName;
    if (!PathFileExistsW(path.c_str())) return L"";
    return path + L"," + std::to_wstring(index);
}

// Read a string value; empty when absent.
static std::wstring ReadRegStr(HKEY root, const wchar_t* path,
                               const wchar_t* name)
{
    HKEY hk = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[512] = {};
    DWORD cb = sizeof(buf), type = 0;
    LONG rc = RegQueryValueExW(hk, name, nullptr, &type,
                               reinterpret_cast<BYTE*>(buf), &cb);
    RegCloseKey(hk);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return L"";
    return buf;
}

// Read a DWORD, honouring both registry views so a 32-bit regsvr32 and a
// 64-bit one see the same opt-in flag.
static DWORD ReadRegDword(HKEY root, const wchar_t* path,
                          const wchar_t* name, DWORD fallback)
{
    for (REGSAM view : { (REGSAM)KEY_WOW64_64KEY, (REGSAM)KEY_WOW64_32KEY })
    {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | view, &hk)
                != ERROR_SUCCESS)
            continue;
        DWORD value = 0, cb = sizeof(value), type = 0;
        LONG rc = RegQueryValueExW(hk, name, nullptr, &type,
                                   reinterpret_cast<BYTE*>(&value), &cb);
        RegCloseKey(hk);
        if (rc == ERROR_SUCCESS && type == REG_DWORD) return value;
    }
    return fallback;
}

HRESULT CRegistry::SetRegDword(HKEY root, const wchar_t* path,
                                const wchar_t* name, DWORD value)
{
    HKEY hk = nullptr;
    LONG rc = RegCreateKeyExW(root, path, 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_WRITE,
                              nullptr, &hk, nullptr);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);

    rc = RegSetValueExW(hk, name, 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(hk);
    return HRESULT_FROM_WIN32(rc);
}

// Remove one value, leaving the key and its siblings alone. Used where
// a value written by an earlier version has to go away without taking
// the key it lives on with it.
static HRESULT DelRegValue(HKEY root, const wchar_t* path,
                           const wchar_t* name)
{
    HKEY hk = nullptr;
    LONG rc = RegOpenKeyExW(root, path, 0, KEY_SET_VALUE, &hk);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) return S_OK;
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);

    rc = RegDeleteValueW(hk, name);
    RegCloseKey(hk);
    if (rc == ERROR_FILE_NOT_FOUND) return S_OK;
    return HRESULT_FROM_WIN32(rc);
}

HRESULT CRegistry::DelRegKey(HKEY root, const wchar_t* path)
{
    // Late bound: RegDeleteTreeW is Vista+, and a static import of it
    // would stop the whole DLL loading on XP.
    LONG rc = SysInfo::DeleteRegTree(root, path);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND)
        return S_OK;
    return HRESULT_FROM_WIN32(rc);
}

// Point a "file as folder" junction key (its default value holds a CLSID) at
// our namespace extension, remembering any previous owner.
HRESULT CRegistry::TakeOverJunction(const std::wstring& keyPath,
                                     const std::wstring& ourClsid)
{
    HKEY hk = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                              nullptr, &hk, nullptr);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);

    wchar_t cur[64] = {};
    DWORD cb = sizeof(cur), type = 0;
    if (RegQueryValueExW(hk, nullptr, nullptr, &type,
                         reinterpret_cast<LPBYTE>(cur), &cb) == ERROR_SUCCESS &&
        type == REG_SZ && cur[0] && _wcsicmp(cur, ourClsid.c_str()) != 0)
    {
        // Only record the first (i.e. the genuine, non-ArchiveFldr) owner.
        DWORD probe = 0, ptype = 0;
        if (RegQueryValueExW(hk, kBackupValueName, nullptr, &ptype,
                             nullptr, &probe) != ERROR_SUCCESS)
        {
            RegSetValueExW(hk, kBackupValueName, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(cur),
                           (DWORD)((wcslen(cur) + 1) * sizeof(wchar_t)));
        }
    }

    rc = RegSetValueExW(hk, nullptr, 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(ourClsid.c_str()),
                        (DWORD)((ourClsid.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    return HRESULT_FROM_WIN32(rc);
}

void CRegistry::ReleaseJunction(const std::wstring& keyPath,
                                 const std::wstring& ourClsid)
{
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0,
                      KEY_READ | KEY_WRITE, &hk) != ERROR_SUCCESS)
        return;

    wchar_t cur[64] = {};
    DWORD cb = sizeof(cur), type = 0;
    bool ours = RegQueryValueExW(hk, nullptr, nullptr, &type,
                                 reinterpret_cast<LPBYTE>(cur), &cb) == ERROR_SUCCESS &&
                type == REG_SZ && _wcsicmp(cur, ourClsid.c_str()) == 0;

    wchar_t prev[64] = {};
    DWORD pcb = sizeof(prev), ptype = 0;
    bool hadPrev = RegQueryValueExW(hk, kBackupValueName, nullptr, &ptype,
                                    reinterpret_cast<LPBYTE>(prev), &pcb) == ERROR_SUCCESS &&
                   ptype == REG_SZ && prev[0];

    if (ours && hadPrev)
    {
        RegSetValueExW(hk, nullptr, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(prev),
                       (DWORD)((wcslen(prev) + 1) * sizeof(wchar_t)));
        RegDeleteValueW(hk, kBackupValueName);
        RegCloseKey(hk);
        return;
    }

    RegDeleteValueW(hk, kBackupValueName);
    RegCloseKey(hk);

    // Nothing owned this junction before us — remove the key we created.
    if (ours)
        DelRegKey(HKEY_LOCAL_MACHINE, keyPath.c_str());
}

// ─────────────────────────────────────────────────────────
// Namespace-extension (folder object) registration
//
// Registering the DLL as an in-proc server is NOT enough for Explorer to
// browse a .7z as if it were a folder. The folder CLSID additionally needs:
//
//   ShellFolder\Attributes            the SFGAO_* flags of the junction —
//                                     without SFGAO_FOLDER the shell never
//                                     treats the archive as browsable
//   ShellFolder\WantsFORPARSING       ask us for a parsing name (address bar)
//   Implemented Categories\{00021490} CATID_BrowsableShellExt
//
// Missing these is why "Open with ArchiveFldr" opened nothing.
// ─────────────────────────────────────────────────────────
HRESULT CRegistry::RegisterNamespaceFolder(const wchar_t* dllPath)
{
    const std::wstring sid  = ClsidToStr(CLSID_ArchiveFldrFolder);
    const std::wstring base = std::wstring(L"Software\\Classes\\CLSID\\") + sid;

    // Our own icon, falling back to the stock compressed-folder icon that
    // every Windows install has. If neither resolves, write nothing: no
    // value at all makes the shell fall back to the generic folder icon,
    // which is still a real icon.
    {
        std::wstring icon = OwnIcon(dllPath);
        if (icon.empty()) icon = SystemIcon(L"zipfldr.dll", 0);
        if (!icon.empty())
            RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
                (base + L"\\DefaultIcon").c_str(), nullptr, icon.c_str()));
        else
            DelRegKey(HKEY_LOCAL_MACHINE, (base + L"\\DefaultIcon").c_str());
    }

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\Implemented Categories\\" + kCatidBrowsableShellExt).c_str(),
        nullptr, L""));

    // The value Windows ships for CompressedFolder, the .zip junction this
    // one is modelled on: 0x200001A0.
    //
    //   0x20000000  SFGAO_FOLDER      — the file browses as a folder
    //   0x00000100  SFGAO_DROPTARGET  — things can be dropped on it
    //   0x00000020  SFGAO_CANDELETE
    //   0x00000080  undocumented, and shipped anyway
    //
    // Guessing a set was the mistake here. Earlier builds added
    // SFGAO_HASSUBFOLDER and SFGAO_BROWSABLE, neither of which zipfldr
    // sets: the first hangs an expand arrow off every archive in the
    // navigation pane, and the second describes a root that can be hosted
    // in a browser frame, which a file junction is not.
    const DWORD kFolderAttributes = 0x200001A0;

    RETURN_IF_FAILED(SetRegDword(HKEY_LOCAL_MACHINE,
        (base + L"\\ShellFolder").c_str(), L"Attributes", kFolderAttributes));

    // Send drops to the DropHandler registered on the file type rather than
    // to the folder object. Also copied from CompressedFolder.
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\ShellFolder").c_str(), L"UseDropHandler", L""));

    // WantsFORPARSING is gone with the rest of the guesswork: the parsing
    // name of an archive is its path, which is what the shell uses anyway
    // when no one asks to answer for it. zipfldr does not set it either.
    {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (base + L"\\ShellFolder").c_str(),
                          0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS)
        {
            RegDeleteValueW(hk, L"WantsFORPARSING");
            RegCloseKey(hk);
        }
    }

    return S_OK;
}

// Register ContextMenu / Drop / Thumbnail / Preview under a
// Software\Classes\... base path (extension, ProgID, or SystemFileAssociations).
// Where the compress commands are offered: every file, every folder.
// "Directory" covers folders themselves; "Directory\\Background" is
// deliberately absent, since right-clicking empty space selects nothing
// to compress.
static const wchar_t* const kAllFilesBases[] = { L"*", L"Directory" };

HRESULT CRegistry::RegisterShellExOnBase(const std::wstring& base)
{
    const std::wstring ctx  = ClsidToStr(CLSID_ArchiveFldrContextMenu);
    const std::wstring drop = ClsidToStr(CLSID_ArchiveFldrDropTarget);
    const std::wstring th   = ClsidToStr(CLSID_ArchiveFldrThumbnail);
    const std::wstring pv   = ClsidToStr(CLSID_ArchiveFldrPreview);

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\ContextMenuHandlers\\ArchiveFldr").c_str(),
        nullptr, ctx.c_str()));

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\DropHandler").c_str(),
        nullptr, drop.c_str()));

    // Thumbnail providers and preview handlers are Vista-era shell
    // features. On XP nothing reads these keys and the DLL does not even
    // build the handlers, so leave the registry clean instead.
    if (SysInfo::IsVistaOrLater())
    {
        RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
            (base + L"\\shellex\\" + kIThumbnailProvider).c_str(),
            nullptr, th.c_str()));

        RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
            (base + L"\\shellex\\" + kIPreviewHandler).c_str(),
            nullptr, pv.c_str()));
    }

    // No property-sheet handler: there is no "Archive" tab any more.
    // Delete the key so re-registering an older install drops the tab.
    DelRegKey(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\PropertySheetHandlers\\ArchiveFldr").c_str());

    return S_OK;
}

// ─────────────────────────────────────────────────────────
// RegisterContextMenuOnBase — the compress commands, everywhere
//
// Registered on "*" (every file) and "Directory" (every folder) so that
// "Add to Archive..." is reachable from any selection, the way every
// other archiver on Windows behaves. Deliberately only the context menu:
// a drop handler on every file would make ArchiveFldr the drop target
// for the entire shell, and a thumbnail or preview handler on "*" would
// claim files it has nothing to say about.
// ─────────────────────────────────────────────────────────
HRESULT CRegistry::RegisterContextMenuOnBase(const std::wstring& base)
{
    return SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\ContextMenuHandlers\\ArchiveFldr").c_str(),
        nullptr, ClsidToStr(CLSID_ArchiveFldrContextMenu).c_str());
}

void CRegistry::UnregisterContextMenuOnBase(const std::wstring& base)
{
    DelRegKey(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\ContextMenuHandlers\\ArchiveFldr").c_str());
}

void CRegistry::UnregisterShellExOnBase(const std::wstring& base)
{
    DelRegKey(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\ContextMenuHandlers\\ArchiveFldr").c_str());
    DelRegKey(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\DropHandler").c_str());
    DelRegKey(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\" + kIThumbnailProvider).c_str());
    DelRegKey(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\" + kIPreviewHandler).c_str());
    DelRegKey(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\PropertySheetHandlers\\ArchiveFldr").c_str());
}

// ── COM Server registration ───────────────────────────────
HRESULT CRegistry::RegisterCOMServer(const CLSID& clsid,
                                      const wchar_t* name,
                                      const wchar_t* dllPath,
                                      const wchar_t* threadModel)
{
    std::wstring sid  = ClsidToStr(clsid);
    std::wstring base = std::wstring(L"Software\\Classes\\CLSID\\") + sid;

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, base.c_str(), nullptr, name));
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\InProcServer32").c_str(), nullptr, dllPath));
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\InProcServer32").c_str(), L"ThreadingModel", threadModel));
    return S_OK;
}

HRESULT CRegistry::UnregisterCOMServer(const CLSID& clsid)
{
    std::wstring sid  = ClsidToStr(clsid);
    std::wstring base = std::wstring(L"Software\\Classes\\CLSID\\") + sid;
    return DelRegKey(HKEY_LOCAL_MACHINE, base.c_str());
}


// ─────────────────────────────────────────────────────────
// Being choosable in "Default apps"
// ─────────────────────────────────────────────────────────
//
// Windows does not build its app picker out of RegisteredApplications.
// That key only decides whether ArchiveFldr gets a page of its own in
// Settings > Default apps and which file types that page lists. The list
// of apps offered *for one extension* comes from the shell's association
// handlers, and a handler has to be reachable from the extension key:
//
//     HKCR\.7z\OpenWithProgids\ArchiveFldr.7zFile = ""
//
// Without that value our ProgID is never a candidate, so the page could
// list .7z and the picker behind it still had nothing of ours to offer.
// The value is a list, not an owner: adding ourselves there takes the type
// from nobody and is exactly how an installer asks to be choosable.
static constexpr wchar_t kFriendlyAppName[] = L"ArchiveFldr";

// The verb itself stays with Explorer. ArchiveFldr opens archives through
// its namespace extension — the DLL — exactly the way the built-in zip
// folder does, so the open verb is the same one Windows registers for
// CompressedFolder and the browsing is done by the shell, in the window
// the user is already looking at. An executable in the middle of that is
// a second process that can only get in the way.
//
// Windows registers it like this (HKCR\CompressedFolder\shell\Open):
//
//     MultiSelectModel = Document
//     Command\(Default)       = %SystemRoot%\Explorer.exe /idlist,%I,%L
//     Command\DelegateExecute = {11dbb47c-a525-400b-9e80-a54615a090c0}
//
// That GUID is the shell's own "open this folder" IExecuteCommand handler,
// and on Windows 10 and 11 it is what actually runs; the command line is
// the fallback for callers that cannot use a delegate. Registering only
// the command line — which is what ArchiveFldr did — leaves the open verb
// taking a different code path from every real folder on the machine.
static constexpr wchar_t kFolderOpenDelegate[] =
    L"{11dbb47c-a525-400b-9e80-a54615a090c0}";

static std::wstring ExplorerOpenCommand()
{
    wchar_t win[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(win, ARRAYSIZE(win)))
        wcscpy_s(win, L"C:\\Windows");
    return std::wstring(win) + L"\\Explorer.exe /idlist,%I,%L";
}

// ArchiveFldrOpen.exe, which ships beside the DLL.
//
// Windows identifies a candidate in "Open with" and "pick a default
// app" by the base name of the executable in its open command, and
// merges handlers that resolve to the same one. Pointing at Explorer
// gave us the same name as the built-in CompressedFolder handler, so
// every ArchiveFldr type was folded into the single "File Explorer"
// row and the application never appeared in the picker under its own
// name — the reason the entry was missing and the icon was Explorer's.
//
// The helper opens nothing; it hands the pidl back to the Folder class
// and lets the namespace extension do the work. See src/OpenMain.cpp.
static constexpr wchar_t kOpenHelperExe[] = L"ArchiveFldrOpen.exe";

// Which copy of it to name, now that each platform builds into its own
// folder (<Config>\x64 and <Config>\x32).
//
// There is only one place to record the answer — the open command lives
// under HKLM\Software\Classes, which is shared between the two registry
// views — but Install registers both DLLs in turn, and if each simply
// named the helper beside itself then the surviving value would be
// whichever pass happened to run last.
//
// So on 64-bit Windows the x64 build is preferred outright: both passes
// then agree, and opening an archive does not start a WOW64 process.
// Beside the DLL comes next, which is what a single-folder deployment
// and a 32-bit-only machine both want.
static std::wstring OpenHelperPath(const wchar_t* dllPath)
{
    if (!dllPath || !*dllPath) return L"";

    std::wstring dir = dllPath;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    dir.erase(slash + 1);                       // ...\bin\Release\x64\

    std::wstring parent = dir;                  // ...\bin\Release\
    parent.pop_back();
    slash = parent.find_last_of(L"\\/");
    parent = (slash == std::wstring::npos) ? std::wstring()
                                           : parent.substr(0, slash + 1);

    std::vector<std::wstring> tries;
    if (!parent.empty() && SysInfo::Is64BitWindows())
        tries.push_back(parent + L"x64\\" + kOpenHelperExe);
    tries.push_back(dir + kOpenHelperExe);
    if (!parent.empty())
        tries.push_back(parent + L"x32\\" + kOpenHelperExe);

    // Only claim one that is really there. A command pointing at a
    // program that was never built would break opening altogether,
    // which is far worse than being missing from a list.
    for (const auto& t : tries)
        if (PathFileExistsW(t.c_str())) return t;
    return L"";
}

// The type description Explorer shows in its Type column and Settings
// shows next to the extension. Every ProgID used to say "Archive File",
// which told the user nothing about which of the 21 types they were
// looking at.
static std::wstring TypeNameFor(const wchar_t* ext)
{
    const Formats::Format* f = Formats::Find(ext);
    if (!f || !f->name || !*f->name) return L"Archive";

    std::wstring name = f->name, low = f->name;
    for (auto& ch : low) ch = (wchar_t)towlower(ch);

    // "Java archive", "ISO image", "Android pack" already read as a type.
    if (low.find(L"archive") != std::wstring::npos ||
        low.find(L"image")   != std::wstring::npos ||
        low.find(L"pack")    != std::wstring::npos)
        return name;
    return name + L" archive";
}

// Earlier builds pointed the open verb at the settings program and gave it
// an application registration so Windows' app picker had a name to show.
// The verb belongs to Explorer again, so the key is dead weight: remove it
// on registration as well as on unregistration, or an install that was
// upgraded keeps advertising a program that no longer opens anything.
void CRegistry::UnregisterOpenWithApp()
{
    const wchar_t* names[] = { L"ArchiveFldrSetting.64.exe",
                               L"ArchiveFldrSetting.32.exe",
                               L"ArchiveFldrSetting.exe" };
    for (const wchar_t* n : names)
        DelRegKey(HKEY_LOCAL_MACHINE,
                  (std::wstring(L"Software\\Classes\\Applications\\") + n).c_str());
}

// Add or remove one ProgID in one extension's picker list. Driven by the
// Formats page: a type the user unticked should stop being offered, and a
// type they ticked should start, without either touching the current
// owner of the file type.
static void OfferProgIdFor(const wchar_t* ext, const wchar_t* progId,
                           bool offer)
{
    if (!ext || !progId || !*progId) return;

    const std::wstring key = std::wstring(L"Software\\Classes\\") + ext +
                             L"\\OpenWithProgids";
    HKEY hk = nullptr;
    if (offer)
    {
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hk, nullptr)
                == ERROR_SUCCESS)
        {
            // An empty REG_SZ: the value name is the whole message.
            RegSetValueExW(hk, progId, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(L""),
                           sizeof(wchar_t));
            RegCloseKey(hk);
        }
        return;
    }

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_SET_VALUE, &hk)
            == ERROR_SUCCESS)
    {
        RegDeleteValueW(hk, progId);
        RegCloseKey(hk);
    }
}

// Is this extension ticked on the Formats page?
static bool ExtensionIsWanted(const wchar_t* ext)
{
    std::wstring low = ext ? ext : L"";
    for (auto& ch : low) ch = (wchar_t)towlower(ch);

    // The union of both lists, exactly as RegisterCapabilities uses.
    //
    // This used to ask AssociatedHere(), which is this bitness's list
    // alone, and the two then disagreed: the Capabilities key advertised
    // a type on ArchiveFldr's Default apps page while OpenWithProgids --
    // the value the picker behind that page is actually built from --
    // had it removed. The result was a file type listed under ArchiveFldr
    // that offered no ArchiveFldr to choose.
    //
    // A union is also the only answer that survives registering both
    // DLLs in turn. Software\Classes is shared between the 32- and
    // 64-bit views, so both registrations write these same keys; if each
    // honoured only its own list, the second would strip out whatever
    // the first had offered.
    const Settings& cfg = Settings::Get();
    return cfg.assoc64.find(low) != cfg.assoc64.end()
        || cfg.assoc32.find(low) != cfg.assoc32.end();
}

// Registration is best-effort, step by step.
//
// RETURN_IF_FAILED used to guard every write in RegisterExtension and every
// call to it, which meant one refusal stopped everything after it. Several
// of the keys involved belong to TrustedInstaller — Windows 11 ships its
// own handler for .7z, .rar, .tar and .gz — so an elevated regsvr32 can
// still be told no, and when it was, every file type later in the table
// went unregistered without a word. That is why ArchiveFldr appeared in
// the app picker for some extensions and not others.
//
// Each write is attempted on its own now. The first failure is remembered
// and returned at the end, so a genuine problem is still reported, but a
// key we are not allowed to touch costs only that key.
struct FirstFailure
{
    HRESULT hr = S_OK;
    void operator()(HRESULT h) { if (FAILED(h) && SUCCEEDED(hr)) hr = h; }
};

// ── File extension registration ───────────────────────────
HRESULT CRegistry::RegisterExtension(const wchar_t* ext,
                                      const wchar_t* progId,
                                      const wchar_t* dllPath)
{
    FirstFailure keep;
    const std::wstring folder = ClsidToStr(CLSID_ArchiveFldrFolder);

    // ── 1) ProgID ─────────────────────────────────────────
    // When HKCR\.zip\(Default) = ArchiveFldr.ZipFile, Explorer loads shellex
    // from the ProgID — NOT from .zip. This was the missing piece.
    std::wstring progBase = std::wstring(L"Software\\Classes\\") + progId;

    const std::wstring typeName = TypeNameFor(ext);
    std::wstring ownIcon;

    keep(SetRegStr(HKEY_LOCAL_MACHINE, progBase.c_str(),
        nullptr, typeName.c_str()));

    {
        std::wstring icon = OwnIcon(dllPath);
        if (icon.empty()) icon = SystemIcon(L"zipfldr.dll", 0);
        if (!icon.empty())
            keep(SetRegStr(HKEY_LOCAL_MACHINE,
                (progBase + L"\\DefaultIcon").c_str(), nullptr, icon.c_str()));
        else
            DelRegKey(HKEY_LOCAL_MACHINE,
                      (progBase + L"\\DefaultIcon").c_str());
        ownIcon = icon;
    }

    // Which application the picker believes is behind this ProgID.
    //
    // With no Application subkey the shell works that out from the open
    // command — and ours names Explorer.exe, so every row in "pick a
    // default app" borrowed Explorer's icon and publisher. The
    // FriendlyAppName on the verb below already corrected the label, but
    // a label is not an icon: nothing else told Windows these entries
    // belonged to ArchiveFldr rather than to File Explorer.
    //
    // ApplicationCompany earns its place too. The Open With list quietly
    // skips handlers it cannot attribute to a publisher, which is one of
    // the ways an entry goes missing from the picker.
    {
        const std::wstring appKey = progBase + L"\\Application";
        keep(SetRegStr(HKEY_LOCAL_MACHINE, appKey.c_str(),
            L"ApplicationName", kFriendlyAppName));
        keep(SetRegStr(HKEY_LOCAL_MACHINE, appKey.c_str(),
            L"ApplicationCompany", kFriendlyAppName));
        keep(SetRegStr(HKEY_LOCAL_MACHINE, appKey.c_str(),
            L"ApplicationDescription",
            L"Browse archives as folders in File Explorer."));
        if (!ownIcon.empty())
            keep(SetRegStr(HKEY_LOCAL_MACHINE, appKey.c_str(),
                L"ApplicationIcon", ownIcon.c_str()));
    }

    // FriendlyTypeName is a value ON the ProgID key. Earlier builds wrote a
    // subkey of that name, where nothing reads it — which is why every
    // archive type showed as "Archive File" everywhere it was named.
    DelRegKey(HKEY_LOCAL_MACHINE, (progBase + L"\\FriendlyTypeName").c_str());
    keep(SetRegStr(HKEY_LOCAL_MACHINE, progBase.c_str(),
        L"FriendlyTypeName", typeName.c_str()));

    keep(RegisterShellExOnBase(progBase));

    // Make the ProgID a "file as folder" junction: this single value is what
    // makes Explorer hand the archive to our namespace extension instead of
    // treating it as an opaque file. Modelled on CompressedFolder (.zip),
    // which registers HKCR\CompressedFolder\CLSID the same way.
    keep(TakeOverJunction(progBase + L"\\CLSID", folder));

    // The open verb — Explorer's, delegate and all. See kFolderOpenDelegate.
    {
        std::wstring openKey = progBase + L"\\shell\\open";
        keep(SetRegStr(HKEY_LOCAL_MACHINE, openKey.c_str(),
            L"MultiSelectModel", L"Document"));

        // What Windows' app picker calls this entry. Without it the name
        // comes from the version resource of whatever the command line
        // names — Explorer.exe — so the row would read "File Explorer",
        // the one label that cannot be told apart from the handler already
        // in the list.
        keep(SetRegStr(HKEY_LOCAL_MACHINE, openKey.c_str(),
            L"FriendlyAppName", kFriendlyAppName));

        // ...and the picture beside it. With no Icon on the verb the
        // shell falls back to the first icon of the executable the
        // command names, which is Explorer's.
        if (!ownIcon.empty())
            keep(SetRegStr(HKEY_LOCAL_MACHINE, openKey.c_str(),
                L"Icon", ownIcon.c_str()));

        const std::wstring cmdKey = openKey + L"\\command";
        const std::wstring helper = OpenHelperPath(dllPath);
        if (!helper.empty())
        {
            keep(SetRegStr(HKEY_LOCAL_MACHINE, cmdKey.c_str(), nullptr,
                (L"\"" + helper + L"\" \"%1\"").c_str()));

            // No DelegateExecute alongside it. The delegate IS the
            // folder-open handler, and it wins over the command line
            // wherever it is understood — which would put us straight
            // back to being Explorer.
            DelRegValue(HKEY_LOCAL_MACHINE, cmdKey.c_str(),
                L"DelegateExecute");
        }
        else
        {
            // No helper on disk — an older layout, or a build where only
            // the DLL was produced. Fall back to driving Explorer
            // directly: indistinguishable from CompressedFolder in the
            // picker, but archives still open, which matters more.
            keep(SetRegStr(HKEY_LOCAL_MACHINE, cmdKey.c_str(),
                nullptr, ExplorerOpenCommand().c_str()));
            keep(SetRegStr(HKEY_LOCAL_MACHINE, cmdKey.c_str(),
                L"DelegateExecute", kFolderOpenDelegate));
        }
    }

    // (No extra "open with ArchiveFldr" static verb here on purpose: the
    // IContextMenu handler registered above already supplies that command,
    // and a second registry verb would show up as a duplicate menu entry.)

    // ── 2) Extension (.zip) ───────────────────────────────
    std::wstring extBase = std::wstring(L"Software\\Classes\\") + ext;

    // Offer the ProgID as a choice for this extension. This is the value
    // the "choose an app" list is built from — the Capabilities key only
    // decides what the Default apps *page* lists, not what the picker
    // behind it contains, which is why ArchiveFldr could be on that page
    // with .7z under it and still be absent from the list that opens.
    // Adding a value here takes the type from no one.
    OfferProgIdFor(ext, progId, ExtensionIsWanted(ext));


    // Claim the type only when it is unclaimed, or when the claim is
    // already ours. Overwriting a live association — CompressedFolder on
    // .zip, Windows 11's archive handler on .bz2 and .7z — would be taking
    // the file type without being asked, and it does not even work: the
    // user's own choice under HKCU wins regardless. Settings > Default
    // apps is where that swap belongs, which is what RegisterCapabilities
    // sets up.
    {
        const std::wstring owner =
            ReadRegStr(HKEY_LOCAL_MACHINE, extBase.c_str(), nullptr);
        const bool unowned = owner.empty();
        const bool alreadyOurs =
            owner.rfind(L"ArchiveFldr.", 0) == 0 ||
            owner.rfind(L"ShellNSE.",   0) == 0;   // upgrading in place
        if (unowned || alreadyOurs)
            keep(SetRegStr(HKEY_LOCAL_MACHINE, extBase.c_str(),
                nullptr, progId));
    }

    keep(SetRegStr(HKEY_LOCAL_MACHINE, extBase.c_str(),
        L"PerceivedType", L"compressed"));

    // Older ArchiveFldr builds wrote a bogus ".ext\ShellFolder = {CLSID}" key.
    // That is not a junction location the shell has ever read, so it did
    // nothing; drop it instead of leaving confusing leftovers behind.
    DelRegKey(HKEY_LOCAL_MACHINE, (extBase + L"\\ShellFolder").c_str());

    // Also on the extension (harmless; ignored when ProgID owns the type)
    keep(RegisterShellExOnBase(extBase));

    // ── 3) SystemFileAssociations\.ext ────────────────────
    // Used by Explorer even when UserChoice / ProgID differs. The CLSID
    // value here is the junction Windows 11 itself uses for .7z/.rar/.tar
    // (its built-in ArchiveFolder), and it keeps working when another
    // archiver owns the file association.
    std::wstring sfaBase =
        std::wstring(L"Software\\Classes\\SystemFileAssociations\\") + ext;
    keep(RegisterShellExOnBase(sfaBase));
    keep(TakeOverJunction(sfaBase + L"\\CLSID", folder));

    return keep.hr;
}

HRESULT CRegistry::UnregisterExtension(const wchar_t* ext,
                                       const wchar_t* progId)
{
    const std::wstring folder = ClsidToStr(CLSID_ArchiveFldrFolder);

    std::wstring extBase = std::wstring(L"Software\\Classes\\") + ext;
    UnregisterShellExOnBase(extBase);
    DelRegKey(HKEY_LOCAL_MACHINE, (extBase + L"\\ShellFolder").c_str());

    // Stop offering ourselves in this extension's app picker. One value,
    // not the key: everyone else who can open a .zip is listed in there
    // too, and deleting the key would take them all out with us.
    if (progId && *progId)
    {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                (extBase + L"\\OpenWithProgids").c_str(), 0, KEY_SET_VALUE,
                &hk) == ERROR_SUCCESS)
        {
            RegDeleteValueW(hk, progId);
            RegCloseKey(hk);
        }
    }

    std::wstring sfaBase =
        std::wstring(L"Software\\Classes\\SystemFileAssociations\\") + ext;
    UnregisterShellExOnBase(sfaBase);
    // Give the file-as-folder junction back to whoever had it before us
    // (on Windows 11 that is the built-in ArchiveFolder handler).
    ReleaseJunction(sfaBase + L"\\CLSID", folder);

    // Do not delete the entire .ext key (may restore system default later).
    // ProgID tree is removed in UnregisterAll by name.
    return S_OK;
}

// ── Approved Extensions ───────────────────────────────────
HRESULT CRegistry::RegisterApproved(const CLSID& clsid, const wchar_t* name)
{
    return SetRegStr(HKEY_LOCAL_MACHINE, kRegKeyApproved,
        ClsidToStr(clsid).c_str(), name);
}

HRESULT CRegistry::UnregisterApproved(const CLSID& clsid)
{
    HKEY hk = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegKeyApproved, 0, KEY_WRITE, &hk);
    if (rc != ERROR_SUCCESS)
        return S_OK;
    RegDeleteValueW(hk, ClsidToStr(clsid).c_str());
    RegCloseKey(hk);
    return S_OK;
}

// ── Icon Overlay ──────────────────────────────────────────
HRESULT CRegistry::UnregisterOverlay(const CLSID& /*clsid*/, const wchar_t* name)
{
    std::wstring path = std::wstring(kRegKeyOverlays) + L"\\" + name;
    return DelRegKey(HKEY_LOCAL_MACHINE, path.c_str());
}

// ── Default apps / Registered Applications ────────────────
//
// Windows resolves a "file as folder" junction through the ProgID the file
// type currently points at, and that beats SystemFileAssociations. So when
// Windows' own archive handler owns .zip or .bz2, registering our junction
// cannot win and should not try to — forcing it would be taking the file
// type behind the user's back.
//
// The supported way to offer the swap is to publish a Capabilities key and
// list it in RegisteredApplications. ArchiveFldr then appears in
// Settings > Default apps, where the user can point individual extensions
// at it. Once they do, the ProgID is ours and everything else follows:
// double-click browses the archive, and so does our own context menu.
static constexpr wchar_t kCapabilitiesKey[] = L"Software\\ArchiveFldr\\Capabilities";
static constexpr wchar_t kRegisteredApps[]  = L"Software\\RegisteredApplications";
static constexpr wchar_t kAppName[]         = L"ArchiveFldr";

// Capabilities are written through the 64-bit view whatever the bitness
// of the DLL doing the writing.
//
// Software\RegisteredApplications is shared between the two registry
// views rather than redirected, because Default apps has to see
// applications of both bitnesses. The Capabilities key it points at is
// not shared. Registering a 32-bit DLL without this flag therefore
// publishes a pointer in the shared list to a key that only exists in
// WOW6432Node, and Default apps follows the pointer and finds nothing.
// Pinning both to the 64-bit view keeps the pointer and its target in
// the same place. On 32-bit Windows the flag is ignored.
static HRESULT SetCapStr(const wchar_t* path, const wchar_t* name,
                         const wchar_t* value)
{
    HKEY hk = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, nullptr,
                              REG_OPTION_NON_VOLATILE,
                              KEY_WRITE | KEY_WOW64_64KEY,
                              nullptr, &hk, nullptr);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);

    rc = RegSetValueExW(
        hk, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value),
        static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    return HRESULT_FROM_WIN32(rc);
}

// Delete one subkey through the 64-bit view. DeleteRegTree recurses on
// the handle it is given, so the view carries down to the children.
static HRESULT DelCapKey(const wchar_t* parent, const wchar_t* child)
{
    HKEY hk = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, parent, 0,
                            KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &hk);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND)
        return S_OK;
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);

    rc = SysInfo::DeleteRegTree(hk, child);
    RegCloseKey(hk);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND)
        return S_OK;
    return HRESULT_FROM_WIN32(rc);
}

// HKCR\Applications\ArchiveFldrOpen.exe — what the shell reads to find
// out who an executable belongs to.
//
// This is the key the Open With UI consults once it has resolved a
// handler down to a program. It is also, verbatim, why ArchiveFldr used
// to show up as File Explorer with File Explorer's icon: the open
// command named Explorer.exe, so the shell read
// HKCR\Applications\Explorer.exe and faithfully reported what it found
// there. Now that the command names a program of ours, this is where
// the right answer goes.
HRESULT CRegistry::RegisterOpenHelper(const wchar_t* dllPath)
{
    const std::wstring helper = OpenHelperPath(dllPath);
    if (helper.empty()) return S_OK;   // nothing built, nothing to say

    const std::wstring base =
        std::wstring(L"Software\\Classes\\Applications\\") + kOpenHelperExe;

    FirstFailure keep;
    keep(SetRegStr(HKEY_LOCAL_MACHINE, base.c_str(),
        L"FriendlyAppName", kFriendlyAppName));

    const std::wstring icon = OwnIcon(dllPath ? dllPath : L"");
    if (!icon.empty())
        keep(SetRegStr(HKEY_LOCAL_MACHINE, (base + L"\\DefaultIcon").c_str(),
            nullptr, icon.c_str()));

    keep(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\shell\\open\\command").c_str(), nullptr,
        (L"\"" + helper + L"\" \"%1\"").c_str()));

    // What it will admit to handling. Windows uses this to decide
    // whether to suggest the program for a type it has not been asked
    // about yet; without it we are offered for everything, which is
    // how an archive tool ends up suggested for .jpg.
    const std::wstring types = base + L"\\SupportedTypes";
    for (const auto* f : Formats::Registrable())
        keep(SetRegStr(HKEY_LOCAL_MACHINE, types.c_str(), f->ext, L""));

    return keep.hr;
}

HRESULT CRegistry::RegisterCapabilities(const wchar_t* dllPath)
{
    RETURN_IF_FAILED(SetCapStr(kCapabilitiesKey,
        L"ApplicationName", L"ArchiveFldr"));
    RETURN_IF_FAILED(SetCapStr(kCapabilitiesKey,
        L"ApplicationDescription",
        L"Browse archives as folders in File Explorer."));

    {
        std::wstring icon = OwnIcon(dllPath ? dllPath : L"");
        if (icon.empty()) icon = SystemIcon(L"zipfldr.dll", 0);
        if (!icon.empty())
            SetCapStr(kCapabilitiesKey, L"ApplicationIcon", icon.c_str());
    }

    // Only the extensions the user left ticked on the Formats page.
    // Windows reads this key to build the per-type list in Settings >
    // Default apps, so an unticked type simply never appears there.
    const std::wstring assoc = std::wstring(kCapabilitiesKey) + L"\\FileAssociations";

    // The union of both association lists, not just this bitness's.
    // There is one Capabilities key but two lists, and on an x64 machine
    // Install registers both DLLs in turn -- so if each wrote only its
    // own list, the second run would erase what the first had just
    // published. The Default apps entry describes the application, not
    // the DLL that happened to register it.
    const Settings& cfg = Settings::Get();
    std::set<std::wstring> wanted = cfg.assoc64;
    wanted.insert(cfg.assoc32.begin(), cfg.assoc32.end());

    // Clear first: an extension that was ticked last time and is not now
    // has to lose its value, not keep it.
    DelCapKey(kCapabilitiesKey, L"FileAssociations");

    for (const auto* f : Formats::Registrable())
    {
        std::wstring ext = f->ext;
        for (auto& ch : ext) ch = (wchar_t)towlower(ch);
        const bool want = wanted.find(ext) != wanted.end();

        // Keep the picker list in step with the page the user just left,
        // so ticking a format here does not need a re-register to show up.
        OfferProgIdFor(f->ext, f->progId, want);

        if (!want) continue;
        RETURN_IF_FAILED(SetCapStr(assoc.c_str(), f->ext, f->progId));
    }

    // The pointer that makes Windows actually look at the key above.
    RETURN_IF_FAILED(SetCapStr(kRegisteredApps, kAppName, kCapabilitiesKey));
    return S_OK;
}

HRESULT CRegistry::UnregisterCapabilities()
{
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegisteredApps, 0,
                      KEY_SET_VALUE | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS)
    {
        RegDeleteValueW(hk, kAppName);
        RegCloseKey(hk);
    }

    // The Capabilities subkey and nothing else. This used to delete
    // Software\ArchiveFldr outright -- which is the settings root, not a
    // registration artefact -- so withdrawing from Default apps threw
    // away every preference the user had, including the association
    // ticks and the very flag that asked for the withdrawal.
    return DelCapKey(L"Software\\ArchiveFldr", L"Capabilities");
}

// ─────────────────────────────────────────────────────────
// RegisterAll
// ─────────────────────────────────────────────────────────
HRESULT CRegistry::RegisterAll(const wchar_t* dllPath)
{
    // 1. COM servers
    RETURN_IF_FAILED(RegisterCOMServer(CLSID_ArchiveFldrFolder,
        L"ArchiveFldr Shell Namespace Extension", dllPath));
    RETURN_IF_FAILED(RegisterCOMServer(CLSID_ArchiveFldrContextMenu,
        L"ArchiveFldr Context Menu Handler", dllPath));
    RETURN_IF_FAILED(RegisterCOMServer(CLSID_ArchiveFldrDropTarget,
        L"ArchiveFldr Drop Target Handler", dllPath));
    RETURN_IF_FAILED(RegisterCOMServer(CLSID_ArchiveFldrThumbnail,
        L"ArchiveFldr Thumbnail Provider", dllPath));
    RETURN_IF_FAILED(RegisterCOMServer(CLSID_ArchiveFldrPreview,
        L"ArchiveFldr Preview Handler", dllPath));

    // 1b. Namespace-extension specifics for the folder object
    //     (ShellFolder\Attributes, CATID_BrowsableShellExt, icon).
    //     Without this Explorer will not browse into an archive.
    RETURN_IF_FAILED(RegisterNamespaceFolder(dllPath));

    // 2. Approved list (Vista+)
    RETURN_IF_FAILED(RegisterApproved(CLSID_ArchiveFldrFolder,
        L"ArchiveFldr Shell Namespace Extension"));
    RETURN_IF_FAILED(RegisterApproved(CLSID_ArchiveFldrContextMenu,
        L"ArchiveFldr Context Menu Handler"));
    RETURN_IF_FAILED(RegisterApproved(CLSID_ArchiveFldrDropTarget,
        L"ArchiveFldr Drop Target Handler"));
    RETURN_IF_FAILED(RegisterApproved(CLSID_ArchiveFldrThumbnail,
        L"ArchiveFldr Thumbnail Provider"));
    RETURN_IF_FAILED(RegisterApproved(CLSID_ArchiveFldrPreview,
        L"ArchiveFldr Preview Handler"));

    // 3. PreviewHandlers global list (needed for preview pane)
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, kRegKeyPreviewHandlers,
        ClsidToStr(CLSID_ArchiveFldrPreview).c_str(),
        L"ArchiveFldr Archive Preview Handler"));

    // 4. Icon overlay — removed.
    //
    // There is no overlay handler any more, so nothing is registered here.
    // Earlier builds did register one, under ' ArchiveFldr_Archive' with a
    // leading space to sort ahead of other handlers in the 15 slots Windows
    // allows. Clean up both spellings, plus its COM registration, so
    // re-registering an existing install drops the badge.
    UnregisterOverlay(CLSID_ArchiveFldrIconOverlay, L" ArchiveFldr_Archive");
    UnregisterOverlay(CLSID_ArchiveFldrIconOverlay, L"ArchiveFldr_Archive");
    UnregisterApproved(CLSID_ArchiveFldrIconOverlay);
    UnregisterCOMServer(CLSID_ArchiveFldrIconOverlay);

    // 4b. Property sheet — removed.
    //
    // Earlier builds added an "Archive" tab to the file Properties dialog.
    // Nothing registers it now; strip its COM registration as well so the
    // tab disappears from installs that already have it. The per-extension
    // PropertySheetHandlers keys are deleted in RegisterShellExOnBase.
    UnregisterApproved(CLSID_ArchiveFldrPropSheet);
    UnregisterCOMServer(CLSID_ArchiveFldrPropSheet);

    // 4c. Drop the application registration older builds wrote for the
    //     settings program, which is no longer part of opening anything.
    UnregisterOpenWithApp();

    // 5. Extensions — every row in the Formats table that carries a
    //    progId. Registering and unregistering now read the same list, so
    //    they cannot drift apart the way two hand-written copies did.
    //    Office containers deliberately have no progId: .docx really is a
    //    zip, but taking Word's file association is hostile.
    //    One file type that will not take registration must not cost the
    //    rest of them theirs, so the loop records and continues.
    FirstFailure extensions;
    for (const auto* f : Formats::Registrable())
        extensions(RegisterExtension(f->ext, f->progId, dllPath));

    // 6. Offer ourselves in Settings > Default apps. This is the only way
    //    to take a file type that Windows' built-in archive handler owns
    //    (.zip through CompressedFolder, and on Windows 11 .bz2, .gz, .tar
    //    and .7z through its newer one), and it leaves the choice to the
    //    user instead of grabbing the type during registration.
    //    The user controls this from the settings program; registration
    //    only honours the stored answer. Withdrawing is explicit, so a
    //    re-register after unticking really does remove the entry.
    // Give the open helper an identity before anything points at it.
    RETURN_IF_FAILED(RegisterOpenHelper(dllPath));

    if (Settings::Get().registerAsDefaultApp)
        RETURN_IF_FAILED(RegisterCapabilities(dllPath));
    else
        UnregisterCapabilities();

    // 7. The compress commands apply to any file or folder, not just to
    //    the types ArchiveFldr can open, so the context menu handler goes
    //    on "*" and "Directory" as well. Only the menu: see
    //    RegisterContextMenuOnBase for why the other handlers do not.
    for (const wchar_t* base : kAllFilesBases)
        RETURN_IF_FAILED(RegisterContextMenuOnBase(
            std::wstring(L"Software\\Classes\\") + base));

    // Everything that could be registered has been. If a file type refused,
    // report it — but only after the other twenty got their turn.
    return extensions.hr;
}

// ─────────────────────────────────────────────────────────
// UnregisterAll
// ─────────────────────────────────────────────────────────
HRESULT CRegistry::UnregisterAll()
{
    // Stop advertising in Settings > Default apps first, so the entry does
    // not linger pointing at file types we are about to release.
    UnregisterCapabilities();

    // The all-files / all-folders context menu.
    for (const wchar_t* base : kAllFilesBases)
        UnregisterContextMenuOnBase(std::wstring(L"Software\\Classes\\") + base);

    // The application registration older builds wrote for the settings
    // program.
    UnregisterOpenWithApp();

    // Everything we ever registered, plus a few progIds from older builds
    // that are no longer in the table, so an upgrade cleans up after
    // itself rather than leaving orphans behind.
    struct ExtDef { const wchar_t* ext; const wchar_t* progId; };
    std::vector<ExtDef> exts;
    for (const auto* f : Formats::Registrable())
        exts.push_back({ f->ext, f->progId });

    const ExtDef legacy[] = {
        { L".docx", L"ArchiveFldr.DocxFile" },
        { L".xlsx", L"ArchiveFldr.XlsxFile" },
        { L".pptx", L"ArchiveFldr.PptxFile" },
    };
    for (const auto& e : legacy) exts.push_back(e);

    // The open helper's identity key. Nothing else refers to it once
    // the ProgID trees below are gone, and leaving it behind would keep
    // ArchiveFldr in the Open With list with no way to act on it.
    DelRegKey(HKEY_LOCAL_MACHINE,
        (std::wstring(L"Software\\Classes\\Applications\\")
            + kOpenHelperExe).c_str());

    for (const auto& e : exts)
    {
        UnregisterExtension(e.ext, e.progId);

        // Remove entire ProgID tree we created
        std::wstring progBase = std::wstring(L"Software\\Classes\\") + e.progId;
        DelRegKey(HKEY_LOCAL_MACHINE, progBase.c_str());

        // If we still own the extension default, clear it (best-effort).
        // Safer than leaving a dead ProgID name as default.
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                (std::wstring(L"Software\\Classes\\") + e.ext).c_str(),
                0, KEY_READ | KEY_WRITE, &hk) == ERROR_SUCCESS)
        {
            wchar_t cur[256] = {};
            DWORD cb = sizeof(cur);
            DWORD type = 0;
            if (RegQueryValueExW(hk, nullptr, nullptr, &type,
                    reinterpret_cast<LPBYTE>(cur), &cb) == ERROR_SUCCESS &&
                type == REG_SZ &&
                _wcsicmp(cur, e.progId) == 0)
            {
                // Clear default ProgID; user/Windows can restore association.
                RegSetValueExW(hk, nullptr, 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(L""), sizeof(wchar_t));
            }
            RegCloseKey(hk);
        }
    }

    // PreviewHandlers list
    {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegKeyPreviewHandlers,
                0, KEY_WRITE, &hk) == ERROR_SUCCESS)
        {
            RegDeleteValueW(hk, ClsidToStr(CLSID_ArchiveFldrPreview).c_str());
            RegCloseKey(hk);
        }
    }

    UnregisterOverlay(CLSID_ArchiveFldrIconOverlay, L" ArchiveFldr_Archive");
    UnregisterOverlay(CLSID_ArchiveFldrIconOverlay, L"ArchiveFldr_Archive");

    UnregisterApproved(CLSID_ArchiveFldrFolder);
    UnregisterApproved(CLSID_ArchiveFldrContextMenu);
    UnregisterApproved(CLSID_ArchiveFldrIconOverlay);
    UnregisterApproved(CLSID_ArchiveFldrDropTarget);
    UnregisterApproved(CLSID_ArchiveFldrThumbnail);
    UnregisterApproved(CLSID_ArchiveFldrPreview);
    UnregisterApproved(CLSID_ArchiveFldrPropSheet);

    UnregisterCOMServer(CLSID_ArchiveFldrFolder);
    UnregisterCOMServer(CLSID_ArchiveFldrContextMenu);
    UnregisterCOMServer(CLSID_ArchiveFldrIconOverlay);
    UnregisterCOMServer(CLSID_ArchiveFldrDropTarget);
    UnregisterCOMServer(CLSID_ArchiveFldrThumbnail);
    UnregisterCOMServer(CLSID_ArchiveFldrPreview);
    UnregisterCOMServer(CLSID_ArchiveFldrPropSheet);

    return S_OK;
}
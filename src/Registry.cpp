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

// ArchiveFldr ships no icon resources, so every DefaultIcon points at a stock
// Windows icon rather than an index into this DLL. An index with nothing
// behind it does not fall back to anything — the shell paints its empty
// placeholder, which is how a blank page ended up badged onto archives.
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

    // zipfldr.dll,0 is the compressed-folder icon every Windows install
    // has. If it is somehow missing, write nothing: no value at all makes
    // the shell fall back to the generic folder icon, which is still a
    // real icon.
    {
        const std::wstring icon = SystemIcon(L"zipfldr.dll", 0);
        if (!icon.empty())
            RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
                (base + L"\\DefaultIcon").c_str(), nullptr, icon.c_str()));
        else
            DelRegKey(HKEY_LOCAL_MACHINE, (base + L"\\DefaultIcon").c_str());
    }

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\Implemented Categories\\" + kCatidBrowsableShellExt).c_str(),
        nullptr, L""));

    // SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_BROWSABLE | SFGAO_DROPTARGET
    const DWORD kFolderAttributes =
        SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_BROWSABLE | SFGAO_DROPTARGET;

    RETURN_IF_FAILED(SetRegDword(HKEY_LOCAL_MACHINE,
        (base + L"\\ShellFolder").c_str(), L"Attributes", kFolderAttributes));

    // Tells the shell to ask IShellFolder::GetDisplayNameOf(SHGDN_FORPARSING)
    // for this junction's parsing name (used by the address bar / breadcrumb).
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\ShellFolder").c_str(), L"WantsFORPARSING", L""));

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

// A handler also has to be a program. ArchiveFldr's code is in a DLL, and
// a DLL cannot appear in a list of applications, so the open verb points
// at the companion executable that ships beside it — the same one the
// context menu opens for settings, which browses an archive when it is
// handed a path. When it is missing (DLL deployed on its own) the verb
// falls back to Explorer, which still works; it just shows up in the
// picker as "File Explorer", indistinguishable from the built-in handler.
static std::wstring CompanionExe(const wchar_t* dllPath)
{
    if (!dllPath || !*dllPath) return L"";

    wchar_t dir[MAX_PATH] = {};
    wcsncpy_s(dir, dllPath, _TRUNCATE);
    PathRemoveFileSpecW(dir);

#ifdef _WIN64
    const wchar_t* tagged = L"ArchiveFldrSetting.64.exe";
#else
    const wchar_t* tagged = L"ArchiveFldrSetting.32.exe";
#endif
    const wchar_t* candidates[] = { tagged, L"ArchiveFldrSetting.exe" };
    for (const wchar_t* c : candidates)
    {
        std::wstring p = std::wstring(dir) + L"\\" + c;
        if (PathFileExistsW(p.c_str())) return p;
    }
    return L"";
}

static std::wstring OpenCommandFor(const wchar_t* dllPath)
{
    const std::wstring exe = CompanionExe(dllPath);
    if (!exe.empty())
        return L"\"" + exe + L"\" /open \"%1\"";

    wchar_t win[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(win, ARRAYSIZE(win)))
        wcscpy_s(win, L"C:\\Windows");
    return std::wstring(win) + L"\\Explorer.exe /idlist,%I,%L";
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

// HKCR\Applications\<exe> is where the shell looks up the display name and
// icon of a program it is about to offer or has just been told to use.
// Deliberately no SupportedTypes value: that would add a *second*
// ArchiveFldr row to every picker, one for the ProgID and one for the
// executable, and the two behave differently once chosen.
HRESULT CRegistry::RegisterOpenWithApp(const wchar_t* dllPath)
{
    const std::wstring exe = CompanionExe(dllPath);
    if (exe.empty()) return S_FALSE;

    const std::wstring base = std::wstring(L"Software\\Classes\\Applications\\") +
                              PathFindFileNameW(exe.c_str());

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, base.c_str(),
        nullptr, kFriendlyAppName));
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, base.c_str(),
        L"FriendlyAppName", kFriendlyAppName));

    {
        const std::wstring icon = SystemIcon(L"zipfldr.dll", 0);
        if (!icon.empty())
            SetRegStr(HKEY_LOCAL_MACHINE, (base + L"\\DefaultIcon").c_str(),
                      nullptr, icon.c_str());
    }

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\shell\\open").c_str(), L"FriendlyAppName", kFriendlyAppName));
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\shell\\open\\command").c_str(), nullptr,
        OpenCommandFor(dllPath).c_str()));
    return S_OK;
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
    const std::set<std::wstring>& wanted = Settings::Get().associatedExts;
    return wanted.find(low) != wanted.end();
}

void CRegistry::UnregisterOpenWithApp()
{
    const wchar_t* names[] = { L"ArchiveFldrSetting.64.exe",
                               L"ArchiveFldrSetting.32.exe",
                               L"ArchiveFldrSetting.exe" };
    for (const wchar_t* n : names)
        DelRegKey(HKEY_LOCAL_MACHINE,
                  (std::wstring(L"Software\\Classes\\Applications\\") + n).c_str());
}

// ── File extension registration ───────────────────────────
HRESULT CRegistry::RegisterExtension(const wchar_t* ext,
                                      const wchar_t* progId,
                                      const wchar_t* dllPath)
{
    const std::wstring folder = ClsidToStr(CLSID_ArchiveFldrFolder);

    // ── 1) ProgID ─────────────────────────────────────────
    // When HKCR\.zip\(Default) = ArchiveFldr.ZipFile, Explorer loads shellex
    // from the ProgID — NOT from .zip. This was the missing piece.
    std::wstring progBase = std::wstring(L"Software\\Classes\\") + progId;

    const std::wstring typeName = TypeNameFor(ext);

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, progBase.c_str(),
        nullptr, typeName.c_str()));

    {
        const std::wstring icon = SystemIcon(L"zipfldr.dll", 0);
        if (!icon.empty())
            RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
                (progBase + L"\\DefaultIcon").c_str(), nullptr, icon.c_str()));
        else
            DelRegKey(HKEY_LOCAL_MACHINE,
                      (progBase + L"\\DefaultIcon").c_str());
    }

    // FriendlyTypeName is a value ON the ProgID key. Earlier builds wrote a
    // subkey of that name, where nothing reads it — which is why every
    // archive type showed as "Archive File" everywhere it was named.
    DelRegKey(HKEY_LOCAL_MACHINE, (progBase + L"\\FriendlyTypeName").c_str());
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, progBase.c_str(),
        L"FriendlyTypeName", typeName.c_str()));

    RETURN_IF_FAILED(RegisterShellExOnBase(progBase));

    // Make the ProgID a "file as folder" junction: this single value is what
    // makes Explorer hand the archive to our namespace extension instead of
    // treating it as an opaque file. Modelled on CompressedFolder (.zip),
    // which registers HKCR\CompressedFolder\CLSID the same way.
    RETURN_IF_FAILED(TakeOverJunction(progBase + L"\\CLSID", folder));

    // The open verb. Inside Explorer a double-click rarely reaches it —
    // the CLSID junction above makes the file report itself as a folder, so
    // the shell navigates into it instead of invoking a command. The verb is
    // what everything else uses: "Open with", ShellExecute from another
    // program, and the row Windows shows in its app picker.
    {
        std::wstring openKey = progBase + L"\\shell\\open";
        RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, openKey.c_str(),
            L"MultiSelectModel", L"Document"));

        // What the picker calls this entry. Without it the name comes from
        // the version resource of whatever the command line names, so the
        // Explorer fallback would read "File Explorer" — the one label that
        // cannot be told apart from the handler already in the list.
        RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, openKey.c_str(),
            L"FriendlyAppName", kFriendlyAppName));

        RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
            (openKey + L"\\command").c_str(), nullptr,
            OpenCommandFor(dllPath).c_str()));
    }

    // (No extra "open with ArchiveFldr" static verb here on purpose: the
    // IContextMenu handler registered above already supplies that command,
    // and a second registry verb would show up as a duplicate menu entry.)

    // ── 2) Extension (.zip) ───────────────────────────────
    std::wstring extBase = std::wstring(L"Software\\Classes\\") + ext;

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
            RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, extBase.c_str(),
                nullptr, progId));
    }

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, extBase.c_str(),
        L"PerceivedType", L"compressed"));

    // Offer the ProgID as a choice for this extension. This is the value
    // the "choose an app" list is built from — the Capabilities key only
    // decides what the Default apps *page* lists, not what the picker
    // behind it contains, which is why ArchiveFldr could be on that page
    // with .7z under it and still be absent from the list that opens.
    // Adding a value here takes the type from no one.
    OfferProgIdFor(ext, progId, ExtensionIsWanted(ext));

    // Older ArchiveFldr builds wrote a bogus ".ext\ShellFolder = {CLSID}" key.
    // That is not a junction location the shell has ever read, so it did
    // nothing; drop it instead of leaving confusing leftovers behind.
    DelRegKey(HKEY_LOCAL_MACHINE, (extBase + L"\\ShellFolder").c_str());

    // Also on the extension (harmless; ignored when ProgID owns the type)
    RETURN_IF_FAILED(RegisterShellExOnBase(extBase));

    // ── 3) SystemFileAssociations\.ext ────────────────────
    // Used by Explorer even when UserChoice / ProgID differs. The CLSID
    // value here is the junction Windows 11 itself uses for .7z/.rar/.tar
    // (its built-in ArchiveFolder), and it keeps working when another
    // archiver owns the file association.
    std::wstring sfaBase =
        std::wstring(L"Software\\Classes\\SystemFileAssociations\\") + ext;
    RETURN_IF_FAILED(RegisterShellExOnBase(sfaBase));
    RETURN_IF_FAILED(TakeOverJunction(sfaBase + L"\\CLSID", folder));

    return S_OK;
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

HRESULT CRegistry::RegisterCapabilities(const wchar_t* /*dllPath*/)
{
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, kCapabilitiesKey,
        L"ApplicationName", L"ArchiveFldr"));
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, kCapabilitiesKey,
        L"ApplicationDescription",
        L"Browse archives as folders in File Explorer."));

    {
        // No icon of our own ships with this project, so borrow the stock
        // compressed-folder icon rather than pointing at an empty slot.
        const std::wstring icon = SystemIcon(L"zipfldr.dll", 0);
        if (!icon.empty())
            SetRegStr(HKEY_LOCAL_MACHINE, kCapabilitiesKey,
                      L"ApplicationIcon", icon.c_str());
    }

    // Only the extensions the user left ticked on the Formats page.
    // Windows reads this key to build the per-type list in Settings >
    // Default apps, so an unticked type simply never appears there.
    const std::wstring assoc = std::wstring(kCapabilitiesKey) + L"\\FileAssociations";
    const std::set<std::wstring>& wanted = Settings::Get().associatedExts;

    // Clear first: an extension that was ticked last time and is not now
    // has to lose its value, not keep it.
    DelRegKey(HKEY_LOCAL_MACHINE, assoc.c_str());

    for (const auto* f : Formats::Registrable())
    {
        std::wstring ext = f->ext;
        for (auto& ch : ext) ch = (wchar_t)towlower(ch);
        const bool want = wanted.find(ext) != wanted.end();

        // Keep the picker list in step with the page the user just left,
        // so ticking a format here does not need a re-register to show up.
        OfferProgIdFor(f->ext, f->progId, want);

        if (!want) continue;
        RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, assoc.c_str(),
                                   f->ext, f->progId));
    }

    // The pointer that makes Windows actually look at the key above.
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, kRegisteredApps,
        kAppName, kCapabilitiesKey));
    return S_OK;
}

HRESULT CRegistry::UnregisterCapabilities()
{
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegisteredApps, 0, KEY_SET_VALUE,
                      &hk) == ERROR_SUCCESS)
    {
        RegDeleteValueW(hk, kAppName);
        RegCloseKey(hk);
    }
    return DelRegKey(HKEY_LOCAL_MACHINE, L"Software\\ArchiveFldr");
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

    // 4c. The companion program, so the open verb each ProgID gets below
    //     has a name and an icon to show in Windows' app picker.
    RegisterOpenWithApp(dllPath);

    // 5. Extensions — every row in the Formats table that carries a
    //    progId. Registering and unregistering now read the same list, so
    //    they cannot drift apart the way two hand-written copies did.
    //    Office containers deliberately have no progId: .docx really is a
    //    zip, but taking Word's file association is hostile.
    for (const auto* f : Formats::Registrable())
        RETURN_IF_FAILED(RegisterExtension(f->ext, f->progId, dllPath));

    // 6. Offer ourselves in Settings > Default apps. This is the only way
    //    to take a file type that Windows' built-in archive handler owns
    //    (.zip through CompressedFolder, and on Windows 11 .bz2, .gz, .tar
    //    and .7z through its newer one), and it leaves the choice to the
    //    user instead of grabbing the type during registration.
    //    The user controls this from the settings program; registration
    //    only honours the stored answer. Withdrawing is explicit, so a
    //    re-register after unticking really does remove the entry.
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

    return S_OK;
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

    // The companion program's application registration.
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
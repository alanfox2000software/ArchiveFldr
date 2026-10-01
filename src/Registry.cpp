// Registry.cpp
// Fixed registration so ContextMenu (and other shellex handlers) work on Windows 11
// when the extension default ProgID is ShellNSE.* (Explorer reads shellex from ProgID,
// not from .zip alone). Also registers under SystemFileAssociations.

#include "stdafx.h"
#include "Registry.h"
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
static constexpr wchar_t kBackupValueName[] = L"ShellNSE.PreviousCLSID";

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
    LONG rc = RegDeleteTreeW(root, path);
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
        // Only record the first (i.e. the genuine, non-ShellNSE) owner.
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
// Missing these is why "Open with ShellNSE" opened nothing.
// ─────────────────────────────────────────────────────────
HRESULT CRegistry::RegisterNamespaceFolder(const wchar_t* dllPath)
{
    const std::wstring sid  = ClsidToStr(CLSID_ShellNSEFolder);
    const std::wstring base = std::wstring(L"Software\\Classes\\CLSID\\") + sid;

    // Index 2 = IDI_FOLDER_ARCHIVE. These indices only became real when
    // res/resource.rc gained its ICON statements; before that every one of
    // them resolved to nothing and the shell drew a blank page.
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\DefaultIcon").c_str(), nullptr,
        (std::wstring(dllPath) + L",2").c_str()));

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

// Register ContextMenu / Drop / Thumbnail / Preview / PropertySheet under a
// Software\Classes\... base path (extension, ProgID, or SystemFileAssociations).
HRESULT CRegistry::RegisterShellExOnBase(const std::wstring& base)
{
    const std::wstring ctx  = ClsidToStr(CLSID_ShellNSEContextMenu);
    const std::wstring drop = ClsidToStr(CLSID_ShellNSEDropTarget);
    const std::wstring th   = ClsidToStr(CLSID_ShellNSEThumbnail);
    const std::wstring pv   = ClsidToStr(CLSID_ShellNSEPreview);
    const std::wstring ps   = ClsidToStr(CLSID_ShellNSEPropSheet);

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\ContextMenuHandlers\\ShellNSE").c_str(),
        nullptr, ctx.c_str()));

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\DropHandler").c_str(),
        nullptr, drop.c_str()));

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\" + kIThumbnailProvider).c_str(),
        nullptr, th.c_str()));

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\" + kIPreviewHandler).c_str(),
        nullptr, pv.c_str()));

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\PropertySheetHandlers\\ShellNSE").c_str(),
        nullptr, ps.c_str()));

    return S_OK;
}

void CRegistry::UnregisterShellExOnBase(const std::wstring& base)
{
    DelRegKey(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\ContextMenuHandlers\\ShellNSE").c_str());
    DelRegKey(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\DropHandler").c_str());
    DelRegKey(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\" + kIThumbnailProvider).c_str());
    DelRegKey(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\" + kIPreviewHandler).c_str());
    DelRegKey(HKEY_LOCAL_MACHINE,
        (base + L"\\shellex\\PropertySheetHandlers\\ShellNSE").c_str());
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

// ── File extension registration ───────────────────────────
HRESULT CRegistry::RegisterExtension(const wchar_t* ext,
                                      const wchar_t* progId,
                                      const wchar_t* dllPath)
{
    const std::wstring folder = ClsidToStr(CLSID_ShellNSEFolder);

    // ── 1) ProgID ─────────────────────────────────────────
    // When HKCR\.zip\(Default) = ShellNSE.ZipFile, Explorer loads shellex
    // from the ProgID — NOT from .zip. This was the missing piece.
    std::wstring progBase = std::wstring(L"Software\\Classes\\") + progId;

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, progBase.c_str(),
        nullptr, L"Archive File"));

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (progBase + L"\\DefaultIcon").c_str(), nullptr,
        (std::wstring(dllPath) + L",1").c_str()));   // index 1 = IDI_ARCHIVE

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (progBase + L"\\FriendlyTypeName").c_str(),
        nullptr, L"Archive File"));

    RETURN_IF_FAILED(RegisterShellExOnBase(progBase));

    // Make the ProgID a "file as folder" junction: this single value is what
    // makes Explorer hand the archive to our namespace extension instead of
    // treating it as an opaque file. Modelled on CompressedFolder (.zip),
    // which registers HKCR\CompressedFolder\CLSID the same way.
    RETURN_IF_FAILED(TakeOverJunction(progBase + L"\\CLSID", folder));

    // Double-clicking the archive browses it, exactly like a zip folder:
    //   HKCR\<ProgID>\shell\open\command = %SystemRoot%\Explorer.exe /idlist,%I,%L
    {
        std::wstring openKey = progBase + L"\\shell\\open";
        RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, openKey.c_str(),
            L"MultiSelectModel", L"Document"));

        wchar_t explorerExe[MAX_PATH] = {};
        if (!GetWindowsDirectoryW(explorerExe, MAX_PATH))
            wcscpy_s(explorerExe, L"C:\\Windows");
        std::wstring cmd = std::wstring(explorerExe) +
                           L"\\Explorer.exe /idlist,%I,%L";
        RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
            (openKey + L"\\command").c_str(), nullptr, cmd.c_str()));
    }

    // (No extra "open with ShellNSE" static verb here on purpose: the
    // IContextMenu handler registered above already supplies that command,
    // and a second registry verb would show up as a duplicate menu entry.)

    // ── 2) Extension (.zip) ───────────────────────────────
    std::wstring extBase = std::wstring(L"Software\\Classes\\") + ext;

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, extBase.c_str(),
        nullptr, progId));

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, extBase.c_str(),
        L"PerceivedType", L"compressed"));

    // Older ShellNSE builds wrote a bogus ".ext\ShellFolder = {CLSID}" key.
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

HRESULT CRegistry::UnregisterExtension(const wchar_t* ext)
{
    const std::wstring folder = ClsidToStr(CLSID_ShellNSEFolder);

    std::wstring extBase = std::wstring(L"Software\\Classes\\") + ext;
    UnregisterShellExOnBase(extBase);
    DelRegKey(HKEY_LOCAL_MACHINE, (extBase + L"\\ShellFolder").c_str());

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
HRESULT CRegistry::RegisterOverlay(const CLSID& clsid, const wchar_t* name)
{
    // Pass a plain name. Prefixing spaces to sort ahead of other handlers
    // only works by pushing someone else out of the 15 available slots.
    std::wstring path = std::wstring(kRegKeyOverlays) + L"\\" + name;
    return SetRegStr(HKEY_LOCAL_MACHINE, path.c_str(),
        nullptr, ClsidToStr(clsid).c_str());
}

HRESULT CRegistry::UnregisterOverlay(const CLSID& /*clsid*/, const wchar_t* name)
{
    std::wstring path = std::wstring(kRegKeyOverlays) + L"\\" + name;
    return DelRegKey(HKEY_LOCAL_MACHINE, path.c_str());
}

// ─────────────────────────────────────────────────────────
// RegisterAll
// ─────────────────────────────────────────────────────────
HRESULT CRegistry::RegisterAll(const wchar_t* dllPath)
{
    // 1. COM servers
    RETURN_IF_FAILED(RegisterCOMServer(CLSID_ShellNSEFolder,
        L"ShellNSE Shell Namespace Extension", dllPath));
    RETURN_IF_FAILED(RegisterCOMServer(CLSID_ShellNSEContextMenu,
        L"ShellNSE Context Menu Handler", dllPath));
    RETURN_IF_FAILED(RegisterCOMServer(CLSID_ShellNSEIconOverlay,
        L"ShellNSE Icon Overlay Handler", dllPath));
    RETURN_IF_FAILED(RegisterCOMServer(CLSID_ShellNSEDropTarget,
        L"ShellNSE Drop Target Handler", dllPath));
    RETURN_IF_FAILED(RegisterCOMServer(CLSID_ShellNSEThumbnail,
        L"ShellNSE Thumbnail Provider", dllPath));
    RETURN_IF_FAILED(RegisterCOMServer(CLSID_ShellNSEPreview,
        L"ShellNSE Preview Handler", dllPath));
    RETURN_IF_FAILED(RegisterCOMServer(CLSID_ShellNSEPropSheet,
        L"ShellNSE Property Sheet", dllPath));

    // 1b. Namespace-extension specifics for the folder object
    //     (ShellFolder\Attributes, CATID_BrowsableShellExt, icon).
    //     Without this Explorer will not browse into an archive.
    RETURN_IF_FAILED(RegisterNamespaceFolder(dllPath));

    // 2. Approved list (Vista+)
    RETURN_IF_FAILED(RegisterApproved(CLSID_ShellNSEFolder,
        L"ShellNSE Shell Namespace Extension"));
    RETURN_IF_FAILED(RegisterApproved(CLSID_ShellNSEContextMenu,
        L"ShellNSE Context Menu Handler"));
    RETURN_IF_FAILED(RegisterApproved(CLSID_ShellNSEIconOverlay,
        L"ShellNSE Icon Overlay Handler"));
    RETURN_IF_FAILED(RegisterApproved(CLSID_ShellNSEDropTarget,
        L"ShellNSE Drop Target Handler"));
    RETURN_IF_FAILED(RegisterApproved(CLSID_ShellNSEThumbnail,
        L"ShellNSE Thumbnail Provider"));
    RETURN_IF_FAILED(RegisterApproved(CLSID_ShellNSEPreview,
        L"ShellNSE Preview Handler"));
    RETURN_IF_FAILED(RegisterApproved(CLSID_ShellNSEPropSheet,
        L"ShellNSE Property Sheet"));

    // 3. PreviewHandlers global list (needed for preview pane)
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, kRegKeyPreviewHandlers,
        ClsidToStr(CLSID_ShellNSEPreview).c_str(),
        L"ShellNSE Archive Preview Handler"));

    // 4. Icon overlay — opt-in only.
    //
    // Windows honours just 15 overlay handlers machine-wide, sorted by key
    // name, and the ones that lose are silently dropped. A badge saying
    // "this archive is an archive" is not worth evicting someone's cloud
    // sync overlay, so the key is written only when an administrator has
    // asked for it:
    //
    //     HKLM\Software\ShellNSE\IconOverlay = 1   (DWORD)
    //
    // HKLM, not HKCU, because registration runs elevated and HKCU would be
    // the administrator's hive rather than the user's. The per-user
    // ShowOverlay setting still switches it off without unregistering.
    if (ReadRegDword(HKEY_LOCAL_MACHINE, L"Software\\ShellNSE",
                     L"IconOverlay", 0) != 0)
    {
        RETURN_IF_FAILED(RegisterOverlay(CLSID_ShellNSEIconOverlay,
            L"ShellNSE_Archive"));
    }
    else
    {
        // Clear anything an earlier build left behind, including the
        // leading-space name it used to queue-jump with.
        UnregisterOverlay(CLSID_ShellNSEIconOverlay, L" ShellNSE_Archive");
    UnregisterOverlay(CLSID_ShellNSEIconOverlay, L"ShellNSE_Archive");
        UnregisterOverlay(CLSID_ShellNSEIconOverlay, L"ShellNSE_Archive");
    }

    // 5. Extensions
    // NOTE: .docx / .xlsx / .pptx intentionally OMITTED so Office is not hijacked.
    // Add them back only if you really want archive handling on Office packs.
    struct ExtDef { const wchar_t* ext; const wchar_t* progId; } exts[] = {
        { L".zip",  L"ShellNSE.ZipFile"  },
        { L".7z",   L"ShellNSE.7zFile"   },
        { L".rar",  L"ShellNSE.RarFile"  },
        { L".tar",  L"ShellNSE.TarFile"  },
        { L".gz",   L"ShellNSE.GzFile"   },
        { L".tgz",  L"ShellNSE.TgzFile"  },
        { L".bz2",  L"ShellNSE.Bz2File"  },
        { L".xz",   L"ShellNSE.XzFile"   },
        { L".zst",  L"ShellNSE.ZstFile"  },
        { L".iso",  L"ShellNSE.IsoFile"  },
        { L".cab",  L"ShellNSE.CabFile"  },
        { L".lzh",  L"ShellNSE.LzhFile"  },
        { L".wim",  L"ShellNSE.WimFile"  },
        { L".jar",  L"ShellNSE.JarFile"  },
        { L".apk",  L"ShellNSE.ApkFile"  },
    };

    for (const auto& e : exts)
        RETURN_IF_FAILED(RegisterExtension(e.ext, e.progId, dllPath));

    return S_OK;
}

// ─────────────────────────────────────────────────────────
// UnregisterAll
// ─────────────────────────────────────────────────────────
HRESULT CRegistry::UnregisterAll()
{
    struct ExtDef { const wchar_t* ext; const wchar_t* progId; } exts[] = {
        { L".zip",  L"ShellNSE.ZipFile"  },
        { L".7z",   L"ShellNSE.7zFile"   },
        { L".rar",  L"ShellNSE.RarFile"  },
        { L".tar",  L"ShellNSE.TarFile"  },
        { L".gz",   L"ShellNSE.GzFile"   },
        { L".tgz",  L"ShellNSE.TgzFile"  },
        { L".bz2",  L"ShellNSE.Bz2File"  },
        { L".xz",   L"ShellNSE.XzFile"   },
        { L".zst",  L"ShellNSE.ZstFile"  },
        { L".iso",  L"ShellNSE.IsoFile"  },
        { L".cab",  L"ShellNSE.CabFile"  },
        { L".lzh",  L"ShellNSE.LzhFile"  },
        { L".wim",  L"ShellNSE.WimFile"  },
        { L".jar",  L"ShellNSE.JarFile"  },
        { L".apk",  L"ShellNSE.ApkFile"  },
        // Clean up older installs that registered Office types
        { L".docx", L"ShellNSE.DocxFile" },
        { L".xlsx", L"ShellNSE.XlsxFile" },
        { L".pptx", L"ShellNSE.PptxFile" },
    };

    for (const auto& e : exts)
    {
        UnregisterExtension(e.ext);

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
            RegDeleteValueW(hk, ClsidToStr(CLSID_ShellNSEPreview).c_str());
            RegCloseKey(hk);
        }
    }

    UnregisterOverlay(CLSID_ShellNSEIconOverlay, L" ShellNSE_Archive");
    UnregisterOverlay(CLSID_ShellNSEIconOverlay, L"ShellNSE_Archive");

    UnregisterApproved(CLSID_ShellNSEFolder);
    UnregisterApproved(CLSID_ShellNSEContextMenu);
    UnregisterApproved(CLSID_ShellNSEIconOverlay);
    UnregisterApproved(CLSID_ShellNSEDropTarget);
    UnregisterApproved(CLSID_ShellNSEThumbnail);
    UnregisterApproved(CLSID_ShellNSEPreview);
    UnregisterApproved(CLSID_ShellNSEPropSheet);

    UnregisterCOMServer(CLSID_ShellNSEFolder);
    UnregisterCOMServer(CLSID_ShellNSEContextMenu);
    UnregisterCOMServer(CLSID_ShellNSEIconOverlay);
    UnregisterCOMServer(CLSID_ShellNSEDropTarget);
    UnregisterCOMServer(CLSID_ShellNSEThumbnail);
    UnregisterCOMServer(CLSID_ShellNSEPreview);
    UnregisterCOMServer(CLSID_ShellNSEPropSheet);

    return S_OK;
}
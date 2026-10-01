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

static constexpr wchar_t kRegKeyPreviewHandlers[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers";

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

HRESULT CRegistry::DelRegKey(HKEY root, const wchar_t* path)
{
    LONG rc = RegDeleteTreeW(root, path);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND)
        return S_OK;
    return HRESULT_FROM_WIN32(rc);
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
        (std::wstring(dllPath) + L",0").c_str()));

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (progBase + L"\\FriendlyTypeName").c_str(),
        nullptr, L"Archive File"));

    RETURN_IF_FAILED(RegisterShellExOnBase(progBase));

    // ── 2) Extension (.zip) ───────────────────────────────
    std::wstring extBase = std::wstring(L"Software\\Classes\\") + ext;

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, extBase.c_str(),
        nullptr, progId));

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, extBase.c_str(),
        L"PerceivedType", L"compressed"));

    // Project-specific ShellFolder marker (kept from original)
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (extBase + L"\\ShellFolder").c_str(), nullptr, folder.c_str()));

    // Also on the extension (harmless; ignored when ProgID owns the type)
    RETURN_IF_FAILED(RegisterShellExOnBase(extBase));

    // ── 3) SystemFileAssociations\.ext ────────────────────
    // Used by Explorer even when UserChoice / ProgID differs.
    std::wstring sfaBase =
        std::wstring(L"Software\\Classes\\SystemFileAssociations\\") + ext;
    RETURN_IF_FAILED(RegisterShellExOnBase(sfaBase));

    return S_OK;
}

HRESULT CRegistry::UnregisterExtension(const wchar_t* ext)
{
    std::wstring extBase = std::wstring(L"Software\\Classes\\") + ext;
    UnregisterShellExOnBase(extBase);
    DelRegKey(HKEY_LOCAL_MACHINE, (extBase + L"\\ShellFolder").c_str());

    std::wstring sfaBase =
        std::wstring(L"Software\\Classes\\SystemFileAssociations\\") + ext;
    UnregisterShellExOnBase(sfaBase);

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
    // Leading space = try higher overlay priority slot
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

    // 4. Icon overlay
    RETURN_IF_FAILED(RegisterOverlay(CLSID_ShellNSEIconOverlay,
        L" ShellNSE_Archive"));

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
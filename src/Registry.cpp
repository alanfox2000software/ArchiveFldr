// Registry.cpp
#include "stdafx.h"
#include "Registry.h"
#include "GUIDs.h"

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
    LONG rc = RegCreateKeyExW(root, path, 0, nullptr, 0,
                              KEY_WRITE, nullptr, &hk, nullptr);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
    rc = RegSetValueExW(hk, name, 0, REG_SZ,
        (BYTE*)value, (DWORD)((wcslen(value)+1)*sizeof(wchar_t)));
    RegCloseKey(hk);
    return HRESULT_FROM_WIN32(rc);
}

HRESULT CRegistry::DelRegKey(HKEY root, const wchar_t* path)
{
    LONG rc = RegDeleteTreeW(root, path);
    if (rc == ERROR_FILE_NOT_FOUND) return S_OK;
    return HRESULT_FROM_WIN32(rc);
}

// ── COM Server registration ───────────────────────────────
HRESULT CRegistry::RegisterCOMServer(const CLSID& clsid,
                                      const wchar_t* name,
                                      const wchar_t* dllPath,
                                      const wchar_t* threadModel)
{
    std::wstring sid  = ClsidToStr(clsid);
    std::wstring base = std::wstring(L"Software\\Classes\\CLSID\\") + sid;

    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, base.c_str(),        nullptr,  name));
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, (base+L"\\InProcServer32").c_str(), nullptr, dllPath));
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, (base+L"\\InProcServer32").c_str(), L"ThreadingModel", threadModel));
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
    // Register ProgID
    std::wstring progBase = std::wstring(L"Software\\Classes\\") + progId;
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, progBase.c_str(), nullptr, L"Archive File"));
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE,
        (progBase+L"\\DefaultIcon").c_str(), nullptr, (std::wstring(dllPath)+L",0").c_str()));

    // Shell namespace extension on extension
    std::wstring extBase = std::wstring(L"Software\\Classes\\") + ext;
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, extBase.c_str(), nullptr, progId));

    // ShellFolder handler
    std::wstring sfKey = extBase + L"\\ShellFolder";
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, sfKey.c_str(),
        nullptr, ClsidToStr(CLSID_ShellNSEFolder).c_str()));

    // ContextMenu handler
    std::wstring cmKey = extBase + L"\\shellex\\ContextMenuHandlers\\ShellNSE";
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, cmKey.c_str(),
        nullptr, ClsidToStr(CLSID_ShellNSEContextMenu).c_str()));

    // Drop Target
    std::wstring dtKey = extBase + L"\\shellex\\DropHandler";
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, dtKey.c_str(),
        nullptr, ClsidToStr(CLSID_ShellNSEDropTarget).c_str()));

    // Thumbnail handler
    std::wstring thKey = extBase + L"\\shellex\\{E357FCCD-A995-4576-B01F-234630154E96}";
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, thKey.c_str(),
        nullptr, ClsidToStr(CLSID_ShellNSEThumbnail).c_str()));

    // Preview handler
    std::wstring pvKey = extBase + L"\\shellex\\{8895b1c6-b41f-4c1c-a562-0d564250836f}";
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, pvKey.c_str(),
        nullptr, ClsidToStr(CLSID_ShellNSEPreview).c_str()));

    // Property sheet
    std::wstring psKey = extBase + L"\\shellex\\PropertySheetHandlers\\ShellNSE";
    RETURN_IF_FAILED(SetRegStr(HKEY_LOCAL_MACHINE, psKey.c_str(),
        nullptr, ClsidToStr(CLSID_ShellNSEPropSheet).c_str()));

    return S_OK;
}

HRESULT CRegistry::UnregisterExtension(const wchar_t* ext)
{
    std::wstring extBase = std::wstring(L"Software\\Classes\\") + ext;
    // Only remove our shellex entries; don't destroy system default
    DelRegKey(HKEY_LOCAL_MACHINE, (extBase+L"\\shellex\\ContextMenuHandlers\\ShellNSE").c_str());
    DelRegKey(HKEY_LOCAL_MACHINE, (extBase+L"\\shellex\\DropHandler").c_str());
    DelRegKey(HKEY_LOCAL_MACHINE, (extBase+L"\\shellex\\{E357FCCD-A995-4576-B01F-234630154E96}").c_str());
    DelRegKey(HKEY_LOCAL_MACHINE, (extBase+L"\\shellex\\{8895b1c6-b41f-4c1c-a562-0d564250836f}").c_str());
    DelRegKey(HKEY_LOCAL_MACHINE, (extBase+L"\\shellex\\PropertySheetHandlers\\ShellNSE").c_str());
    DelRegKey(HKEY_LOCAL_MACHINE, (extBase+L"\\ShellFolder").c_str());
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
    if (rc != ERROR_SUCCESS) return S_OK;
    RegDeleteValueW(hk, ClsidToStr(clsid).c_str());
    RegCloseKey(hk);
    return S_OK;
}

// ── Icon Overlay ──────────────────────────────────────────
HRESULT CRegistry::RegisterOverlay(const CLSID& clsid, const wchar_t* name)
{
    std::wstring path = std::wstring(kRegKeyOverlays) + L"\\" + name;
    return SetRegStr(HKEY_LOCAL_MACHINE, path.c_str(),
        nullptr, ClsidToStr(clsid).c_str());
}
HRESULT CRegistry::UnregisterOverlay(const CLSID& clsid, const wchar_t* name)
{
    std::wstring path = std::wstring(kRegKeyOverlays) + L"\\" + name;
    (void)clsid;
    return DelRegKey(HKEY_LOCAL_MACHINE, path.c_str());
}

// ─────────────────────────────────────────────────────────
// RegisterAll
// ─────────────────────────────────────────────────────────
HRESULT CRegistry::RegisterAll(const wchar_t* dllPath)
{
    // 1. Register COM servers
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

    // 2. Approved extensions (required for Vista+)
    RETURN_IF_FAILED(RegisterApproved(CLSID_ShellNSEFolder,      L"ShellNSE Shell Namespace Extension"));
    RETURN_IF_FAILED(RegisterApproved(CLSID_ShellNSEContextMenu, L"ShellNSE Context Menu Handler"));
    RETURN_IF_FAILED(RegisterApproved(CLSID_ShellNSEIconOverlay, L"ShellNSE Icon Overlay Handler"));
    RETURN_IF_FAILED(RegisterApproved(CLSID_ShellNSEDropTarget,  L"ShellNSE Drop Target Handler"));
    RETURN_IF_FAILED(RegisterApproved(CLSID_ShellNSEThumbnail,   L"ShellNSE Thumbnail Provider"));
    RETURN_IF_FAILED(RegisterApproved(CLSID_ShellNSEPreview,     L"ShellNSE Preview Handler"));
    RETURN_IF_FAILED(RegisterApproved(CLSID_ShellNSEPropSheet,   L"ShellNSE Property Sheet"));

    // 3. Register icon overlay
    RETURN_IF_FAILED(RegisterOverlay(CLSID_ShellNSEIconOverlay, L" ShellNSE_Archive"));

    // 4. Register all supported extensions
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
        { L".docx", L"ShellNSE.DocxFile" },
        { L".xlsx", L"ShellNSE.XlsxFile" },
        { L".pptx", L"ShellNSE.PptxFile" },
    };
    for (auto& e : exts)
        RETURN_IF_FAILED(RegisterExtension(e.ext, e.progId, dllPath));

    return S_OK;
}

// ─────────────────────────────────────────────────────────
// UnregisterAll
// ─────────────────────────────────────────────────────────
HRESULT CRegistry::UnregisterAll()
{
    // Unregister extensions
    const wchar_t* exts[] = {
        L".zip",L".7z",L".rar",L".tar",L".gz",L".tgz",
        L".bz2",L".xz",L".zst",L".iso",L".cab",L".lzh",
        L".wim",L".jar",L".apk",L".docx",L".xlsx",L".pptx",
        nullptr
    };
    for (int i = 0; exts[i]; i++) UnregisterExtension(exts[i]);

    // Unregister overlay
    UnregisterOverlay(CLSID_ShellNSEIconOverlay, L" ShellNSE_Archive");

    // Unapprove
    UnregisterApproved(CLSID_ShellNSEFolder);
    UnregisterApproved(CLSID_ShellNSEContextMenu);
    UnregisterApproved(CLSID_ShellNSEIconOverlay);
    UnregisterApproved(CLSID_ShellNSEDropTarget);
    UnregisterApproved(CLSID_ShellNSEThumbnail);
    UnregisterApproved(CLSID_ShellNSEPreview);
    UnregisterApproved(CLSID_ShellNSEPropSheet);

    // Unregister COM servers
    UnregisterCOMServer(CLSID_ShellNSEFolder);
    UnregisterCOMServer(CLSID_ShellNSEContextMenu);
    UnregisterCOMServer(CLSID_ShellNSEIconOverlay);
    UnregisterCOMServer(CLSID_ShellNSEDropTarget);
    UnregisterCOMServer(CLSID_ShellNSEThumbnail);
    UnregisterCOMServer(CLSID_ShellNSEPreview);
    UnregisterCOMServer(CLSID_ShellNSEPropSheet);

    return S_OK;
}
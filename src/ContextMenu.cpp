// ContextMenu.cpp
#include "stdafx.h"
#include "ContextMenu.h"
#include "ShellFolder.h"
#include "ArchiveEngine.h"
#include "SevenZipEngine.h"   // Is7zEngineAvailable() / Get7zEnginePath()
#include "Settings.h"
#include "SettingsDialog.h"
#include "GUIDs.h"
#include "../res/resource.h"

CContextMenu::CContextMenu() { InterlockedIncrement(&g_cDllRefCount); }
CContextMenu::~CContextMenu()
{
    for (auto p : m_pidls) ILFree(p);
    if (m_pFolder) m_pFolder->Release();
    InterlockedDecrement(&g_cDllRefCount);
}

void CContextMenu::SetFolder(CShellFolder* pFolder, HWND hwnd,
                              UINT cidl, LPCITEMIDLIST const* apidl)
{
    m_pFolder = pFolder; if (m_pFolder) m_pFolder->AddRef();
    m_hwnd    = hwnd;
    if (pFolder) m_archivePath = pFolder->GetArchivePath();
    for (UINT i = 0; i < cidl; i++)
        m_pidls.push_back(CPidlMgr::Clone(apidl[i]));
}

// ── IUnknown ─────────────────────────────────────────────
STDMETHODIMP CContextMenu::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER; *ppv = nullptr;
    if (IsEqualIID(riid,IID_IUnknown)||
        IsEqualIID(riid,IID_IContextMenu)||
        IsEqualIID(riid,IID_IContextMenu2)||
        IsEqualIID(riid,IID_IContextMenu3))
    { *ppv=static_cast<IContextMenu3*>(this); AddRef(); return S_OK; }
    if (IsEqualIID(riid,IID_IShellExtInit))
    { *ppv=static_cast<IShellExtInit*>(this); AddRef(); return S_OK; }
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CContextMenu::AddRef()
    { return InterlockedIncrement(&m_cRef); }
STDMETHODIMP_(ULONG) CContextMenu::Release()
    { ULONG n=InterlockedDecrement(&m_cRef); if(!n) delete this; return n; }

// ── IShellExtInit ─────────────────────────────────────────
STDMETHODIMP CContextMenu::Initialize(LPCITEMIDLIST /*pidlFolder*/,
                                       IDataObject* pdtobj,
                                       HKEY /*hkeyProgID*/)
{
    if (!pdtobj) return E_INVALIDARG;
    FORMATETC fe{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (FAILED(pdtobj->GetData(&fe, &sm))) return E_FAIL;
    HDROP hDrop = (HDROP)GlobalLock(sm.hGlobal);
    if (hDrop) {
        wchar_t path[MAX_PATH*2] = {};
        DragQueryFileW(hDrop, 0, path, MAX_PATH*2);
        m_archivePath = path;
        GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
    return S_OK;
}

// ── IContextMenu::QueryContextMenu ───────────────────────
STDMETHODIMP CContextMenu::QueryContextMenu(
    HMENU hMenu, UINT indexMenu, UINT idCmdFirst,
    UINT /*idCmdLast*/, UINT uFlags)
{
    if (uFlags & CMF_DEFAULTONLY) return MAKE_HRESULT(SEVERITY_SUCCESS,0,0);
    m_cmdBase = idCmdFirst;
    auto& s = Settings::Get();
    m_useSubMenu = s.ctxUseSubMenu;

    HMENU hTarget = hMenu;
    UINT  pos     = indexMenu;

    if (m_useSubMenu) {
        hTarget = CreatePopupMenu();
        pos = 0;
    }

    UINT id = idCmdFirst;
    auto addItem = [&](bool enabled, UINT cmd, const wchar_t* text) {
        if (enabled) {
            MENUITEMINFOW mi{sizeof(mi), MIIM_STRING|MIIM_ID|MIIM_STATE};
            mi.wID       = id + cmd;
            mi.dwTypeData = (LPWSTR)text;
            mi.fState    = MFS_ENABLED;
            InsertMenuItemW(hTarget, pos++, TRUE, &mi);
        }
    };

    if (s.ctxExtract)       addItem(true, CMD_EXTRACT,       L"Extract...");
    if (s.ctxExtractHere)   addItem(true, CMD_EXTRACTHERE,   L"Extract Here");
    InsertMenuItemW(hTarget, pos++, TRUE, []{
        static MENUITEMINFOW sep{sizeof(MENUITEMINFOW)};
        sep.fMask = MIIM_TYPE;
        sep.fType = MFT_SEPARATOR;
        return &sep;
    }());
    if (s.ctxAddToArchive)  addItem(true, CMD_ADD,           L"Add to Archive...");
    if (s.ctxCompressEmail) addItem(true, CMD_COMPRESS_EMAIL,L"Compress and E-mail...");
    InsertMenuW(hTarget, pos++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    if (s.ctxOpenInShell)   addItem(true, CMD_OPEN_SHELL,    L"Open with ShellNSE");
    if (s.ctxTestArchive)   addItem(true, CMD_TEST,          L"Test Archive");
    if (s.ctxArchiveInfo)   addItem(true, CMD_INFO,          L"Archive Info...");
    if (s.ctxSettings)      addItem(true, CMD_SETTINGS,      L"ShellNSE Settings...");

    if (m_useSubMenu) {
        MENUITEMINFOW mi{sizeof(mi),MIIM_STRING|MIIM_SUBMENU|MIIM_STATE};
        mi.hSubMenu   = hTarget;
        mi.dwTypeData = (LPWSTR)s.ctxSubMenuTitle.c_str();
        mi.fState     = MFS_ENABLED;
        InsertMenuItemW(hMenu, indexMenu, TRUE, &mi);
    }

    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, CMD_COUNT);
}

// ── IContextMenu::InvokeCommand ───────────────────────────
// A caller may address a command either by menu offset (lpVerb packed with
// MAKEINTRESOURCE) or by its canonical verb string — and the verb may arrive
// in ANSI (lpVerb) or, with CMINVOKECOMMANDINFOEX, Unicode (lpVerbW) form.
// All of those have to be accepted: Explorer picks the form, not us.
STDMETHODIMP CContextMenu::InvokeCommand(LPCMINVOKECOMMANDINFO pici)
{
    if (!pici) return E_POINTER;

    // Parent any UI we show (progress, message boxes, dialogs) on the window
    // that invoked us — otherwise dialogs come up ownerless behind Explorer.
    if (pici->hwnd) m_hwnd = pici->hwnd;

    UINT cmd = (UINT)-1;
    std::wstring verb;

    const CMINVOKECOMMANDINFOEX* piciEx =
        (pici->cbSize >= sizeof(CMINVOKECOMMANDINFOEX))
            ? reinterpret_cast<const CMINVOKECOMMANDINFOEX*>(pici) : nullptr;

    if (piciEx && (piciEx->fMask & CMIC_MASK_UNICODE) &&
        piciEx->lpVerbW && !IS_INTRESOURCE(piciEx->lpVerbW))
    {
        verb = piciEx->lpVerbW;
    }
    else if (pici->lpVerb && !IS_INTRESOURCE(pici->lpVerb))
    {
        int need = MultiByteToWideChar(CP_ACP, 0, pici->lpVerb, -1, nullptr, 0);
        if (need > 1) {
            verb.resize((size_t)need - 1);
            MultiByteToWideChar(CP_ACP, 0, pici->lpVerb, -1, verb.data(), need);
        }
    }
    else if (pici->lpVerb)   // MAKEINTRESOURCE(offset); NULL means "default verb"
    {
        cmd = (UINT)LOWORD(reinterpret_cast<UINT_PTR>(pici->lpVerb));
    }

    if (!verb.empty())
    {
        static const struct { const wchar_t* verb; UINT cmd; } kVerbs[] = {
            { L"extract",     CMD_EXTRACT        },
            { L"extracthere", CMD_EXTRACTHERE    },
            { L"add",         CMD_ADD            },
            { L"email",       CMD_COMPRESS_EMAIL },
            { L"openshell",   CMD_OPEN_SHELL     },
            { L"open",        CMD_OPEN_SHELL     },  // legacy alias
            { L"test",        CMD_TEST           },
            { L"info",        CMD_INFO           },
            { L"settings",    CMD_SETTINGS       },
        };
        for (const auto& k : kVerbs)
            if (_wcsicmp(verb.c_str(), k.verb) == 0) { cmd = k.cmd; break; }
    }

    if (cmd >= CMD_COUNT) return E_INVALIDARG;

    switch (cmd) {
    case CMD_EXTRACT:       DoExtract(false); break;
    case CMD_EXTRACTHERE:   DoExtract(true);  break;
    case CMD_ADD:           DoAdd();           break;
    case CMD_COMPRESS_EMAIL:DoCompressEmail(); break;
    case CMD_OPEN_SHELL:    DoOpenShell();     break;
    case CMD_TEST:          DoTest();          break;
    case CMD_INFO:          DoInfo();          break;
    case CMD_SETTINGS:      DoSettings();      break;
    default: return E_INVALIDARG;
    }
    return S_OK;
}

// ── IContextMenu::GetCommandString ────────────────────────

STDMETHODIMP CContextMenu::GetCommandString(
    UINT_PTR idCmd, UINT uType,
    UINT* /*pReserved*/, CHAR* pszName, UINT cchMax)
{
    if (idCmd >= CMD_COUNT) return E_INVALIDARG;

    // Help text (Unicode)
    static const wchar_t* const helps[] = {
        L"Extract archive contents to a folder",
        L"Extract archive contents here",
        L"Add files to archive",
        L"Compress and send by e-mail",
        L"Open archive with ShellNSE",
        L"Test archive integrity",
        L"View archive information",
        L"Open ShellNSE settings",
    };

    // Verb strings (ANSI)
    // NOTE: the ShellNSE browse command is "openshell", not "open" — a verb
    // literally named "open" collides with the file type's own default verb
    // and can make Explorer route the wrong command here. InvokeCommand()
    // still accepts "open" as a backwards-compatible alias.
    static const char* const verbsA[] = {
        "extract",   "extracthere", "add",  "email",
        "openshell", "test",        "info", "settings"
    };

    // Verb strings (Unicode)
    static const wchar_t* const verbsW[] = {
        L"extract",   L"extracthere", L"add",   L"email",
        L"openshell", L"test",        L"info",  L"settings"
    };

    switch (uType)
    {
    case GCS_HELPTEXTW:   // Unicode help text
        wcsncpy_s(reinterpret_cast<wchar_t*>(pszName),
                  cchMax, helps[idCmd], _TRUNCATE);
        return S_OK;

    case GCS_HELPTEXTA:   // ANSI help text (convert)
    {
        char ansiHelp[256] = {};
        WideCharToMultiByte(CP_ACP, 0,
            helps[idCmd], -1,
            ansiHelp, sizeof(ansiHelp), nullptr, nullptr);
        strncpy_s(pszName, cchMax, ansiHelp, _TRUNCATE);
        return S_OK;
    }

    case GCS_VERBA:       // ANSI verb
        strncpy_s(pszName, cchMax, verbsA[idCmd], _TRUNCATE);
        return S_OK;

    case GCS_VERBW:       // Unicode verb
        wcsncpy_s(reinterpret_cast<wchar_t*>(pszName),
                  cchMax, verbsW[idCmd], _TRUNCATE);
        return S_OK;

    case GCS_VALIDATEA:
    case GCS_VALIDATEW:
        return S_OK;      // idCmd is valid

    default:
        return E_INVALIDARG;
    }
}

STDMETHODIMP CContextMenu::HandleMenuMsg(UINT,WPARAM,LPARAM) { return S_OK; }
STDMETHODIMP CContextMenu::HandleMenuMsg2(UINT,WPARAM,LPARAM,LRESULT* p)
    { if(p)*p=0; return S_OK; }

// ── Command implementations ───────────────────────────────
void CContextMenu::DoExtract(bool here)
{
    std::wstring dest;
    if (here) {
        wchar_t dir[MAX_PATH]; wcscpy_s(dir, m_archivePath.c_str());
        PathRemoveFileSpecW(dir); dest = dir;
    } else {
        wchar_t buf[MAX_PATH] = {};
        BROWSEINFOW bi{m_hwnd,nullptr,buf,L"Select destination folder:",
            BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE};
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if (!pidl) return;
        SHGetPathFromIDListW(pidl, buf);
        CoTaskMemFree(pidl); dest = buf;
    }
    auto engine = CreateArchiveEngine(m_archivePath);
    if (!engine || !engine->Open(m_archivePath)) return;
    engine->ExtractAll(dest, nullptr);
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH, dest.c_str(), nullptr);
}

void CContextMenu::DoAdd()
{
    wchar_t buf[32768] = {};
    OPENFILENAMEW ofn{sizeof(ofn),m_hwnd};
    ofn.lpstrFilter = L"All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = buf; ofn.nMaxFile = 32767;
    ofn.Flags = OFN_ALLOWMULTISELECT|OFN_EXPLORER|OFN_FILEMUSTEXIST;
    ofn.lpstrTitle  = L"Add Files to Archive";
    if (!GetOpenFileNameW(&ofn)) return;

    auto engine = CreateArchiveEngine(m_archivePath);
    if (!engine) return;
    bool isNew = !PathFileExistsW(m_archivePath.c_str());
    if (isNew) engine->Create(m_archivePath);
    else       engine->Open(m_archivePath);

    wchar_t* p = buf;
    std::wstring dir = p; p += dir.size()+1;
    while (*p) {
        engine->AddFile(dir+L"\\"+p, L"", nullptr);
        p += wcslen(p)+1;
    }
}

void CContextMenu::DoCompressEmail()
{
    // Compress to temp then launch MAPI
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH,tmp);
    std::wstring zipPath = std::wstring(tmp) + L"archive.zip";
    auto engine = CreateArchiveEngine(zipPath);
    if (!engine) return;
    engine->Create(zipPath);
    for (auto& p : m_pidls) {
        wchar_t name[MAX_PATH]; SHGetPathFromIDListW(p,name);
        engine->AddFile(name, L"", nullptr);
    }
    // Launch default e-mail client with attachment (MAPI)
    wchar_t cmd[MAX_PATH*2];
    swprintf_s(cmd,MAX_PATH*2,L"mailto:?subject=Archive&attach=%s",zipPath.c_str());
    ShellExecuteW(m_hwnd,L"open",cmd,nullptr,nullptr,SW_SHOWNORMAL);
}

void CContextMenu::DoOpenShell()
{
    // ─────────────────────────────────────────────────────────────────
    // "Open with ShellNSE" — browse the archive inside Windows Explorer.
    //
    // This used to be ShellExecute(L"open", <archive>), which just asks the
    // shell to run the file type's *default* open command. For an archive
    // that is either nothing at all (silent no-op — the bug), or whatever
    // other archiver owns the association. It could never show the archive
    // in Explorer, because nothing told Explorer to use our namespace
    // extension.
    //
    // The documented way to open a view of a namespace extension on a
    // specific object is (see "Specifying a Namespace Extension's Location"):
    //
    //     %SystemRoot%\Explorer.exe /e,::{extension CLSID},<object name>
    //
    // Explorer parses <object name> into a PIDL and hands it to our
    // IPersistFolder::Initialize — exactly what CShellFolder expects. Going
    // through the CLSID explicitly also means this works no matter which
    // application currently owns the .7z/.zip file association.
    // ─────────────────────────────────────────────────────────────────
    if (m_archivePath.empty()) {
        MessageBoxW(m_hwnd, L"No archive was selected.",
                    L"ShellNSE", MB_ICONWARNING | MB_OK);
        return;
    }

    if (!PathFileExistsW(m_archivePath.c_str())) {
        MessageBoxW(m_hwnd,
            (L"The archive no longer exists:\n\n" + m_archivePath).c_str(),
            L"ShellNSE", MB_ICONERROR | MB_OK);
        return;
    }

    // Pre-flight the archive so a failure is reported here, with a reason,
    // instead of silently producing an empty Explorer window.
    LPCWSTR ext = PathFindExtensionW(m_archivePath.c_str());
    const bool is7z = ext && (_wcsicmp(ext, L".7z")   == 0 ||
                              _wcsicmp(ext, L".7zip") == 0);
    if (is7z && !Is7zEngineAvailable())
    {
        MessageBoxW(m_hwnd,
            L"The 7-Zip engine DLL was not found, so .7z archives cannot be "
            L"opened.\n\n"
            L"Put a bitness-matched 7z.dll next to ShellNSE, in any of:\n"
            L"    <ShellNSE folder>\\thirdparty\\7z\\7z.64.dll   (64-bit)\n"
            L"    <ShellNSE folder>\\thirdparty\\7z\\7z.32.dll   (32-bit)\n"
            L"    <ShellNSE folder>\\7z.64.dll  /  7z.32.dll  /  7z.dll\n\n"
            L"A system-wide 7-Zip installation is also used automatically.",
            L"ShellNSE", MB_ICONERROR | MB_OK);
        return;
    }

    {
        auto engine = CreateArchiveEngine(m_archivePath);
        if (!engine || !engine->Open(m_archivePath))
        {
            std::wstring msg = L"ShellNSE could not read this archive:\n\n" +
                               m_archivePath;
            if (is7z)
                msg += L"\n\nEngine: " + (Get7zEnginePath().empty()
                                            ? std::wstring(L"<none>")
                                            : Get7zEnginePath()) +
                       L"\n\nThe file may be corrupt, or it may use encrypted "
                       L"headers (password-protected archives are not "
                       L"supported yet).";
            MessageBoxW(m_hwnd, msg.c_str(), L"ShellNSE",
                        MB_ICONERROR | MB_OK);
            return;
        }
    }

    // explorer.exe /e,::{CLSID},<archive path>
    wchar_t clsid[64] = {};
    StringFromGUID2(CLSID_ShellNSEFolder, clsid, ARRAYSIZE(clsid));

    wchar_t explorerExe[MAX_PATH] = {};
    if (GetWindowsDirectoryW(explorerExe, MAX_PATH))
        PathAppendW(explorerExe, L"explorer.exe");
    else
        wcscpy_s(explorerExe, L"explorer.exe");

    std::wstring params = L"/e,::";
    params += clsid;
    params += L',';
    params += m_archivePath;

    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask        = SEE_MASK_FLAG_NO_UI;
    sei.hwnd         = m_hwnd;
    sei.lpVerb       = L"open";
    sei.lpFile       = explorerExe;
    sei.lpParameters = params.c_str();
    sei.nShow        = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei))
        return;

    // Fallback: browse the archive as a file-system junction. This is what
    // double-clicking does once DllRegisterServer has run, so it works even
    // if launching explorer.exe with a rooted CLSID was blocked.
    if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(m_archivePath.c_str()))
    {
        HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
        if (SUCCEEDED(hr)) return;
    }

    MessageBoxW(m_hwnd,
        L"ShellNSE could not open an Explorer window for this archive.\n\n"
        L"Make sure the extension is registered (run, as administrator):\n"
        L"    regsvr32 ShellNSE.64.dll",
        L"ShellNSE", MB_ICONERROR | MB_OK);
}

void CContextMenu::DoTest()
{
    auto engine = CreateArchiveEngine(m_archivePath);
    if (!engine || !engine->Open(m_archivePath)) return;
    bool ok = engine->Test(nullptr);
    MessageBoxW(m_hwnd,
        ok ? L"Archive test: PASSED\nAll files are intact."
           : L"Archive test: FAILED!\nCorruption detected.",
        L"Test Archive", ok ? MB_ICONINFORMATION : MB_ICONERROR);
}

void CContextMenu::DoInfo()
{
    auto engine = CreateArchiveEngine(m_archivePath);
    if (!engine || !engine->Open(m_archivePath)) return;
    wchar_t buf[512];
    swprintf_s(buf, 512,
        L"Archive: %s\nFiles: %llu\nSize: %s\nPacked: %s",
        PathFindFileNameW(m_archivePath.c_str()),
        engine->GetFileCount(),
        engine->GetFormattedSize(engine->GetTotalSize()).c_str(),
        engine->GetFormattedSize(engine->GetPackedSize()).c_str());
    MessageBoxW(m_hwnd, buf, L"Archive Info", MB_ICONINFORMATION);
}

void CContextMenu::DoSettings()
{
    CSettingsDialog dlg;
    dlg.Show(m_hwnd);
}
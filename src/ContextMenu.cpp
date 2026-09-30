// ContextMenu.cpp
#include "stdafx.h"
#include "ContextMenu.h"
#include "ShellFolder.h"
#include "ArchiveEngine.h"
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
    InsertMenuW(hTarget, pos++, TRUE, MF_SEPARATOR, 0, nullptr);
    if (s.ctxAddToArchive)  addItem(true, CMD_ADD,           L"Add to Archive...");
    if (s.ctxCompressEmail) addItem(true, CMD_COMPRESS_EMAIL,L"Compress and E-mail...");
    InsertMenuW(hTarget, pos++, TRUE, MF_SEPARATOR, 0, nullptr);
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
STDMETHODIMP CContextMenu::InvokeCommand(LPCMINVOKECOMMANDINFO pici)
{
    if (!pici) return E_POINTER;
    UINT cmd = IS_INTRESOURCE(pici->lpVerb) ?
        (UINT)LOWORD(pici->lpVerb) : 0xFFFF;

    // Also handle string verbs
    if (!IS_INTRESOURCE(pici->lpVerb)) {
        std::string v = pici->lpVerb;
        if      (v=="extract")  cmd = CMD_EXTRACT;
        else if (v=="extracthere") cmd = CMD_EXTRACTHERE;
        else if (v=="add")      cmd = CMD_ADD;
        else if (v=="test")     cmd = CMD_TEST;
        else if (v=="info")     cmd = CMD_INFO;
        else if (v=="settings") cmd = CMD_SETTINGS;
        else return E_FAIL;
    }

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
    UINT_PTR idCmd, UINT uType, UINT*, CHAR* pszName, UINT cchMax)
{
    static const wchar_t* helps[] = {
        L"Extract archive contents",
        L"Extract here in place",
        L"Add files to archive",
        L"Compress and send by e-mail",
        L"Open in ShellNSE browser",
        L"Test archive integrity",
        L"View archive information",
        L"Open ShellNSE settings",
    };
    static const char* verbs[] = {
        "extract","extracthere","add","email",
        "open","test","info","settings"
    };
    if (idCmd >= CMD_COUNT) return E_INVALIDARG;
    if (uType == GCS_HELPW)
        return StringCchCopyW((LPWSTR)pszName,cchMax,helps[idCmd]);
    if (uType == GCS_VERBA)
        return StringCchCopyA(pszName,cchMax,verbs[idCmd]);
    if (uType == GCS_VERBW)
        return StringCchCopyW((LPWSTR)pszName,cchMax,
            (LPCWSTR)_bstr_t(verbs[idCmd]));
    return S_OK;
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
    // Open the archive in Windows Explorer (via ShellExecute)
    ShellExecuteW(m_hwnd, L"open",
        m_archivePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
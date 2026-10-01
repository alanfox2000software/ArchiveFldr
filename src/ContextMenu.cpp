// ContextMenu.cpp
#include "stdafx.h"
#include "ContextMenu.h"
#include "ShellFolder.h"
#include "DataObject.h"
#include "ArchiveEngine.h"
#include "ArchiveOps.h"
#include "SevenZipEngine.h"   // Is7zEngineAvailable() / Get7zEnginePath()
#include "ThirdParty.h"
#include "Settings.h"
#include "SettingsDialog.h"
#include "GUIDs.h"
#include "../res/resource.h"

// ─────────────────────────────────────────────────────────
// Verb table — ONE source of truth for command id ↔ verb ↔ help text.
// Order must match the Cmd enum in ContextMenu.h.
// ─────────────────────────────────────────────────────────
namespace {

struct VerbDef {
    const wchar_t* verbW;
    const char*    verbA;
    const wchar_t* help;
};

const VerbDef kVerbs[] = {
    { L"open",        "open",        L"Open this item"                        },
    { L"extract",     "extract",     L"Extract to a folder"                   },
    { L"extracthere", "extracthere", L"Extract here"                          },
    { L"add",         "add",         L"Add files to archive"                  },
    { L"email",       "email",       L"Compress and send by e-mail"           },
    { L"openshell",   "openshell",   L"Browse this archive in Explorer"       },
    { L"test",        "test",        L"Test archive integrity"                },
    { L"info",        "info",        L"View archive information"              },
    { L"copy",        "copy",        L"Copy to the clipboard"                 },
    { L"paste",       "paste",       L"Add the clipboard's files here"        },
    { L"refresh",     "refresh",     L"Refresh this view"                     },
    { L"properties",  "properties",  L"Show properties"                       },
    { L"settings",    "settings",    L"Open ShellNSE settings"                },
};

// Scoped hourglass for the operations that can take a moment.
struct WaitCursor {
    HCURSOR prev;
    WaitCursor()  : prev(SetCursor(LoadCursorW(nullptr, IDC_WAIT))) {}
    ~WaitCursor() { SetCursor(prev); }
};

} // namespace

CContextMenu::CContextMenu() { InterlockedIncrement(&g_cDllRefCount); }
CContextMenu::~CContextMenu()
{
    for (auto p : m_pidls) ILFree(p);
    if (m_pFolder) m_pFolder->Release();
    if (m_pSite)   m_pSite->Release();
    InterlockedDecrement(&g_cDllRefCount);
}

void CContextMenu::SetFolder(CShellFolder* pFolder, HWND hwnd,
                              UINT cidl, LPCITEMIDLIST const* apidl)
{
    m_pFolder = pFolder; if (m_pFolder) m_pFolder->AddRef();
    m_hwnd    = hwnd;
    m_mode    = ModeItem;
    if (pFolder) m_archivePath = pFolder->GetArchivePath();
    for (UINT i = 0; i < cidl; i++)
        m_pidls.push_back(CPidlMgr::Clone(apidl[i]));
}

void CContextMenu::SetBackground(CShellFolder* pFolder, HWND hwnd)
{
    m_pFolder = pFolder; if (m_pFolder) m_pFolder->AddRef();
    m_hwnd    = hwnd;
    m_mode    = ModeBackground;
    if (pFolder) m_archivePath = pFolder->GetArchivePath();
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
    if (IsEqualIID(riid,IID_IObjectWithSite))
    { *ppv=static_cast<IObjectWithSite*>(this); AddRef(); return S_OK; }
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
    m_mode = ModeArchiveFile;
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
    static_assert(ARRAYSIZE(kVerbs) == CMD_COUNT,
                  "verb table and Cmd enum are out of sync");

    m_cmdBase = idCmdFirst;

    // CMF_DEFAULTONLY = "tell me the one command a double-click should run".
    //
    // For a FILE that is Open (extract a temp copy and launch it) — saying
    // nothing here is why double-clicking a file used to do nothing.
    //
    // For a FOLDER we must stay silent: the view browses into a sub-folder
    // by itself, and any default command we claim here replaces that
    // navigation with our own handler. That is exactly what stopped
    // double-click from entering folders inside an archive.
    if (uFlags & CMF_DEFAULTONLY)
    {
        if (m_mode != ModeItem || m_pidls.empty())
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);

        bool allFolders = true;
        for (auto p : m_pidls)
            if (!CPidlMgr::IsDir(p)) { allFolders = false; break; }
        if (allFolders)
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);   // let the view navigate

        InsertMenuW(hMenu, indexMenu, MF_BYPOSITION | MF_STRING,
                    idCmdFirst + CMD_OPEN_ITEM, L"&Open");
        SetMenuDefaultItem(hMenu, idCmdFirst + CMD_OPEN_ITEM, FALSE);
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, CMD_OPEN_ITEM + 1);
    }

    auto& s = Settings::Get();
    // The "collect everything under one ShellNSE sub-menu" preference only
    // applies to the crowded file menu in a normal Explorer folder.
    m_useSubMenu = s.ctxUseSubMenu && (m_mode == ModeArchiveFile);

    HMENU hTarget = m_useSubMenu ? CreatePopupMenu() : hMenu;
    UINT  pos     = m_useSubMenu ? 0 : indexMenu;
    UINT  used    = 0;

    auto addItem = [&](UINT cmd, const wchar_t* text, bool enabled = true) {
        MENUITEMINFOW mi{sizeof(mi), MIIM_STRING | MIIM_ID | MIIM_STATE};
        mi.wID        = idCmdFirst + cmd;
        mi.dwTypeData = (LPWSTR)text;
        mi.fState     = enabled ? MFS_ENABLED : MFS_GRAYED;
        InsertMenuItemW(hTarget, pos++, TRUE, &mi);
        if (cmd + 1 > used) used = cmd + 1;
    };
    auto addSep = [&] {
        InsertMenuW(hTarget, pos++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    };

    switch (m_mode)
    {
    // ── Items inside an archive ──────────────────────────
    case ModeItem:
    {
        const bool single = (m_pidls.size() == 1);
        addItem(CMD_OPEN_ITEM, single ? L"&Open" : L"&Open items");
        SetMenuDefaultItem(hTarget, idCmdFirst + CMD_OPEN_ITEM, FALSE);
        addSep();
        addItem(CMD_EXTRACT,     L"E&xtract selected...");
        addItem(CMD_EXTRACTHERE, L"Extract selected &here");
        addSep();
        addItem(CMD_COPY,        L"&Copy");
        addSep();
        addItem(CMD_TEST,        L"&Test archive");
        if (single) addItem(CMD_PROPERTIES, L"P&roperties");
        break;
    }

    // ── Empty space in an archive's view ─────────────────
    case ModeBackground:
    {
        addItem(CMD_EXTRACT,     L"E&xtract all...");
        addItem(CMD_EXTRACTHERE, L"Extract all &here");
        addSep();
        addItem(CMD_PASTE,       L"&Paste", ArchiveOps::ClipboardHasFiles());
        addItem(CMD_REFRESH,     L"&Refresh");
        addSep();
        addItem(CMD_TEST,        L"&Test archive");
        addItem(CMD_INFO,        L"Archive &info...");
        addItem(CMD_SETTINGS,    L"ShellNSE &settings...");
        break;
    }

    // ── An archive file in a normal Explorer folder ──────
    case ModeArchiveFile:
    default:
    {
        if (s.ctxExtract)       addItem(CMD_EXTRACT,        L"Extract...");
        if (s.ctxExtractHere)   addItem(CMD_EXTRACTHERE,    L"Extract Here");
        addSep();
        if (s.ctxAddToArchive)  addItem(CMD_ADD,            L"Add to Archive...");
        if (s.ctxCompressEmail) addItem(CMD_COMPRESS_EMAIL, L"Compress and E-mail...");
        addSep();
        if (s.ctxOpenInShell)   addItem(CMD_OPEN_SHELL,     L"Open with ShellNSE");
        if (s.ctxTestArchive)   addItem(CMD_TEST,           L"Test Archive");
        if (s.ctxArchiveInfo)   addItem(CMD_INFO,           L"Archive Info...");
        if (s.ctxSettings)      addItem(CMD_SETTINGS,       L"ShellNSE Settings...");
        break;
    }
    }

    if (m_useSubMenu) {
        MENUITEMINFOW mi{sizeof(mi),MIIM_STRING|MIIM_SUBMENU|MIIM_STATE};
        mi.hSubMenu   = hTarget;
        mi.dwTypeData = (LPWSTR)s.ctxSubMenuTitle.c_str();
        mi.fState     = MFS_ENABLED;
        InsertMenuItemW(hMenu, indexMenu, TRUE, &mi);
    }

    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, used);
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
    else
    {
        // No verb at all: run the default command for this menu.
        cmd = (m_mode == ModeItem) ? (UINT)CMD_OPEN_ITEM : (UINT)CMD_OPEN_SHELL;
    }

    if (!verb.empty())
    {
        for (UINT i = 0; i < CMD_COUNT; ++i)
            if (_wcsicmp(verb.c_str(), kVerbs[i].verbW) == 0) { cmd = i; break; }

        // "open" means different things in different menus: browse the
        // archive when invoked on the file, open the entry when invoked
        // inside it.
        if (cmd == CMD_OPEN_ITEM && m_mode != ModeItem) cmd = CMD_OPEN_SHELL;
    }

    if (cmd >= CMD_COUNT) return E_INVALIDARG;

    switch (cmd) {
    case CMD_OPEN_ITEM:     DoOpenItem();     break;
    case CMD_EXTRACT:       DoExtract(false); break;
    case CMD_EXTRACTHERE:   DoExtract(true);  break;
    case CMD_ADD:           DoAdd();          break;
    case CMD_COMPRESS_EMAIL:DoCompressEmail();break;
    case CMD_OPEN_SHELL:    DoOpenShell();    break;
    case CMD_TEST:          DoTest();         break;
    case CMD_INFO:          DoInfo();         break;
    case CMD_COPY:          DoCopy();         break;
    case CMD_PASTE:         DoPaste();        break;
    case CMD_REFRESH:       DoRefresh();      break;
    case CMD_PROPERTIES:    DoProperties();   break;
    case CMD_SETTINGS:      DoSettings();     break;
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
    const VerbDef& v = kVerbs[idCmd];

    switch (uType)
    {
    case GCS_HELPTEXTW:
        wcsncpy_s(reinterpret_cast<wchar_t*>(pszName), cchMax, v.help, _TRUNCATE);
        return S_OK;

    case GCS_HELPTEXTA:
    {
        char ansiHelp[256] = {};
        WideCharToMultiByte(CP_ACP, 0, v.help, -1,
                            ansiHelp, sizeof(ansiHelp), nullptr, nullptr);
        strncpy_s(pszName, cchMax, ansiHelp, _TRUNCATE);
        return S_OK;
    }

    case GCS_VERBA:
        strncpy_s(pszName, cchMax, v.verbA, _TRUNCATE);
        return S_OK;

    case GCS_VERBW:
        wcsncpy_s(reinterpret_cast<wchar_t*>(pszName), cchMax, v.verbW, _TRUNCATE);
        return S_OK;

    case GCS_VALIDATEA:
    case GCS_VALIDATEW:
        return S_OK;      // idCmd is valid

    default:
        return E_INVALIDARG;
    }
}

// ── IObjectWithSite ──────────────────────────────────────
STDMETHODIMP CContextMenu::SetSite(IUnknown* pUnkSite)
{
    if (m_pSite) { m_pSite->Release(); m_pSite = nullptr; }
    m_pSite = pUnkSite;
    if (m_pSite) m_pSite->AddRef();
    return S_OK;
}

STDMETHODIMP CContextMenu::GetSite(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!m_pSite) return E_FAIL;
    return m_pSite->QueryInterface(riid, ppv);
}

STDMETHODIMP CContextMenu::HandleMenuMsg(UINT,WPARAM,LPARAM) { return S_OK; }
STDMETHODIMP CContextMenu::HandleMenuMsg2(UINT,WPARAM,LPARAM,LRESULT* p)
    { if(p)*p=0; return S_OK; }

// ─────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────

// Inside an archive view we reuse the folder's already-open engine; invoked
// on an archive file we open our own.
std::shared_ptr<IArchiveEngine> CContextMenu::AcquireEngine()
{
    if (m_pFolder) {
        if (auto eng = m_pFolder->GetEngine()) return eng;
    }
    if (m_archivePath.empty()) return nullptr;
    auto eng = CreateArchiveEngine(m_archivePath);
    if (eng && !eng->Open(m_archivePath)) {
        // Keep the engine: GetCaps() still explains why it cannot be read.
    }
    return eng;
}

bool CContextMenu::SelectedEntries(const std::shared_ptr<IArchiveEngine>& eng,
                                    std::vector<ArchiveEntry>& out)
{
    if (!eng || !m_pFolder) return false;
    const std::wstring dir = m_pFolder->GetInternalPath();
    for (auto p : m_pidls) {
        ArchiveEntry e;
        if (ArchiveOps::FindEntry(eng, dir, CPidlMgr::GetName(p), e))
            out.push_back(e);
    }
    return !out.empty();
}

std::wstring CContextMenu::AskForFolder(const wchar_t* title)
{
    wchar_t buf[MAX_PATH] = {};
    BROWSEINFOW bi{ m_hwnd, nullptr, buf, title,
                    BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE };
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return L"";
    SHGetPathFromIDListW(pidl, buf);
    CoTaskMemFree(pidl);
    return buf;
}

// Navigate the window the user is looking at into `pidlRel`, a child of this
// folder. Falls back to opening a new window when there is no site to ask.
bool CContextMenu::BrowseTo(LPCITEMIDLIST pidlRel)
{
    if (!m_pFolder || !pidlRel) return false;

    if (m_pSite)
    {
        IShellBrowser* psb = nullptr;
        if (SUCCEEDED(IUnknown_QueryService(m_pSite, SID_SShellBrowser,
                                            IID_IShellBrowser, (void**)&psb)) && psb)
        {
            HRESULT hr = psb->BrowseObject(pidlRel, SBSP_RELATIVE | SBSP_DEFBROWSER);
            psb->Release();
            if (SUCCEEDED(hr)) return true;
        }
    }

    bool ok = false;
    if (LPITEMIDLIST abs = ILCombine(m_pFolder->GetAbsPidl(), pidlRel))
    {
        ok = SUCCEEDED(SHOpenFolderAndSelectItems(abs, 0, nullptr, 0));
        ILFree(abs);
    }
    return ok;
}

HRESULT CContextMenu::MakeDataObject(REFIID riid, void** ppv)
{
    if (!m_pFolder || m_pidls.empty()) return E_FAIL;
    std::vector<LPCITEMIDLIST> items(m_pidls.begin(), m_pidls.end());
    return CArchiveDataObject::Create(m_pFolder, (UINT)items.size(),
                                      items.data(), riid, ppv);
}

void CContextMenu::NotifyRefresh()
{
    if (!m_pFolder) return;
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSH,
                   m_pFolder->GetAbsPidl(), nullptr);
}

// ─────────────────────────────────────────────────────────
// Command implementations
// ─────────────────────────────────────────────────────────

// Open an entry: a folder opens a window on it, a file is extracted to a
// private temp copy and handed to whatever application owns its type.
void CContextMenu::DoOpenItem()
{
    if (!m_pFolder || m_pidls.empty()) return;

    auto eng = AcquireEngine();
    if (!ArchiveOps::EnsureCanRead(m_hwnd, eng)) return;

    std::vector<ArchiveEntry> sel;
    if (!SelectedEntries(eng, sel)) return;

    WaitCursor wait;
    std::wstring tempDir;
    bool browsed = false;

    for (const auto& e : sel)
    {
        if (e.isDirectory)
        {
            if (browsed) continue;   // one navigation per invocation
            for (auto p : m_pidls)
            {
                if (_wcsicmp(CPidlMgr::GetName(p).c_str(), e.name.c_str()) != 0)
                    continue;
                browsed = true;
                if (!BrowseTo(p))
                    MessageBoxW(m_hwnd,
                        (L"ShellNSE could not open \"" + e.name +
                         L"\" inside the archive.").c_str(),
                        L"ShellNSE", MB_ICONWARNING | MB_OK);
                break;
            }
            continue;
        }

        if (tempDir.empty())
            tempDir = ArchiveOps::MakeTempDir(
                PathFindFileNameW(m_archivePath.c_str()));
        if (tempDir.empty()) return;

        std::wstring onDisk;
        if (!ArchiveOps::ExtractEntry(eng, e, tempDir, &onDisk))
        {
            MessageBoxW(m_hwnd,
                (L"ShellNSE could not extract \"" + e.name +
                 L"\" from the archive.").c_str(),
                L"ShellNSE", MB_ICONERROR | MB_OK);
            continue;
        }

        // The copy is read-only on purpose: edits cannot be written back
        // into the archive, and a silently discarded edit is worse than a
        // "this file is read-only" prompt.
        SetFileAttributesW(onDisk.c_str(), FILE_ATTRIBUTE_READONLY);

        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.fMask  = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
        sei.hwnd   = m_hwnd;
        sei.lpVerb = nullptr;              // the file type's default verb
        sei.lpFile = onDisk.c_str();
        sei.nShow  = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei))
        {
            // No association: let the user pick an application.
            std::wstring args = L"shell32.dll,OpenAs_RunDLL " + onDisk;
            ShellExecuteW(m_hwnd, L"open", L"rundll32.exe", args.c_str(),
                          nullptr, SW_SHOWNORMAL);
        }
    }
}

// Extract: the whole archive when invoked on the file or the view's
// background, just the selection when invoked on items.
void CContextMenu::DoExtract(bool here)
{
    auto eng = AcquireEngine();
    if (!ArchiveOps::EnsureCanRead(m_hwnd, eng)) return;

    std::wstring dest;
    if (here) {
        wchar_t dir[MAX_PATH * 2] = {};
        wcsncpy_s(dir, m_archivePath.c_str(), _TRUNCATE);
        PathRemoveFileSpecW(dir);
        dest = dir;
    } else {
        dest = AskForFolder(L"Select destination folder:");
        if (dest.empty()) return;
    }
    if (dest.empty()) return;

    WaitCursor wait;
    bool ok = true;

    std::vector<ArchiveEntry> sel;
    if (m_mode == ModeItem && SelectedEntries(eng, sel))
    {
        for (const auto& e : sel)
            ok = eng->ExtractFile(e, dest, nullptr) && ok;
    }
    else
    {
        ok = eng->ExtractAll(dest, nullptr);
    }

    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH, dest.c_str(), nullptr);

    if (!ok)
        MessageBoxW(m_hwnd,
            L"Some items could not be extracted.\n\n"
            L"The archive may be damaged, or it may contain encrypted items "
            L"(ShellNSE has no password prompt yet).",
            L"ShellNSE", MB_ICONWARNING | MB_OK);
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
    auto engine = AcquireEngine();
    if (!ArchiveOps::EnsureCanRead(m_hwnd, engine)) return;

    WaitCursor wait;
    bool ok = engine->Test(nullptr);
    MessageBoxW(m_hwnd,
        ok ? L"Archive test: PASSED\nAll files are intact."
           : L"Archive test: FAILED!\nCorruption detected.",
        L"Test Archive", ok ? MB_ICONINFORMATION : MB_ICONERROR);
}

void CContextMenu::DoInfo()
{
    auto engine = AcquireEngine();
    if (!engine) return;

    EngineCaps caps = engine->GetCaps();
    std::wstring msg =
        L"Archive: " + std::wstring(PathFindFileNameW(m_archivePath.c_str())) +
        L"\nFormat: " + engine->GetFormatName();

    if (caps.canExtract)
    {
        wchar_t buf[256];
        swprintf_s(buf, 256, L"\nFiles: %llu\nSize: %s\nPacked: %s",
            (unsigned long long)engine->GetFileCount(),
            engine->GetFormattedSize(engine->GetTotalSize()).c_str(),
            engine->GetFormattedSize(engine->GetPackedSize()).c_str());
        msg += buf;
    }

    msg += L"\n\nEngine: ";
    msg += caps.backendPath.empty() ? L"(none loaded)" : caps.backendPath;
    msg += L"\nCan extract: ";  msg += caps.canExtract ? L"yes" : L"no";
    msg += L"\nCan add files: "; msg += caps.canAdd    ? L"yes" : L"no";
    if (!caps.unavailableReason.empty())
        msg += L"\n\n" + caps.unavailableReason;

    MessageBoxW(m_hwnd, msg.c_str(), L"Archive Info", MB_ICONINFORMATION | MB_OK);
}

// Copy selected entries to the clipboard. The data object extracts lazily,
// so pasting into Explorer produces the real files.
void CContextMenu::DoCopy()
{
    auto eng = AcquireEngine();
    if (!ArchiveOps::EnsureCanRead(m_hwnd, eng)) return;

    IDataObject* pdo = nullptr;
    if (FAILED(MakeDataObject(IID_IDataObject, (void**)&pdo)) || !pdo)
    {
        MessageBoxW(m_hwnd, L"Nothing could be copied from this selection.",
                    L"ShellNSE", MB_ICONWARNING | MB_OK);
        return;
    }

    // NOTE: deliberately no OleFlushClipboard() here. Flushing renders every
    // advertised format up front, which would extract the whole selection
    // immediately AND collapse CFSTR_FILECONTENTS to a single item. OLE keeps
    // a reference to this live object instead, and the DLL stays loaded for
    // as long as the clipboard holds it.
    OleSetClipboard(pdo);
    pdo->Release();
}

// Paste file-system files INTO the archive (needs a writing engine).
void CContextMenu::DoPaste()
{
    auto eng = AcquireEngine();
    if (!ArchiveOps::EnsureCanAdd(m_hwnd, eng)) return;

    IDataObject* pdo = nullptr;
    if (FAILED(OleGetClipboard(&pdo)) || !pdo) return;

    std::vector<std::wstring> roots;
    ArchiveOps::PathsFromDataObject(pdo, roots);
    pdo->Release();
    if (roots.empty()) return;

    std::vector<ArchiveOps::AddItem> items;
    ArchiveOps::ExpandForAdd(roots, items);

    WaitCursor wait;
    const std::wstring dir = m_pFolder ? m_pFolder->GetInternalPath() : L"";
    bool ok = true;
    for (const auto& it : items)
        ok = eng->AddFile(it.src, ArchiveOps::TargetDirFor(dir, it), nullptr) && ok;

    NotifyRefresh();
    if (!ok)
        MessageBoxW(m_hwnd, L"Some files could not be added to the archive.",
                    L"ShellNSE", MB_ICONWARNING | MB_OK);
}

void CContextMenu::DoRefresh()
{
    NotifyRefresh();
}

// Properties for one entry inside the archive.
void CContextMenu::DoProperties()
{
    auto eng = AcquireEngine();
    if (!eng) return;

    std::vector<ArchiveEntry> sel;
    if (!SelectedEntries(eng, sel)) return;
    const ArchiveEntry& e = sel.front();

    auto fmt = [&](uint64_t v) { return eng->GetFormattedSize(v); };

    std::wstring msg = e.name;
    msg += e.isDirectory ? L"\n\nType: Folder inside archive"
                         : L"\n\nType: File inside archive";
    msg += L"\nLocation: " + std::wstring(PathFindFileNameW(m_archivePath.c_str()));
    std::wstring inner = ArchiveOps::ToWin32(e.fullPath);
    msg += L"\nPath in archive: " + (inner.empty() ? std::wstring(L"\\") : inner);

    if (!e.isDirectory)
    {
        wchar_t buf[256];
        double ratio = e.uncompressedSize
            ? 100.0 * (1.0 - (double)e.compressedSize / (double)e.uncompressedSize)
            : 0.0;
        swprintf_s(buf, 256,
            L"\n\nSize: %s\nPacked: %s\nRatio: %.0f%%\nCRC-32: %08X",
            fmt(e.uncompressedSize).c_str(), fmt(e.compressedSize).c_str(),
            ratio, e.crc32);
        msg += buf;
        if (!e.compressionMethod.empty())
            msg += L"\nMethod: " + e.compressionMethod;
    }

    SYSTEMTIME st{}; FILETIME lft{};
    if (FileTimeToLocalFileTime(&e.modifiedTime, &lft) &&
        FileTimeToSystemTime(&lft, &st) && st.wYear > 1601)
    {
        wchar_t when[64];
        swprintf_s(when, 64, L"\nModified: %04d-%02d-%02d %02d:%02d:%02d",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        msg += when;
    }
    if (e.isEncrypted) msg += L"\n\nThis item is encrypted.";

    MessageBoxW(m_hwnd, msg.c_str(), L"Properties", MB_ICONINFORMATION | MB_OK);
}

void CContextMenu::DoSettings()
{
    CSettingsDialog dlg;
    dlg.Show(m_hwnd);
}

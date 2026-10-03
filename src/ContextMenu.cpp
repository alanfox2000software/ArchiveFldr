// ContextMenu.cpp
#include "stdafx.h"
#include "ContextMenu.h"
#include "BrowseTo.h"
#include "ShellFolder.h"
#include "DataObject.h"
#include "ArchiveEngine.h"
#include "ArchiveOps.h"
#include "ThirdParty.h"
#include "Settings.h"

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
    { L"test",        "test",        L"Test archive integrity"                },
    { L"info",        "info",        L"View archive information"              },
    { L"copy",        "copy",        L"Copy to the clipboard"                 },
    { L"paste",       "paste",       L"Add the clipboard's files here"        },
    { L"refresh",     "refresh",     L"Refresh this view"                     },
    { L"properties",  "properties",  L"Show properties"                       },
    { L"settings",    "settings",    L"Open ArchiveFldr settings"             },
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
    if (IsEqualIID(riid,IID_IObjectWithSite))
    { *ppv=static_cast<IObjectWithSite*>(this); AddRef(); return S_OK; }
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CContextMenu::AddRef()
    { return InterlockedIncrement(&m_cRef); }
STDMETHODIMP_(ULONG) CContextMenu::Release()
    { ULONG n=InterlockedDecrement(&m_cRef); if(!n) delete this; return n; }

// ── IContextMenu::QueryContextMenu ───────────────────────
STDMETHODIMP CContextMenu::QueryContextMenu(
    HMENU hMenu, UINT indexMenu, UINT idCmdFirst,
    UINT idCmdLast, UINT uFlags)
{
    static_assert(ARRAYSIZE(kVerbs) == CMD_COUNT,
                  "verb table and Cmd enum are out of sync");

    m_cmdBase = idCmdFirst;

    // CMF_DEFAULTONLY = "tell me the one command a double-click should run".
    //
    // Every item in this view needs an answer here, FOLDERS INCLUDED. The
    // default view has no navigation of its own for a namespace extension:
    // activating an item means invoking the default verb of the menu this
    // call returns. Answering nothing (an earlier attempt at this bug) is
    // why double-clicking a folder did nothing at all — the view had no
    // command to run. DoOpenItem() browses folders and extracts-and-runs
    // files, which is exactly what the shell does for a file system folder.
    if (uFlags & CMF_DEFAULTONLY)
    {
        if (m_mode != ModeItem || m_pidls.empty())
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
        if (idCmdFirst + (UINT)CMD_OPEN_ITEM > idCmdLast)
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);

        InsertMenuW(hMenu, indexMenu, MF_BYPOSITION | MF_STRING,
                    idCmdFirst + CMD_OPEN_ITEM, L"&Open");
        SetMenuDefaultItem(hMenu, idCmdFirst + CMD_OPEN_ITEM, FALSE);
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, CMD_OPEN_ITEM + 1);
    }

    UINT pos  = indexMenu;
    UINT used = 0;

    auto addItem = [&](UINT cmd, const wchar_t* text, bool enabled = true) {
        // idCmdFirst..idCmdLast is the range the shell lends us, and it
        // is a promise, not a hint: an id past the end belongs to
        // another handler, which would then run its command when the
        // user picked ours. Silently drop anything that will not fit.
        if (cmd > idCmdLast - idCmdFirst) return;

        MENUITEMINFOW mi{sizeof(mi), MIIM_STRING | MIIM_ID | MIIM_STATE};
        mi.wID        = idCmdFirst + cmd;
        mi.dwTypeData = (LPWSTR)text;
        mi.fState     = enabled ? MFS_ENABLED : MFS_GRAYED;
        InsertMenuItemW(hMenu, pos++, TRUE, &mi);
        if (cmd + 1 > used) used = cmd + 1;
    };
    auto addSep = [&] {
        InsertMenuW(hMenu, pos++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    };

    switch (m_mode)
    {
    // ── Items inside an archive ──────────────────────────
    case ModeItem:
    {
        const bool single = (m_pidls.size() == 1);
        addItem(CMD_OPEN_ITEM, single ? L"&Open" : L"&Open items");
        SetMenuDefaultItem(hMenu, idCmdFirst + CMD_OPEN_ITEM, FALSE);
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
    default:
    {
        addItem(CMD_EXTRACT,     L"E&xtract all...");
        addItem(CMD_EXTRACTHERE, L"Extract all &here");
        addSep();
        addItem(CMD_PASTE,       L"&Paste", ArchiveOps::ClipboardHasFiles());
        addItem(CMD_REFRESH,     L"&Refresh");
        addSep();
        addItem(CMD_TEST,        L"&Test archive");
        addItem(CMD_INFO,        L"Archive &info...");
        addItem(CMD_SETTINGS,    L"ArchiveFldr &settings...");
        break;
    }
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
        // No verb at all: run the default command for this menu. Only
        // the item menu has one; DoOpenItem() is a no-op without a
        // selection, so this stays harmless for the background menu.
        cmd = (UINT)CMD_OPEN_ITEM;
    }

    if (!verb.empty())
    {
        for (UINT i = 0; i < CMD_COUNT; ++i)
            if (_wcsicmp(verb.c_str(), kVerbs[i].verbW) == 0) { cmd = i; break; }
    }

    if (cmd >= CMD_COUNT) return E_INVALIDARG;

    switch (cmd) {
    case CMD_OPEN_ITEM:     DoOpenItem();     break;
    case CMD_EXTRACT:       DoExtract(false); break;
    case CMD_EXTRACTHERE:   DoExtract(true);  break;
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

    // GCS_VALIDATE passes no buffer at all — it is a question, not a
    // request for a string — and nothing stops a caller passing a null
    // one with any of the others. Answer the question before touching
    // anything, so a validate does not walk into a wcsncpy_s on null.
    if (uType == GCS_VALIDATEA || uType == GCS_VALIDATEW)
        return S_OK;                     // idCmd is in range: it is valid
    if (!pszName || cchMax == 0) return E_INVALIDARG;

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

// Reuse the folder's already-open engine; fall back to opening our own
// from the archive path when the folder has none to lend.
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

// Preselect a starting folder. SHBrowseForFolder has no field for it:
// the only way in is to answer BFFM_INITIALIZED on the callback.
static int CALLBACK BrowseInitProc(HWND hwnd, UINT msg, LPARAM, LPARAM lpData)
{
    if (msg == BFFM_INITIALIZED && lpData)
        SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, lpData);
    return 0;
}

std::wstring CContextMenu::AskForFolder(const wchar_t* title)
{
    Settings& cfg = Settings::Get();

    // Where to start: the folder remembered from last time, else the one
    // configured as the default, else wherever the shell would open.
    const std::wstring start = cfg.defaultExtractPath;

    wchar_t buf[MAX_PATH] = {};
    BROWSEINFOW bi{ m_hwnd, nullptr, buf, title,
                    BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE };
    if (!start.empty() && PathFileExistsW(start.c_str()))
    {
        bi.lpfn   = BrowseInitProc;
        bi.lParam = (LPARAM)start.c_str();
    }

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return L"";
    SHGetPathFromIDListW(pidl, buf);
    CoTaskMemFree(pidl);

    // Remember it for next time, if asked to.
    if (cfg.rememberLastPath && *buf)
    {
        cfg.defaultExtractPath = buf;
        cfg.Save();
    }
    return buf;
}

// Ask one site object for the browser that hosts this view and navigate it.
static bool BrowseWithSite(IUnknown* punk, LPCITEMIDLIST pidlRel,
                           LPCITEMIDLIST pidlAbs)
{
    if (!punk) return false;

    const GUID* kServices[] = { &SID_SShellBrowser, &SID_STopLevelBrowser };
    for (const GUID* sid : kServices)
    {
        IShellBrowser* psb = nullptr;
        if (FAILED(IUnknown_QueryService(punk, *sid, IID_IShellBrowser,
                                         (void**)&psb)) || !psb)
            continue;

        // pidlRel is null when the caller only has an absolute PIDL. Handing
        // one to SBSP_RELATIVE asks the browser to append it to the folder
        // it is already showing, which navigates somewhere that does not
        // exist — and can report success doing it.
        HRESULT hr = E_FAIL;
        if (pidlRel)
            hr = psb->BrowseObject(pidlRel, SBSP_RELATIVE | SBSP_DEFBROWSER);
        if (FAILED(hr) && pidlAbs)      // not the browser's current folder
            hr = psb->BrowseObject(pidlAbs, SBSP_ABSOLUTE | SBSP_DEFBROWSER);
        psb->Release();
        if (SUCCEEDED(hr)) return true;
    }
    return false;
}

// Same, for a window: the default view answers WM_GETISHELLBROWSER with its
// IShellBrowser (borrowed, not ref-counted). Only SHELLDLL_DefView windows
// are asked, because the message number is in the private WM_USER range.
static bool BrowseWithWindow(HWND hwnd, LPCITEMIDLIST pidlRel,
                             LPCITEMIDLIST pidlAbs)
{
    const UINT kGetIShellBrowser = WM_USER + 7;

    for (HWND h = hwnd; h; h = GetParent(h))
    {
        HWND candidates[2] = {
            h, FindWindowExW(h, nullptr, L"SHELLDLL_DefView", nullptr) };

        for (HWND c : candidates)
        {
            wchar_t cls[64] = {};
            if (!c || !GetClassNameW(c, cls, ARRAYSIZE(cls))) continue;
            if (_wcsicmp(cls, L"SHELLDLL_DefView") != 0) continue;

            auto* psb = (IShellBrowser*)SendMessageW(c, kGetIShellBrowser, 0, 0);
            if (!psb) continue;

            HRESULT hr = E_FAIL;
            if (pidlRel)
                hr = psb->BrowseObject(pidlRel, SBSP_RELATIVE | SBSP_DEFBROWSER);
            if (FAILED(hr) && pidlAbs)
                hr = psb->BrowseObject(pidlAbs, SBSP_ABSOLUTE | SBSP_DEFBROWSER);
            if (SUCCEEDED(hr)) return true;
        }
    }
    return false;
}

// Navigate the window the user is looking at to an absolute PIDL, in place.
static bool BrowseAbsoluteInPlace(IUnknown* site, HWND hwnd,
                                  LPCITEMIDLIST pidlAbs)
{
    if (BrowseWithSite(site, nullptr, pidlAbs)) return true;
    if (hwnd && BrowseWithWindow(hwnd, nullptr, pidlAbs)) return true;

    IUnknown* punkThread = nullptr;
    if (SUCCEEDED(SHGetThreadRef(&punkThread)) && punkThread)
    {
        const bool ok = BrowseWithSite(punkThread, nullptr, pidlAbs);
        punkThread->Release();
        if (ok) return true;
    }
    return false;
}

// Navigate the window the user is looking at into `pidlRel`, a child of this
// folder. Opening a separate window is the last resort on purpose: it has to
// re-resolve our PIDL from the desktop down, which only works when the
// archive's file association junction is live.
bool CContextMenu::BrowseTo(LPCITEMIDLIST pidlRel)
{
    if (!m_pFolder || !pidlRel) return false;

    LPITEMIDLIST abs = ILCombine(m_pFolder->GetAbsPidl(), pidlRel);

    // 1. The site the view handed us through IObjectWithSite.
    bool ok = BrowseWithSite(m_pSite, pidlRel, abs);

    // 2. The view window itself, for menus invoked without a site.
    if (!ok && m_hwnd)
        ok = BrowseWithWindow(m_hwnd, pidlRel, abs);

    // 3. Explorer keeps a reference to the browser on its UI thread.
    if (!ok)
    {
        IUnknown* punkThread = nullptr;
        if (SUCCEEDED(SHGetThreadRef(&punkThread)) && punkThread)
        {
            ok = BrowseWithSite(punkThread, pidlRel, abs);
            punkThread->Release();
        }
    }

    // 4. Give up on navigating in place and open a window on the item.
    if (!ok && abs)
        ok = ShellBrowseToFolder(m_hwnd, abs);

    if (abs) ILFree(abs);
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

    // ── Folders first, and deliberately before the engine is touched:
    // navigating needs nothing but the PIDL, so entering a sub-folder keeps
    // working even when the backend DLL is missing or the archive is
    // unreadable. Only one navigation per activation.
    bool browsed = false, anyFile = false;
    for (auto p : m_pidls)
    {
        if (!CPidlMgr::IsDir(p)) { anyFile = true; continue; }
        if (browsed) continue;
        browsed = true;
        if (!BrowseTo(p))
            MessageBoxW(m_hwnd,
                (L"ArchiveFldr could not open \"" + CPidlMgr::GetName(p) +
                 L"\" inside the archive.").c_str(),
                L"ArchiveFldr", MB_ICONWARNING | MB_OK);
    }
    if (!anyFile) return;

    // ── Files: extract a copy to a temp folder and launch it.
    auto eng = AcquireEngine();
    if (!ArchiveOps::EnsureCanRead(m_hwnd, eng)) return;

    std::vector<ArchiveEntry> sel;
    if (!SelectedEntries(eng, sel)) return;

    WaitCursor wait;
    std::wstring tempDir;

    for (const auto& e : sel)
    {
        if (e.isDirectory) continue;            // handled above

        if (tempDir.empty())
            tempDir = ArchiveOps::MakeTempDir(
                PathFindFileNameW(m_archivePath.c_str()));
        if (tempDir.empty()) return;

        std::wstring onDisk;
        if (!ArchiveOps::ExtractEntry(eng, e, tempDir, &onDisk))
        {
            MessageBoxW(m_hwnd,
                (L"ArchiveFldr could not extract \"" + e.name +
                 L"\" from the archive.").c_str(),
                L"ArchiveFldr", MB_ICONERROR | MB_OK);
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

// Extract: the whole archive when invoked on the view's background,
// just the selection when invoked on items.
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
        const Settings& cfg = Settings::Get();
        // "Always ask" off plus a configured folder means go straight
        // there; otherwise there would be no way to use the setting.
        if (!cfg.promptForPath && !cfg.defaultExtractPath.empty() &&
            PathFileExistsW(cfg.defaultExtractPath.c_str()))
            dest = cfg.defaultExtractPath;
        else
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
            L"(ArchiveFldr has no password prompt yet).",
            L"ArchiveFldr", MB_ICONWARNING | MB_OK);
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
        // A total of zero against a non-empty archive means the format
        // never recorded one, not that the contents are empty.
        const uint64_t total  = engine->GetTotalSize();
        const uint64_t packed = engine->GetPackedSize();
        const std::wstring totalText =
            (total == 0 && packed > 0) ? std::wstring(L"not recorded by this format")
                                       : engine->GetFormattedSize(total);
        wchar_t buf[320];
        swprintf_s(buf, 320, L"\nFiles: %llu\nSize: %s\nPacked: %s",
            (unsigned long long)engine->GetFileCount(),
            totalText.c_str(),
            engine->GetFormattedSize(packed).c_str());
        msg += buf;
    }

    msg += L"\n\nEngine: ";
    msg += caps.backendPath.empty() ? L"(none loaded)" : caps.backendPath;
    msg += L"\nCan extract: ";  msg += caps.canExtract ? L"yes" : L"no";
    msg += L"\nCan add files: "; msg += caps.canAdd    ? L"yes" : L"no";
    if (!caps.unavailableReason.empty())
        msg += L"\n\n" + caps.unavailableReason;

    // Which binary is actually loaded. Explorer caches shell extensions
    // aggressively, so after a rebuild this is the quickest way to tell
    // whether the DLL under test is the one that just got built.
    msg += L"\n\nArchiveFldr build: " NSE_WIDE(__DATE__) L" " NSE_WIDE(__TIME__);
    msg += (sizeof(void*) == 8) ? L" (64-bit)" : L" (32-bit)";

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
                    L"ArchiveFldr", MB_ICONWARNING | MB_OK);
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
                    L"ArchiveFldr", MB_ICONWARNING | MB_OK);
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
        // "Not recorded" rather than a made-up zero: several formats
        // (bzip2, raw Brotli, lz4 without --content-size) simply do not
        // store the original size.
        const std::wstring sizeText =
            e.sizeKnown ? fmt(e.uncompressedSize)
                        : std::wstring(L"not recorded by this format");
        const std::wstring ratioText =
            e.sizeKnown ? ArchiveOps::FormatRatio(e.uncompressedSize,
                                                  e.compressedSize)
                        : std::wstring(L"\u2014");

        wchar_t buf[320];
        swprintf_s(buf, 320,
            L"\n\nSize: %s\nPacked: %s%s\nRatio: %s",
            sizeText.c_str(),
            fmt(e.compressedSize).c_str(),
            e.packedIsShared ? L" (share of a solid block)" : L"",
            ratioText.c_str());
        msg += buf;

        // Only claim a checksum the format really stores. Tar has none at
        // all; WIM uses SHA-1 instead, so show that when we have it.
        if (e.hasCrc)
        {
            swprintf_s(buf, 320, L"\nCRC-32: %08X", e.crc32);
            msg += buf;
        }
        else if (!e.sha1.empty())
        {
            msg += L"\nSHA-1: " + e.sha1;
        }
        else
        {
            msg += L"\nChecksum: not stored by this format";
        }
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
    // The settings UI is its own program now — a shell extension has no
    // business carrying a six-page dialog into every process that touches
    // a context menu. It sits next to this DLL.
    const std::wstring dir = ThirdParty::ModuleDir();
    if (dir.empty())
    {
        MessageBoxW(m_hwnd, L"ArchiveFldr could not locate its own folder.",
                    L"ArchiveFldr", MB_ICONERROR | MB_OK);
        return;
    }
    // ArchiveFldrSetting.exe, plainly named: each platform builds into
    // its own folder (<Config>\x32, <Config>\x64), so the copy sitting
    // beside this DLL is already the matching bitness and there is
    // nothing for a suffix to disambiguate.
    //
    // The two tagged spellings are still accepted so that an install
    // upgraded from a build that produced them keeps working — looked
    // for after the plain name, never instead of it.
    const std::wstring preferred = dir + L"\\ArchiveFldrSetting.exe";
    std::wstring exeStr = preferred;
    if (!PathFileExistsW(exeStr.c_str()))
    {
        const std::wstring legacy[] = {
            dir + L"\\ArchiveFldrSetting." + ThirdParty::BitnessTag() + L".exe",
            dir + L"\\ArchiveFldrSetting.64.exe",
            dir + L"\\ArchiveFldrSetting.32.exe",
        };
        for (const auto& candidate : legacy)
            if (PathFileExistsW(candidate.c_str())) { exeStr = candidate; break; }
    }
    const wchar_t* exe = exeStr.c_str();

    if (!PathFileExistsW(exe))
    {
        MessageBoxW(m_hwnd,
            (std::wstring(
                L"The settings program is missing. It is built alongside "
                L"ArchiveFldr and belongs in the same folder:\n\n") +
             preferred).c_str(),
            L"ArchiveFldr", MB_ICONWARNING | MB_OK);
        return;
    }

    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask  = SEE_MASK_FLAG_NO_UI;
    sei.hwnd   = m_hwnd;
    sei.lpVerb = L"open";
    sei.lpFile = exe;
    sei.nShow  = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei))
        MessageBoxW(m_hwnd, L"The settings program could not be started.",
                    L"ArchiveFldr", MB_ICONERROR | MB_OK);
}

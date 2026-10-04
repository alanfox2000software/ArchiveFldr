// ContextMenu.cpp
#include "stdafx.h"
#include "ContextMenu.h"
#include "BrowseTo.h"
#include "ShellFolder.h"
#include "DataObject.h"
#include "ArchiveEngine.h"
#include "ArchiveOps.h"
#include "SevenZipEngine.h"   // Is7zEngineAvailable() / Get7zEnginePath()
#include "ArchiveWriter.h"
#include "ArchiveSecurity.h"
#include "AddToArchiveDialog.h"
#include "ThirdParty.h"
#include "Settings.h"
#include "Formats.h"
#include "GUIDs.h"
#include "Lang.h"
#include "../res/resource.h"

// ─────────────────────────────────────────────────────────
// Verb table — ONE source of truth for command id ↔ verb ↔ help text.
// Order must match the Cmd enum in ContextMenu.h.
// ─────────────────────────────────────────────────────────
namespace {

// Language ids for the menu captions. The same ids back the item list
// on the settings page, so what the user ticks there is labelled with
// the exact text the menu will show. See Lang\en.txt.
enum : UINT {
    LNG_CTX_OPEN     = 2000,
    LNG_CTX_EXTRACT  = 2001,
    LNG_CTX_EXTHERE  = 2002,
    LNG_CTX_TEST     = 2003,
    LNG_CTX_ADD      = 2004,
    LNG_CTX_ADDHERE  = 2005,   // carries one %s: the archive name
    LNG_CTX_EMAIL    = 2006,
    LNG_CTX_INFO     = 2007,
    LNG_CTX_SETTINGS = 2008,
};

// Shorthand for a menu caption: the language file's text, or the
// English baked in right here when it has none.
std::wstring CtxText(UINT id, const wchar_t* fallback)
{
    return Lang::Str(id, fallback);
}

// The menu bitmap for "Icons in context menu".
//
// MIIM_BITMAP wants an HBITMAP, not an HICON, and a menu is drawn over
// whatever colour the theme picked — so the icon has to keep its alpha.
// That means a 32-bit top-down DIB section cleared to zero, with
// DrawIconEx compositing the icon's own alpha into it. Built once and
// kept: a context menu handler is created and destroyed on every
// right-click, and re-rasterising each time would be wasteful.
HBITMAP MenuIconBitmap()
{
    static HBITMAP cached = nullptr;
    static bool    tried  = false;
    if (tried) return cached;
    tried = true;

    const int cx = GetSystemMetrics(SM_CXSMICON);
    const int cy = GetSystemMetrics(SM_CYSMICON);

    HICON ico = (HICON)LoadImageW(g_hDllInstance,
                                  MAKEINTRESOURCEW(IDI_ARCHIVEFLDR),
                                  IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    if (!ico) return nullptr;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = cx;
    bi.bmiHeader.biHeight      = -cy;          // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void*   bits = nullptr;
    HDC     screen = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp)
    {
        HDC     dc  = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(dc, bmp);
        DrawIconEx(dc, 0, 0, ico, cx, cy, 0, nullptr, DI_NORMAL);
        SelectObject(dc, old);
        DeleteDC(dc);
    }
    ReleaseDC(nullptr, screen);
    DestroyIcon(ico);

    cached = bmp;
    return cached;
}

struct VerbDef {
    const wchar_t* verbW;
    const char*    verbA;
    const wchar_t* help;
};

const VerbDef kVerbs[] = {
    { L"open",        "open",        L"Open this item"                        },
    { L"extract",     "extract",     L"Extract to a folder"                   },
    { L"extracthere", "extracthere", L"Extract here"                          },
    { L"add",         "add",         L"Compress into a new archive"           },
    { L"compresshere","compresshere",L"Compress using the saved defaults"     },
    { L"email",       "email",       L"Compress and send by e-mail"           },
    { L"openshell",   "openshell",   L"Browse this archive in Explorer"       },
    { L"test",        "test",        L"Test archive integrity"                },
    { L"info",        "info",        L"View archive information"              },
    { L"copy",        "copy",        L"Copy to the clipboard"                 },
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

    m_paths.clear();
    m_archivePath.clear();

    FORMATETC fe{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (FAILED(pdtobj->GetData(&fe, &sm))) return E_FAIL;

    if (HDROP hDrop = (HDROP)GlobalLock(sm.hGlobal))
    {
        // Keep the whole selection. The handler is registered on every
        // file type now, so "the first file" is no longer a useful
        // summary of what the user picked — compressing acts on all of it.
        const UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        m_paths.reserve(count);
        for (UINT i = 0; i < count; ++i)
        {
            const UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
            if (!len) continue;
            std::wstring p(len + 1, L'\0');
            if (DragQueryFileW(hDrop, i, &p[0], len + 1))
            {
                p.resize(wcslen(p.c_str()));
                if (!p.empty()) m_paths.push_back(std::move(p));
            }
        }
        GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);

    if (m_paths.empty()) return E_FAIL;
    m_archivePath = m_paths.front();

    // One archive selected: the full archive menu. Anything else — a
    // folder, an ordinary file, several things at once — only gets the
    // commands that make sense, which is compression.
    const bool singleArchive =
        m_paths.size() == 1 &&
        !(GetFileAttributesW(m_archivePath.c_str()) & FILE_ATTRIBUTE_DIRECTORY) &&
        Formats::IsArchiveExtension(PathFindExtensionW(m_archivePath.c_str()));

    m_mode = singleArchive ? ModeArchiveFile : ModePlainFile;
    return S_OK;
}

// ── IContextMenu::QueryContextMenu ───────────────────────
STDMETHODIMP CContextMenu::QueryContextMenu(
    HMENU hMenu, UINT indexMenu, UINT idCmdFirst,
    UINT idCmdLast, UINT uFlags)
{
    static_assert(ARRAYSIZE(kVerbs) == CMD_COUNT,
                  "verb table and Cmd enum are out of sync");

    m_cmdBase = idCmdFirst;

    // Explorer keeps shell extensions loaded, sometimes for the whole
    // login session. Take a fresh immutable registry snapshot for every
    // menu so Apply in ArchiveFldrSetting is visible on the next right-click.
    const ContextMenuPrefs menu = Settings::ReadContextMenuPrefs();

    // The ArchiveFldr page controls Explorer-level integration only.
    // Menus on items and background *inside an opened archive* belong to
    // the namespace folder and must remain available: its default Open
    // command is also how double-click navigation works.
    const bool explorerMenu =
        m_mode == ModeArchiveFile || m_mode == ModePlainFile;
    if (explorerMenu && (!menu.show || !menu.enabledHere))
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);

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

    // The shell loads this DLL into Explorer once and keeps it, so the
    // language file is read on the first right-click and then reused.
    // Function-local static initialisation serialises simultaneous menu
    // requests, avoiding two threads clearing/refilling Lang's table.
    static const bool langLoaded = [] {
        Lang::Load(Settings::Get().language);
        return true;
    }();
    (void)langLoaded;

    // "Add to <name>" names its own result. Built here because both the
    // archive-file and plain-file menus show it.
    if (m_mode == ModeArchiveFile || m_mode == ModePlainFile)
    {
        const std::wstring ext = L"." + menu.defaultFormat;
        const std::wstring out = ArchiveWriter::SuggestOutputPath(m_paths, ext);
        m_quickName = Lang::Format1(LNG_CTX_ADDHERE, L"Add to \"%s\"",
                                    out.empty()
                                        ? (L"archive" + ext)
                                        : std::wstring(PathFindFileNameW(out.c_str())));
    }

    // The "collect everything under one ArchiveFldr sub-menu" preference only
    // applies to the crowded file menu in a normal Explorer folder.
    m_useSubMenu = menu.useSubMenu &&
                   (m_mode == ModeArchiveFile || m_mode == ModePlainFile);

    HMENU hTarget = m_useSubMenu ? CreatePopupMenu() : hMenu;
    UINT  pos     = m_useSubMenu ? 0 : indexMenu;
    UINT  used    = 0;
    bool  haveItems = false;
    bool  separatorPending = false;

    // One bitmap shared by every entry, created on first use and kept
    // for the life of the process. Null when the user turned icons off,
    // in which case MIIM_BITMAP is simply not requested.
    HBITMAP hIcon = menu.menuIcons ? MenuIconBitmap() : nullptr;

    auto addItem = [&](UINT cmd, const wchar_t* text, bool enabled = true) {
        // idCmdFirst..idCmdLast is the range the shell lends us, and it
        // is a promise, not a hint: an id past the end belongs to
        // another handler, which would then run its command when the
        // user picked ours. Silently drop anything that will not fit.
        if (cmd > idCmdLast - idCmdFirst) return;

        if (separatorPending && haveItems)
            InsertMenuW(hTarget, pos++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        separatorPending = false;

        MENUITEMINFOW mi{sizeof(mi), MIIM_STRING | MIIM_ID | MIIM_STATE};
        mi.wID        = idCmdFirst + cmd;
        mi.dwTypeData = (LPWSTR)text;
        mi.fState     = enabled ? MFS_ENABLED : MFS_GRAYED;
        if (hIcon) { mi.fMask |= MIIM_BITMAP; mi.hbmpItem = hIcon; }
        if (InsertMenuItemW(hTarget, pos++, TRUE, &mi))
        {
            haveItems = true;
            if (cmd + 1 > used) used = cmd + 1;
        }
    };
    auto addSep = [&] {
        if (haveItems) separatorPending = true;
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
        addSep();
        addItem(CMD_REFRESH,     L"&Refresh");
        addSep();
        addItem(CMD_TEST,        L"&Test archive");
        addItem(CMD_INFO,        L"Archive &info...");
        addItem(CMD_SETTINGS,    L"ArchiveFldr &settings...");
        break;
    }

    // ── Ordinary files and folders ───────────────────────
    // The handler is registered on every file type, so this runs on most
    // right-clicks in Explorer. Only compression applies.
    case ModePlainFile:
    {
        if (menu.addToArchive)  addItem(CMD_ADD, CtxText(LNG_CTX_ADD, L"Add to archive...").c_str());
        if (menu.compressHere)  addItem(CMD_COMPRESS_HERE, m_quickName.c_str());
        if (menu.compressEmail) addItem(CMD_COMPRESS_EMAIL, CtxText(LNG_CTX_EMAIL, L"Compress and email...").c_str());
        if (menu.settings) { addSep(); addItem(CMD_SETTINGS, CtxText(LNG_CTX_SETTINGS, L"ArchiveFldr settings...").c_str()); }
        break;
    }

    // ── An archive file in a normal Explorer folder ──────
    case ModeArchiveFile:
    default:
    {
        if (menu.extract)       addItem(CMD_EXTRACT,        CtxText(LNG_CTX_EXTRACT, L"Extract files...").c_str());
        if (menu.extractHere)   addItem(CMD_EXTRACTHERE,    CtxText(LNG_CTX_EXTHERE, L"Extract Here").c_str());
        addSep();
        if (menu.addToArchive)  addItem(CMD_ADD,            CtxText(LNG_CTX_ADD, L"Add to archive...").c_str());
        if (menu.compressHere)  addItem(CMD_COMPRESS_HERE,  m_quickName.c_str());
        if (menu.compressEmail) addItem(CMD_COMPRESS_EMAIL, CtxText(LNG_CTX_EMAIL, L"Compress and email...").c_str());
        addSep();
        if (menu.openInShell)   addItem(CMD_OPEN_SHELL,     CtxText(LNG_CTX_OPEN, L"Open archive").c_str());
        if (menu.testArchive)   addItem(CMD_TEST,           CtxText(LNG_CTX_TEST, L"Test archive").c_str());
        if (menu.archiveInfo)   addItem(CMD_INFO,           CtxText(LNG_CTX_INFO, L"Archive information").c_str());
        if (menu.settings)      addItem(CMD_SETTINGS,       CtxText(LNG_CTX_SETTINGS, L"ArchiveFldr settings...").c_str());
        break;
    }
    }

    if (m_useSubMenu)
    {
        if (haveItems)
        {
            MENUITEMINFOW mi{sizeof(mi),MIIM_STRING|MIIM_SUBMENU|MIIM_STATE};
            mi.hSubMenu   = hTarget;
            mi.dwTypeData = (LPWSTR)menu.subMenuTitle.c_str();
            mi.fState     = MFS_ENABLED;
            if (hIcon) { mi.fMask |= MIIM_BITMAP; mi.hbmpItem = hIcon; }
            InsertMenuItemW(hMenu, indexMenu, TRUE, &mi);
        }
        else
        {
            DestroyMenu(hTarget);
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
    case CMD_ADD:           DoCompress(false);break;
    case CMD_COMPRESS_HERE: DoCompress(true); break;
    case CMD_COMPRESS_EMAIL:DoCompressEmail();break;
    case CMD_OPEN_SHELL:    DoOpenShell();    break;
    case CMD_TEST:          DoTest();         break;
    case CMD_INFO:          DoInfo();         break;
    case CMD_COPY:          DoCopy();         break;
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

// The Desktop exposes an IShellBrowser too, but it is not a reusable folder
// window. Its BrowseObject implementation can accept an absolute PIDL and do
// nothing, which prevents the new-window fallback from ever running.
static bool IsDesktopBrowser(IShellBrowser* psb)
{
    if (!psb) return false;

    HWND browserWindow = nullptr;
    if (SUCCEEDED(psb->GetWindow(&browserWindow)) && browserWindow)
    {
        HWND root = GetAncestor(browserWindow, GA_ROOT);
        wchar_t cls[64] = {};
        if (root && GetClassNameW(root, cls, ARRAYSIZE(cls)) &&
            (_wcsicmp(cls, L"Progman") == 0 ||
             _wcsicmp(cls, L"WorkerW") == 0))
            return true;
    }

    // Window ownership differs across Windows releases, so also compare the
    // active view's folder with the Desktop PIDL.
    IShellView* view = nullptr;
    if (FAILED(psb->QueryActiveShellView(&view)) || !view) return false;

    IFolderView* folderView = nullptr;
    HRESULT hr = view->QueryInterface(IID_IFolderView, (void**)&folderView);
    view->Release();
    if (FAILED(hr) || !folderView) return false;

    IPersistFolder2* folder = nullptr;
    hr = folderView->GetFolder(IID_IPersistFolder2, (void**)&folder);
    folderView->Release();
    if (FAILED(hr) || !folder) return false;

    LPITEMIDLIST current = nullptr;
    hr = folder->GetCurFolder(&current);
    folder->Release();
    if (FAILED(hr) || !current) return false;

    LPITEMIDLIST desktop = nullptr;
    const bool isDesktop =
        SUCCEEDED(SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOP, &desktop)) &&
        desktop && ILIsEqual(current, desktop);
    ILFree(current);
    if (desktop) ILFree(desktop);
    return isDesktop;
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

        // The Desktop's browser reports success without opening a folder.
        // Decline it so the caller reaches ShellBrowseToFolder and creates a
        // real Explorer window instead.
        if (IsDesktopBrowser(psb))
        {
            psb->Release();
            continue;
        }

        // pidlRel is null when the caller only has an absolute PIDL. Handing
        // one to SBSP_RELATIVE asks the browser to append it to the folder
        // it is already showing, which navigates somewhere that does not
        // exist — and can report success doing it. Explicitly request the
        // current browser as well: SBSP_DEFBROWSER is zero and allowed the
        // host to accept the request without navigating this window.
        HRESULT hr = E_FAIL;
        if (pidlRel)
            hr = psb->BrowseObject(pidlRel, SBSP_RELATIVE | SBSP_SAMEBROWSER);
        if (FAILED(hr) && pidlAbs)      // not the browser's current folder
            hr = psb->BrowseObject(pidlAbs, SBSP_ABSOLUTE | SBSP_SAMEBROWSER);
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
            if (!psb || IsDesktopBrowser(psb)) continue;

            HRESULT hr = E_FAIL;
            if (pidlRel)
                hr = psb->BrowseObject(pidlRel,
                                       SBSP_RELATIVE | SBSP_SAMEBROWSER);
            if (FAILED(hr) && pidlAbs)
                hr = psb->BrowseObject(pidlAbs,
                                       SBSP_ABSOLUTE | SBSP_SAMEBROWSER);
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
    return CArchiveDataObject::Create(m_pFolder, m_hwnd, (UINT)items.size(),
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
        if (!ArchiveOps::ExtractEntryPrompting(m_hwnd, eng, e, tempDir, &onDisk))
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

// Extract: the whole archive when invoked on the file or the view's
// background, just the selection when invoked on items.
void CContextMenu::DoExtract(bool here)
{
    auto eng = AcquireEngine();
    // Header-encrypted archives are not open yet: they must be reopened
    // with a password before capability checks, selection lookup or extract.
    if (!ArchiveOps::EnsureOpenPassword(m_hwnd, eng)) return;
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

    // Encrypted items want their password before the first attempt, and
    // a wrong one gets asked again rather than reported as damage.
    if (!ArchiveOps::EnsureReadPassword(m_hwnd, eng)) return;

    std::vector<ArchiveEntry> sel;
    const bool itemMode = (m_mode == ModeItem && SelectedEntries(eng, sel));

    bool ok = true;
    int passwordAttempts = 0;
    for (;;)
    {
        const bool hadPassword = !eng->GetPassword().empty();
        if (hadPassword) ++passwordAttempts;

        WaitCursor wait;
        ok = true;
        if (itemMode)
        {
            for (const auto& e : sel)
            {
                if (!eng->ExtractFile(e, dest, nullptr))
                {
                    ok = false;
                    // Do not let a later successful unencrypted item clear
                    // the password classification from this failed one.
                    if (eng->LastErrorWasWrongPassword()) break;
                }
            }
        }
        else
        {
            ok = eng->ExtractAll(dest, nullptr);
        }
        if (ok || !eng->LastErrorWasWrongPassword()) break;
        if (hadPassword && passwordAttempts >= 3) break;
        const bool supplied = eng->LastErrorNeedsPassword()
            ? ArchiveOps::EnsureReadPassword(m_hwnd, eng)
            : ArchiveOps::AskPasswordAgain(m_hwnd, eng);
        if (!supplied) break;
    }

    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH, dest.c_str(), nullptr);

    if (!ok)
        MessageBoxW(m_hwnd,
            eng->LastErrorWasWrongPassword()
                ? L"Some items could not be extracted: they are encrypted, "
                  L"and no correct password was given."
                : L"Some items could not be extracted.\n\n"
                  L"The archive may be damaged or incomplete.",
            L"ArchiveFldr", MB_ICONWARNING | MB_OK);
}

// ── Compression ──────────────────────────────────────────
namespace {

// Settings -> writer options for the one-click and e-mail commands.
ArchiveWriter::Options OptionsFromSettings(const std::wstring& format)
{
    const Settings& s = Settings::Get();
    ArchiveWriter::Options opt;
    opt.format       = format.empty() ? s.defaultFormat : format;
    opt.level        = (int)s.defaultCompLevel;
    opt.solid        = s.createSolidArchive;
    opt.encryptNames = s.encryptFileNames;
    opt.threads      = s.multiThreaded ? s.threadCount : 1;
    return opt;
}

static std::wstring Q(const std::wstring& v)
{
    std::wstring s = L"\""; size_t bs = 0;
    for (wchar_t c : v) { if (c == L'\\') { ++bs; continue; } if (c == L'\"') { s.append(bs * 2 + 1, L'\\'); s += c; bs = 0; } else { s.append(bs, L'\\'); bs = 0; s += c; } }
    s.append(bs * 2, L'\\'); return s + L"\"";
}

static bool StartCompressionWorker(const std::wstring& out, const std::vector<std::wstring>& paths,
                                   const ArchiveWriter::Options& o)
{
    uint64_t total = 0;
    const auto items = ArchiveWriter::CollectItems(paths, &total);
    if (items.empty()) return false;
    std::wstring error;
    const bool ok = ArchiveWriter::Compress(out, items, o, nullptr, &error);
    if (!ok) {
        MessageBoxW(nullptr, error.empty() ? L"The archive could not be created." : error.c_str(),
                    L"ArchiveFldr", MB_ICONERROR | MB_OK);
        return false;
    }
    const std::wstring createdPath = o.volumeBytes ? out + L".001" : out;
    SHChangeNotify(SHCNE_CREATE, SHCNF_PATH, createdPath.c_str(), nullptr);
    std::wstring dir = out;
    if (PathRemoveFileSpecW(&dir[0])) {
        dir.resize(wcslen(dir.c_str()));
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH, dir.c_str(), nullptr);
    }
    return true;
}

} // namespace

void CContextMenu::DoCompress(bool here)
{
    if (m_paths.empty()) return;

    if (!ArchiveWriter::IsAvailable())
    {
        MessageBoxW(m_hwnd,
            (L"ArchiveFldr cannot create archives: no usable 7z.dll was found.\n\n" +
             ThirdParty::DescribeSearch(L"7z")).c_str(),
            L"ArchiveFldr", MB_ICONWARNING | MB_OK);
        return;
    }

    std::vector<ArchiveWriter::Item> items = ArchiveWriter::CollectItems(m_paths);
    if (items.empty())
    {
        MessageBoxW(m_hwnd, L"Nothing in the selection could be read.",
                    L"ArchiveFldr", MB_ICONWARNING | MB_OK);
        return;
    }

    const bool singleStreamAllowed =
        items.size() == 1 && !items[0].isDir && !items[0].diskPath.empty();
    std::wstring format = Settings::Get().defaultFormat;
    if (!ArchiveWriter::FormatIsWritable(format) ||
        (!singleStreamAllowed && !ArchiveWriter::CanAddToFormat(format)))
    {
        const auto formats = ArchiveWriter::WritableFormats();
        format.clear();
        for (const auto& candidate : formats)
            if (singleStreamAllowed || ArchiveWriter::CanAddToFormat(candidate))
            { format = candidate; break; }
        if (format.empty())
        {
            MessageBoxW(m_hwnd,
                L"No available writer can store all selected items.",
                L"ArchiveFldr", MB_ICONWARNING | MB_OK);
            return;
        }
    }

    std::wstring outPath = ArchiveWriter::SuggestOutputPath(
        m_paths, ArchiveWriter::DefaultExtensionFor(format));
    if (outPath.empty()) return;

    ArchiveWriter::Options options = OptionsFromSettings(format);
    if (!here)
    {
        AddToArchiveDialog::Request rq;
        rq.path       = outPath;
        rq.format     = format;
        rq.lockFormat = false;
        rq.singleStreamAllowed = singleStreamAllowed;
        rq.fileCount  = items.size();

        AddToArchiveDialog::Result result;
        if (!AddToArchiveDialog::Show(m_hwnd, rq, result)) return;
        outPath = result.path;
        options = result.opt;
    }

    // Refuse to put the archive inside its own input: it would try to
    // compress the file it is still writing.
    for (const auto& item : items)
        if (!item.diskPath.empty() &&
            _wcsicmp(item.diskPath.c_str(), outPath.c_str()) == 0)
        {
            MessageBoxW(m_hwnd,
                L"The archive would be written into its own source. "
                L"Choose a different name or location.",
                L"ArchiveFldr", MB_ICONWARNING | MB_OK);
            return;
        }

    if (!StartCompressionWorker(outPath, m_paths, options))
    {
        MessageBoxW(m_hwnd, L"ArchiveFldrCompress.exe could not be started.",
                    L"ArchiveFldr", MB_ICONERROR | MB_OK);
    }
    // Compression continues in a separate process. Explorer is released
    // immediately; the worker writes atomically and notifies the shell when
    // the archive is complete.
}

void CContextMenu::DoCompressEmail()
{
    if (m_paths.empty()) return;

    if (!ArchiveWriter::IsAvailable())
    {
        MessageBoxW(m_hwnd,
            (L"ArchiveFldr cannot create archives: no usable 7z.dll was found.\n\n" +
             ThirdParty::DescribeSearch(L"7z")).c_str(),
            L"ArchiveFldr", MB_ICONWARNING | MB_OK);
        return;
    }

    // Mail attachments go in a zip: it is the one format every mail
    // client and recipient can open without installing anything.
    const std::wstring format = L"zip";
    if (!ArchiveWriter::FormatIsWritable(format))
    {
        MessageBoxW(m_hwnd,
            L"A writable ZIP handler is unavailable, so the mail attachment could not be created.",
            L"ArchiveFldr", MB_ICONWARNING | MB_OK);
        return;
    }

    // Built in a private temp folder so a second run cannot collide with
    // the attachment the mail client is still holding open.
    const std::wstring dir = ArchiveOps::MakeTempDir(L"mail");
    if (dir.empty())
    {
        MessageBoxW(m_hwnd, L"A temporary folder could not be created.",
                    L"ArchiveFldr", MB_ICONERROR | MB_OK);
        return;
    }

    std::wstring base = (m_paths.size() == 1)
        ? std::wstring(PathFindFileNameW(m_paths.front().c_str()))
        : std::wstring(L"Archive");
    if (m_paths.size() == 1)
    {
        const DWORD attr = GetFileAttributesW(m_paths.front().c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            const size_t dot = base.rfind(L'.');
            if (dot != std::wstring::npos && dot > 0) base.resize(dot);
        }
    }
    const std::wstring outPath =
        dir + L"\\" + base + ArchiveWriter::DefaultExtensionFor(format);

    uint64_t total = 0;
    std::vector<ArchiveWriter::Item> items =
        ArchiveWriter::CollectItems(m_paths, &total);
    if (items.empty()) return;

    std::wstring message;
    bool ok;
    {
        WaitCursor wait;
        ok = ArchiveWriter::Compress(outPath, items,
                                     OptionsFromSettings(format),
                                     nullptr, &message);
    }
    if (!ok)
    {
        MessageBoxW(m_hwnd,
            message.empty() ? L"The archive could not be created." : message.c_str(),
            L"ArchiveFldr", MB_ICONERROR | MB_OK);
        return;
    }

    // MAPI's "attach" parameter on a mailto: URL is honoured by Outlook
    // and ignored by most other clients, so if nothing picks it up the
    // folder holding the archive is opened for the user to drag from.
    const std::wstring url =
        L"mailto:?subject=" + base + L"&attach=%22" + outPath + L"%22";

    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask  = SEE_MASK_FLAG_NO_UI;
    sei.hwnd   = m_hwnd;
    sei.lpVerb = L"open";
    sei.lpFile = url.c_str();
    sei.nShow  = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei))
    {
        LPITEMIDLIST pidl = ILCreateFromPathW(outPath.c_str());
        if (pidl)
        {
            SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            ILFree(pidl);
        }
        MessageBoxW(m_hwnd,
            (L"No mail client accepted the attachment. The archive is here:\n\n" +
             outPath).c_str(),
            L"ArchiveFldr", MB_ICONINFORMATION | MB_OK);
    }
}

void CContextMenu::DoOpenShell()
{
    // ─────────────────────────────────────────────────────────────────
    // "Open with ArchiveFldr" — browse the archive inside Windows Explorer.
    //
    // It used to be ShellExecute("open", <archive>), which only asks the
    // shell to run the file type's default command: either nothing, or
    // whatever other archiver owns the type. Nothing in it told Explorer
    // to use this namespace extension.
    //
    // There is no command line that opens a namespace extension on a
    // specific file either — Explorer's /e takes an object to browse, not
    // a ::{CLSID} to browse it with. Instead, build a PIDL explicitly bound
    // to ArchiveFldr's file-as-folder ProgID and ask the browser to navigate
    // to it.
    // ─────────────────────────────────────────────────────────────────
    if (m_archivePath.empty()) {
        MessageBoxW(m_hwnd, L"No archive was selected.",
                    L"ArchiveFldr", MB_ICONWARNING | MB_OK);
        return;
    }

    if (!PathFileExistsW(m_archivePath.c_str())) {
        MessageBoxW(m_hwnd,
            (L"The archive no longer exists:\n\n" + m_archivePath).c_str(),
            L"ArchiveFldr", MB_ICONERROR | MB_OK);
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
            L"Put a bitness-matched 7z.dll next to ArchiveFldr, in any of:\n"
            L"    <ArchiveFldr folder>\\thirdparty\\7z\\7z.64.dll   (64-bit)\n"
            L"    <ArchiveFldr folder>\\thirdparty\\7z\\7z.32.dll   (32-bit)\n"
            L"    <ArchiveFldr folder>\\7z.64.dll  /  7z.32.dll  /  7z.dll\n\n"
            L"A system-wide 7-Zip installation is also used automatically.",
            L"ArchiveFldr", MB_ICONERROR | MB_OK);
        return;
    }

    {
        auto engine = CreateArchiveEngine(m_archivePath);
        const bool opened = engine && engine->Open(m_archivePath);
        // ShellView owns the password prompt for encrypted headers. Let it
        // navigate and ask rather than rejecting the archive here first.
        if (!opened && !(engine && engine->PasswordNeededToOpen()))
        {
            std::wstring msg = L"ArchiveFldr could not read this archive:\n\n" +
                               m_archivePath;
            if (is7z)
                msg += L"\n\nEngine: " + (Get7zEnginePath().empty()
                                            ? std::wstring(L"<none>")
                                            : Get7zEnginePath()) +
                       L"\n\nThe file may be corrupt, or its encrypted "
                       L"headers may require a password.";
            MessageBoxW(m_hwnd, msg.c_str(), L"ArchiveFldr",
                        MB_ICONERROR | MB_OK);
            return;
        }
    }

    // Ask the filesystem parser to bind this real path through ArchiveFldr's
    // ProgID explicitly. A normal filesystem PIDL is resolved through the
    // extension's default handler when Explorer has to open a new window,
    // which is why a Desktop invocation used to launch Bandizip. The bind
    // context keeps the item associated with ArchiveFldr without changing
    // the user's default application.
    PIDLIST_ABSOLUTE pidl = CreateArchiveFolderPidl(m_archivePath);
    if (!pidl)
    {
        MessageBoxW(m_hwnd,
            (L"Windows could not create an ArchiveFldr view for:\n\n" +
             m_archivePath).c_str(),
            L"ArchiveFldr", MB_ICONERROR | MB_OK);
        return;
    }

    // Same window first; a new one only if there is no browser to reuse
    // (invoked from the desktop, or from a host that exposes no site).
    if (BrowseAbsoluteInPlace(m_pSite, m_hwnd, pidl))
    {
        ILFree(pidl);
        return;
    }

    const bool opened = ShellBrowseToFolder(m_hwnd, pidl);
    ILFree(pidl);
    if (opened) return;

    MessageBoxW(m_hwnd,
        L"ArchiveFldr could not open a view of this archive.\n\n"
        L"Make sure the extension is registered (run, as administrator):\n"
        L"    regsvr32 ArchiveFldr.64.dll",
        L"ArchiveFldr", MB_ICONERROR | MB_OK);
}

void CContextMenu::DoTest()
{
    auto engine = AcquireEngine();
    if (!ArchiveOps::EnsureOpenPassword(m_hwnd, engine)) return;
    if (!ArchiveOps::EnsureCanRead(m_hwnd, engine)) return;
    if (!ArchiveOps::EnsureReadPassword(m_hwnd, engine)) return;

    bool ok = false;
    int passwordAttempts = 0;
    for (;;)
    {
        const bool hadPassword = !engine->GetPassword().empty();
        if (hadPassword) ++passwordAttempts;

        WaitCursor wait;
        ok = engine->Test(nullptr);
        if (ok || !engine->LastErrorWasWrongPassword()) break;
        if (hadPassword && passwordAttempts >= 3) break;
        const bool supplied = engine->LastErrorNeedsPassword()
            ? ArchiveOps::EnsureReadPassword(m_hwnd, engine)
            : ArchiveOps::AskPasswordAgain(m_hwnd, engine);
        if (!supplied) break;
    }

    MessageBoxW(m_hwnd,
        ok ? L"Archive test: PASSED\nAll files are intact."
           : engine->LastErrorWasWrongPassword()
               ? L"Archive test: FAILED!\nEncrypted items could not be "
                 L"verified without the correct password."
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

// Copy selected entries to the clipboard. The data object stages the bytes
// before OLE starts, so password UI never appears inside GetData().
void CContextMenu::DoCopy()
{
    auto eng = AcquireEngine();
    if (!ArchiveOps::EnsureOpenPassword(m_hwnd, eng)) return;
    if (!ArchiveOps::EnsureCanRead(m_hwnd, eng)) return;

    IDataObject* pdo = nullptr;
    const HRESULT dataHr = MakeDataObject(IID_IDataObject, (void**)&pdo);
    if (FAILED(dataHr) || !pdo)
    {
        if (dataHr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
        std::wstring msg = L"Nothing could be copied from this selection.";
        const std::wstring detail = eng->GetLastErrorText();
        if (!detail.empty()) msg += L"\n\n" + detail;
        MessageBoxW(m_hwnd, msg.c_str(), L"ArchiveFldr",
                    MB_ICONWARNING | MB_OK);
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

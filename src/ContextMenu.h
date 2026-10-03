// ContextMenu.h — IContextMenu3 for items inside an opened archive
//
// One handler serves the two menus an open archive view needs; the mode
// decides which:
//
//   ModeItem         right-click on one or more items INSIDE an archive
//                    (CShellFolder::GetUIObjectOf)
//   ModeBackground   right-click on empty space in an archive's view
//                    (CShellFolder::CreateViewObject)
//
// The Explorer-level context menu — right-clicking an archive file, or
// any file, in a normal Explorer folder — is gone. It was registered as
// a shell extension on every file type and never worked reliably, so
// the handler is no longer registered with the shell at all: this class
// is created directly by CShellFolder / CShellView and nothing else.
#pragma once
#include "stdafx.h"
#include "ArchiveEngine.h"

class CShellFolder;

class CContextMenu :
    public IContextMenu3,
    public IObjectWithSite
{
public:
    CContextMenu();

    // Called by CShellFolder::GetUIObjectOf — items inside the archive.
    void SetFolder(CShellFolder* pFolder, HWND hwnd,
                   UINT cidl, LPCITEMIDLIST const* apidl);
    // Called by CShellFolder::CreateViewObject — the view's background.
    void SetBackground(CShellFolder* pFolder, HWND hwnd);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID, void**) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;

    // IContextMenu
    STDMETHODIMP QueryContextMenu(HMENU, UINT, UINT, UINT, UINT) override;
    STDMETHODIMP InvokeCommand   (LPCMINVOKECOMMANDINFO)         override;
    STDMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, CHAR*, UINT) override;

    // IContextMenu2
    STDMETHODIMP HandleMenuMsg (UINT, WPARAM, LPARAM) override;

    // IContextMenu3
    STDMETHODIMP HandleMenuMsg2(UINT, WPARAM, LPARAM, LRESULT*) override;

    // IObjectWithSite — the view passes us its IShellBrowser through this,
    // which is what lets "Open" on a sub-folder navigate the window the user
    // is looking at instead of spawning a second one.
    STDMETHODIMP SetSite(IUnknown* pUnkSite) override;
    STDMETHODIMP GetSite(REFIID riid, void** ppv) override;

private:
    ~CContextMenu();

    // Command ids. The canonical verb and help text for each live in one
    // table (kVerbs[] in ContextMenu.cpp) so the id, the verb string and
    // the status-bar text can never drift apart.
    enum Cmd : UINT {
        CMD_OPEN_ITEM = 0,     // default verb for an item inside an archive
        CMD_EXTRACT,           // Extract... (whole archive, or the selection)
        CMD_EXTRACTHERE,
        CMD_TEST,
        CMD_INFO,
        CMD_COPY,
        CMD_PASTE,
        CMD_REFRESH,
        CMD_PROPERTIES,
        CMD_SETTINGS,
        CMD_COUNT
    };

    enum Mode { ModeItem, ModeBackground };

    // ── Command implementations ──────────────────────────
    void DoOpenItem    ();
    void DoExtract     (bool here);
    void DoTest        ();
    void DoInfo        ();
    void DoCopy        ();
    void DoPaste       ();
    void DoRefresh     ();
    void DoProperties  ();
    void DoSettings    ();

    // ── Helpers ──────────────────────────────────────────
    // Engine to operate on: the one the open folder already owns, or a
    // freshly opened one as a fallback.
    std::shared_ptr<IArchiveEngine> AcquireEngine();
    // Selected items resolved back to archive entries.
    bool SelectedEntries(const std::shared_ptr<IArchiveEngine>& eng,
                         std::vector<ArchiveEntry>& out);
    std::wstring AskForFolder(const wchar_t* title);
    // Navigate the hosting view into a child of this folder.
    bool         BrowseTo(LPCITEMIDLIST pidlRel);
    HRESULT      MakeDataObject(REFIID riid, void** ppv);
    void         NotifyRefresh();

    long           m_cRef      = 1;
    HWND           m_hwnd      = nullptr;
    CShellFolder*  m_pFolder   = nullptr;
    UINT           m_cmdBase   = 0;
    Mode           m_mode      = ModeItem;

    IUnknown*      m_pSite     = nullptr;  // the hosting shell view/browser

    std::vector<LPITEMIDLIST> m_pidls;   // selected items (owned copies)
    std::wstring              m_archivePath;
};

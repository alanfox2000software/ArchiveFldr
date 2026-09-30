// ContextMenu.h — IContextMenu3 + IShellExtInit
#pragma once
#include "stdafx.h"

class CShellFolder;

class CContextMenu :
    public IContextMenu3,
    public IShellExtInit
{
public:
    CContextMenu();

    // Called by CShellFolder::GetUIObjectOf
    void SetFolder(CShellFolder* pFolder, HWND hwnd,
                   UINT cidl, LPCITEMIDLIST const* apidl);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID, void**) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;

    // IShellExtInit
    STDMETHODIMP Initialize(LPCITEMIDLIST pidlFolder,
                             IDataObject*  pdtobj,
                             HKEY          hkeyProgID) override;

    // IContextMenu
    STDMETHODIMP QueryContextMenu(HMENU, UINT, UINT, UINT, UINT) override;
    STDMETHODIMP InvokeCommand   (LPCMINVOKECOMMANDINFO)         override;
    STDMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, CHAR*, UINT) override;

    // IContextMenu2
    STDMETHODIMP HandleMenuMsg (UINT, WPARAM, LPARAM) override;

    // IContextMenu3
    STDMETHODIMP HandleMenuMsg2(UINT, WPARAM, LPARAM, LRESULT*) override;

private:
    ~CContextMenu();

    // Command verbs → IDs
    enum Cmd : UINT {
        CMD_EXTRACT      = 0,
        CMD_EXTRACTHERE  = 1,
        CMD_ADD          = 2,
        CMD_COMPRESS_EMAIL=3,
        CMD_OPEN_SHELL   = 4,
        CMD_TEST         = 5,
        CMD_INFO         = 6,
        CMD_SETTINGS     = 7,
        CMD_COUNT        = 8
    };

    void DoExtract     (bool here);
    void DoAdd         ();
    void DoCompressEmail();
    void DoOpenShell   ();
    void DoTest        ();
    void DoInfo        ();
    void DoSettings    ();

    std::wstring GetSelectedPath() const;

    long           m_cRef      = 1;
    HWND           m_hwnd      = nullptr;
    CShellFolder*  m_pFolder   = nullptr;
    UINT           m_cmdBase   = 0;
    bool           m_useSubMenu = false;

    std::vector<LPITEMIDLIST> m_pidls;   // selected items (owned copies)
    std::wstring              m_archivePath;
};
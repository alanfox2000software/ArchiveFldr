// ShellView.h — IShellView2 implementation
#pragma once
#include "stdafx.h"

class CShellFolder;

class CShellView : public IShellView2
{
public:
    CShellView(CShellFolder* pFolder, HWND hwndOwner);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID, void**) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;

    // IOleWindow
    STDMETHODIMP GetWindow    (HWND*)      override;
    STDMETHODIMP ContextSensitiveHelp(BOOL) override;

    // IShellView
    STDMETHODIMP TranslateAccelerator (MSG*)          override;
    STDMETHODIMP EnableModeless       (BOOL)          override;
    STDMETHODIMP UIActivate           (UINT)          override;
    STDMETHODIMP Refresh              ()              override;
    STDMETHODIMP CreateViewWindow     (IShellView*, LPCFOLDERSETTINGS,
                                       IShellBrowser*, RECT*, HWND*) override;
    STDMETHODIMP DestroyViewWindow    ()              override;
    STDMETHODIMP GetCurrentInfo       (LPFOLDERSETTINGS) override;
    STDMETHODIMP AddPropertySheetPages(DWORD, LPFNSVADDPROPSHEETPAGE, LPARAM) override;
    STDMETHODIMP SaveViewState        ()              override;
    STDMETHODIMP SelectItem           (LPCITEMIDLIST, SVSIF) override;
    STDMETHODIMP GetItemObject        (UINT, REFIID, void**) override;

    // IShellView2
    STDMETHODIMP GetView              (SHELLVIEWID*, ULONG) override;
    STDMETHODIMP CreateViewWindow2    (LPSV2CVW2_PARAMS)  override;
    STDMETHODIMP HandleRename         (LPCITEMIDLIST)      override;
    STDMETHODIMP SelectAndPositionItem(LPCITEMIDLIST, UINT, POINT*) override;

private:
    ~CShellView();

    // View window procedure
    static LRESULT CALLBACK ViewWndProc(HWND,UINT,WPARAM,LPARAM);
    LRESULT WndProc(HWND,UINT,WPARAM,LPARAM);

    void CreateListView();
    void PopulateListView();
    void OnSize(int w, int h);
    void OnDblClick(int idx);
    void OnContextMenu(int x, int y);
    void OnSelChange();
    void NavigateTo(LPCITEMIDLIST pidl);

    long              m_cRef    = 1;
    HWND              m_hwnd    = nullptr;
    HWND              m_hwndOwner = nullptr;
    HWND              m_hwndList  = nullptr;

    // Visible list-view column -> the folder column it shows. Hiding a
    // column must not shift the data under the remaining headings.
    std::vector<int>  m_colMap;

    // Owned while the view lives: a list view does not copy its font.
    HFONT             m_hListFont = nullptr;
    IShellBrowser*    m_pBrowser  = nullptr;
    CShellFolder*     m_pFolder   = nullptr;
    FOLDERSETTINGS    m_fs        = {};
    HIMAGELIST        m_himl      = nullptr;
};
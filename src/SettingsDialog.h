// SettingsDialog.h
// Full tabbed Settings GUI — 6 pages shown in a tree-nav dialog
// Modelled after TC4Shell's Options window.
#pragma once
#include "stdafx.h"
#include "Settings.h"

// ── Page base ─────────────────────────────────────────────
class ISettingsPage
{
public:
    virtual ~ISettingsPage() = default;
    virtual HWND  Create (HWND hParent) = 0;
    virtual void  Show   (bool show)    = 0;
    virtual void  Load   ()             = 0;   // settings → controls
    virtual void  Save   ()             = 0;   // controls → settings
    virtual void  Resize (const RECT& rc) = 0;
    virtual bool  Dirty  () const       = 0;
    virtual const wchar_t* Title() const = 0;
    virtual HWND  GetHwnd() const       = 0;
};

// ── Concrete pages ────────────────────────────────────────
class CPageGeneral     : public ISettingsPage {
public:
    HWND  Create(HWND p) override; void Show(bool) override;
    void  Load()  override;        void Save()  override;
    void  Resize(const RECT&) override;
    bool  Dirty() const override { return m_dirty; }
    const wchar_t* Title() const override { return L"General"; }
    HWND GetHwnd() const override { return m_hwnd; }
private:
    static INT_PTR CALLBACK DlgProc(HWND,UINT,WPARAM,LPARAM);
    HWND m_hwnd=nullptr; bool m_dirty=false;
};

class CPageFormats     : public ISettingsPage {
public:
    HWND  Create(HWND p) override; void Show(bool) override;
    void  Load()  override;        void Save()  override;
    void  Resize(const RECT&) override;
    bool  Dirty() const override { return m_dirty; }
    const wchar_t* Title() const override { return L"Formats"; }
    HWND GetHwnd() const override { return m_hwnd; }
private:
    static INT_PTR CALLBACK DlgProc(HWND,UINT,WPARAM,LPARAM);
    HWND m_hwnd=nullptr; bool m_dirty=false;
    struct FmtRow { const wchar_t* ext; const wchar_t* desc; bool* setting; };
    std::vector<FmtRow> m_rows;
    void BuildRows();
};

class CPageIntegration : public ISettingsPage {
public:
    HWND  Create(HWND p) override; void Show(bool) override;
    void  Load()  override;        void Save()  override;
    void  Resize(const RECT&) override;
    bool  Dirty() const override { return m_dirty; }
    const wchar_t* Title() const override { return L"Integration"; }
    HWND GetHwnd() const override { return m_hwnd; }
private:
    static INT_PTR CALLBACK DlgProc(HWND,UINT,WPARAM,LPARAM);
    HWND m_hwnd=nullptr; bool m_dirty=false;
};

class CPageAppearance  : public ISettingsPage {
public:
    HWND  Create(HWND p) override; void Show(bool) override;
    void  Load()  override;        void Save()  override;
    void  Resize(const RECT&) override;
    bool  Dirty() const override { return m_dirty; }
    const wchar_t* Title() const override { return L"Appearance"; }
    HWND GetHwnd() const override { return m_hwnd; }
private:
    static INT_PTR CALLBACK DlgProc(HWND,UINT,WPARAM,LPARAM);
    HWND m_hwnd=nullptr; bool m_dirty=false;
};

class CPageAdvanced    : public ISettingsPage {
public:
    HWND  Create(HWND p) override; void Show(bool) override;
    void  Load()  override;        void Save()  override;
    void  Resize(const RECT&) override;
    bool  Dirty() const override { return m_dirty; }
    const wchar_t* Title() const override { return L"Advanced"; }
    HWND GetHwnd() const override { return m_hwnd; }
private:
    static INT_PTR CALLBACK DlgProc(HWND,UINT,WPARAM,LPARAM);
    HWND m_hwnd=nullptr; bool m_dirty=false;
};

class CPageAbout       : public ISettingsPage {
public:
    HWND  Create(HWND p) override; void Show(bool) override;
    void  Load()  override {}      void Save()  override {}
    void  Resize(const RECT&) override;
    bool  Dirty() const override { return false; }
    const wchar_t* Title() const override { return L"About"; }
    HWND GetHwnd() const override { return m_hwnd; }
private:
    static INT_PTR CALLBACK DlgProc(HWND,UINT,WPARAM,LPARAM);
    HWND m_hwnd=nullptr;
};

// ─────────────────────────────────────────────────────────
// CSettingsDialog — main container window
// ─────────────────────────────────────────────────────────
class CSettingsDialog
{
public:
    CSettingsDialog();
    ~CSettingsDialog();

    // Show modal settings dialog; returns true if user clicked OK
    bool Show(HWND hwndParent);

private:
    static INT_PTR CALLBACK DlgProc(HWND,UINT,WPARAM,LPARAM);
    INT_PTR WndProc(HWND,UINT,WPARAM,LPARAM);

    void OnInit    (HWND hDlg);
    void OnOK      ();
    void OnCancel  ();
    void OnApply   ();
    void OnTreeSel (HTREEITEM hItem);
    void OnResize  ();
    void BuildTree ();
    void ShowPage  (int idx);
    void EnableApply(bool en);

    HWND  m_hDlg      = nullptr;
    HWND  m_hTree     = nullptr;
    HWND  m_hFrame    = nullptr;
    HWND  m_hBtnOK    = nullptr;
    HWND  m_hBtnCancel= nullptr;
    HWND  m_hBtnApply = nullptr;
    int   m_curPage   = 0;
    bool  m_applied   = false;

    std::vector<std::unique_ptr<ISettingsPage>> m_pages;
    std::vector<HTREEITEM>                      m_treeItems;
};
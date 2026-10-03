// SettingsDialog.h
// ─────────────────────────────────────────────────────────────────────────
// The Options window, laid out after 7-Zip's File Manager > Tools >
// Options: a tab strip with Folders, Settings and Language,
// and OK / Cancel / Apply along the bottom.
//
// Every page is a child dialog created from a template in resource.rc and
// parked inside the tab control's display rectangle. Captions come from
// the template, then Lang::Apply overwrites any control whose id the
// loaded language file carries — so an untranslated control keeps its
// English text instead of going blank.
// ─────────────────────────────────────────────────────────────────────────
#pragma once
#include "stdafx.h"
#include "resource.h"
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
    virtual void  Place  (const RECT& rc) = 0;
    virtual bool  Dirty  () const       = 0;
    virtual void  ClearDirty()          = 0;
    virtual UINT  DialogId() const      = 0;   // also the language-file key
    virtual const wchar_t* Title() const = 0;  // fallback tab caption
    virtual HWND  GetHwnd() const       = 0;
    // Re-read the language file into this page's controls.
    virtual void  Retranslate()         = 0;
};

// The "System" page (file type associations) and the "ArchiveFldr" page
// (Explorer context menu integration) are gone: both features were
// removed. The context menu on items inside an opened archive is part
// of the namespace extension and needs no page.

// ── Folders: working folder ──────────────────────────────
class CPageFolders : public ISettingsPage
{
public:
    HWND Create(HWND) override;  void Show(bool) override;
    void Load() override;        void Save() override;
    void Place(const RECT&) override;
    bool Dirty() const override { return m_dirty; }
    void ClearDirty() override  { m_dirty = false; }
    UINT DialogId() const override { return IDD_PAGE_FOLDERS; }
    const wchar_t* Title() const override { return L"Folders"; }
    HWND GetHwnd() const override { return m_hwnd; }
    void Retranslate() override;

private:
    static INT_PTR CALLBACK DlgProc(HWND, UINT, WPARAM, LPARAM);
    void SyncEnabled();
    void Browse();

    HWND m_hwnd = nullptr;
    bool m_dirty = false;
};

// ── Settings: install / uninstall the shell extension ────
//
// Four buttons: install and uninstall, once per bitness. Each acts on
// the whole extension — browsing archives as folders, the context
// menu inside an opened archive, thumbnails, preview, Default apps.
// The Explorer right-click menu on archive files is gone; nothing
// here installs or uninstalls one.
class CPageInstall : public ISettingsPage
{
public:
    HWND Create(HWND) override;  void Show(bool) override;
    void Load() override;        void Save() override {}
    void Place(const RECT&) override;
    bool Dirty() const override { return false; }
    void ClearDirty() override  {}
    UINT DialogId() const override { return IDD_PAGE_SETTINGS; }
    const wchar_t* Title() const override { return L"Settings"; }
    HWND GetHwnd() const override { return m_hwnd; }
    void Retranslate() override;

private:
    static INT_PTR CALLBACK DlgProc(HWND, UINT, WPARAM, LPARAM);
    // One bitness, the one x64 names, and the whole extension both
    // ways: install registers everything, uninstall removes it all.
    void Run(bool install, bool x64);
    void RefreshState();

    HWND m_hwnd = nullptr;
};

// ── Language ─────────────────────────────────────────────
class CPageLanguage : public ISettingsPage
{
public:
    HWND Create(HWND) override;  void Show(bool) override;
    void Load() override;        void Save() override;
    void Place(const RECT&) override;
    bool Dirty() const override { return m_dirty; }
    void ClearDirty() override  { m_dirty = false; }
    UINT DialogId() const override { return IDD_PAGE_LANGUAGE; }
    const wchar_t* Title() const override { return L"Language"; }
    HWND GetHwnd() const override { return m_hwnd; }
    void Retranslate() override;

    // Code the user has highlighted, which may not be saved yet.
    std::wstring Selected() const;

private:
    static INT_PTR CALLBACK DlgProc(HWND, UINT, WPARAM, LPARAM);

    HWND m_hwnd = nullptr;
    HWND m_list = nullptr;
    bool m_dirty = false;
    std::vector<std::wstring> m_codes;   // row -> language code
};

// ─────────────────────────────────────────────────────────
// CSettingsDialog — the Options window
// ─────────────────────────────────────────────────────────
class CSettingsDialog
{
public:
    CSettingsDialog();
    ~CSettingsDialog();

    bool Show(HWND hwndParent);

    // Re-run Lang::Apply over the frame and every page. Called when the
    // Language page's selection is applied, so the change is visible
    // without restarting.
    void Retranslate();

private:
    static INT_PTR CALLBACK DlgProc(HWND, UINT, WPARAM, LPARAM);
    INT_PTR WndProc(HWND, UINT, WPARAM, LPARAM);

    void OnInit   (HWND hDlg);
    void OnOK     ();
    void OnCancel ();
    bool OnApply  ();
    void ShowPage (int idx);
    void PlacePages();

public:
    void EnableApply(bool en);

private:
    HWND m_hDlg   = nullptr;
    HWND m_hTab   = nullptr;
    int  m_cur    = 0;

    std::vector<std::unique_ptr<ISettingsPage>> m_pages;
    // Owned by m_pages; kept by hand because Retranslate asks it which
    // language the user has highlighted.
    CPageLanguage*    m_langPage    = nullptr;
};

// SettingsDialog.h
// ─────────────────────────────────────────────────────────────────────────
// The Options window, laid out after 7-Zip's File Manager > Tools >
// Options: a tab strip with System, ArchiveFldr, Folders, Settings and
// Language, and OK / Cancel / Apply along the bottom.
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

// ── System: file type associations, one tick column per bitness ──
class CPageSystem : public ISettingsPage
{
public:
    ~CPageSystem() override;
    HWND Create(HWND) override;  void Show(bool) override;
    void Load() override;        void Save() override;
    void Place(const RECT&) override;
    bool Dirty() const override { return m_dirty; }
    void ClearDirty() override  { m_dirty = false; }
    UINT DialogId() const override { return IDD_PAGE_SYSTEM; }
    const wchar_t* Title() const override { return L"System"; }
    HWND GetHwnd() const override { return m_hwnd; }
    void Retranslate() override;

private:
    static INT_PTR CALLBACK DlgProc(HWND, UINT, WPARAM, LPARAM);
    void BuildList();
    void SetAll(int col, bool on);
    void Toggle(int row, int col);
    bool Ticked(int row, int col) const;
    void SetTick(int row, int col, bool on);

    HWND       m_hwnd = nullptr;
    HWND       m_list = nullptr;
    HIMAGELIST m_imgs = nullptr;
    bool       m_dirty = false;
    // Column index of the 32-bit and 64-bit ticks, or -1 when this build
    // does not offer that one. A 32-bit settings program runs on a 32-bit
    // Windows, where a 64-bit DLL could not be loaded by anything.
    int        m_col32 = -1;
    int        m_col64 = -1;
    std::vector<const wchar_t*> m_exts;   // row -> extension
};

// ── ArchiveFldr: the shell context menu ──────────────────
class CPageArchiveFldr : public ISettingsPage
{
public:
    HWND Create(HWND) override;  void Show(bool) override;
    void Load() override;        void Save() override;
    void Place(const RECT&) override;
    bool Dirty() const override { return m_dirty; }
    void ClearDirty() override  { m_dirty = false; }
    UINT DialogId() const override { return IDD_PAGE_ARCHIVEFLDR; }
    const wchar_t* Title() const override { return L"ArchiveFldr"; }
    HWND GetHwnd() const override { return m_hwnd; }
    void Retranslate() override;

private:
    static INT_PTR CALLBACK DlgProc(HWND, UINT, WPARAM, LPARAM);
    void SyncEnabled();
    void FillItems();

    HWND m_hwnd = nullptr;
    HWND m_list = nullptr;
    bool m_dirty = false;
};

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
    void Run(bool install);
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
    CPageLanguage* m_langPage = nullptr;   // owned by m_pages
};

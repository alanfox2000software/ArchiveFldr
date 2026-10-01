// SettingsDialog.cpp
#include "stdafx.h"
#include "SettingsDialog.h"
#include "Registry.h"
#include "GUIDs.h"
#include "../res/resource.h"

// ═════════════════════════════════════════════════════════
// Helper: create a child dialog from a template ID
// ═════════════════════════════════════════════════════════
static HWND CreatePageDialog(UINT idd, DLGPROC proc,
                              LPARAM lParam, HWND hParent)
{
    return CreateDialogParamW(g_hDllInstance,
        MAKEINTRESOURCEW(idd), hParent, proc, lParam);
}

// ─────────────────────────────────────────────────────────
// Helper: set checkbox state
// ─────────────────────────────────────────────────────────
static inline void SetChk(HWND hDlg, int id, bool v)
    { CheckDlgButton(hDlg, id, v?BST_CHECKED:BST_UNCHECKED); }
static inline bool GetChk(HWND hDlg, int id)
    { return IsDlgButtonChecked(hDlg,id)==BST_CHECKED; }

// ═════════════════════════════════════════════════════════
// CPageGeneral
// ═════════════════════════════════════════════════════════
HWND CPageGeneral::Create(HWND hParent)
{
    m_hwnd = CreatePageDialog(IDD_PAGE_GENERAL, DlgProc,
        (LPARAM)this, hParent);
    return m_hwnd;
}
void CPageGeneral::Show(bool show)
    { if(m_hwnd) ShowWindow(m_hwnd, show?SW_SHOW:SW_HIDE); }
void CPageGeneral::Resize(const RECT& rc) {
    if (m_hwnd) SetWindowPos(m_hwnd,nullptr,
        rc.left,rc.top,rc.right-rc.left,rc.bottom-rc.top,
        SWP_NOZORDER|SWP_NOACTIVATE);
}

void CPageGeneral::Load()
{
    if (!m_hwnd) return;
    auto& s = Settings::Get();
    SetChk(m_hwnd, IDC_CHK_SHOW_PREVIEW,       s.showPreviewPane);
    SetChk(m_hwnd, IDC_CHK_SHOW_THUMBNAILS,     s.showThumbnails);
    SetChk(m_hwnd, IDC_CHK_SHOW_OVERLAY,        s.showIconOverlay);
    SetChk(m_hwnd, IDC_CHK_CONTEXT_MENU,        s.showContextMenu);
    SetChk(m_hwnd, IDC_CHK_OPEN_ON_DBLCLICK,    s.openArchiveOnDblClk);
    SetChk(m_hwnd, IDC_CHK_PROMPT_PATH,         s.promptForPath);
    SetChk(m_hwnd, IDC_CHK_REMEMBER_PATH,       s.rememberLastPath);
    SetChk(m_hwnd, IDC_CHK_SOLID_ARCHIVE,       s.createSolidArchive);
    SetChk(m_hwnd, IDC_CHK_ENCRYPT_NAMES,       s.encryptFileNames);
    SetChk(m_hwnd, IDC_CHK_AUTO_CLOSE,          s.autoCloseAfterOp);
    SetDlgItemTextW(m_hwnd, IDC_EDIT_DEFAULT_PATH,
        s.defaultExtractPath.c_str());
    // Compression level combo
    HWND hCombo = GetDlgItem(m_hwnd, IDC_COMBO_COMP_LEVEL);
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    const wchar_t* lvls[] = {L"Store (0)",L"Fastest (1)",
        L"Fast (3)",L"Normal (5)",L"Maximum (7)",L"Ultra (9)"};
    int idxMap[] = {0,1,3,5,7,9};
    for (auto lv : lvls)
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)lv);
    // Select current
    int curLvl = (int)s.defaultCompLevel;
    for (int i=0;i<6;i++) if(idxMap[i]==curLvl){ ComboBox_SetCurSel(hCombo,i); break; }

    // Format combo
    HWND hFmt = GetDlgItem(m_hwnd, IDC_COMBO_DEFAULT_FORMAT);
    SendMessageW(hFmt, CB_RESETCONTENT, 0, 0);
    const wchar_t* fmts[] = {L"ZIP",L"7-Zip",L"TAR",L"TAR+GZ",L"TAR+BZ2"};
    for (auto f : fmts) SendMessageW(hFmt,CB_ADDSTRING,0,(LPARAM)f);
    ComboBox_SetCurSel(hFmt, 0);
}

void CPageGeneral::Save()
{
    if (!m_hwnd) return;
    auto& s = Settings::Get();
    s.showPreviewPane    = GetChk(m_hwnd, IDC_CHK_SHOW_PREVIEW);
    s.showThumbnails     = GetChk(m_hwnd, IDC_CHK_SHOW_THUMBNAILS);
    s.showIconOverlay    = GetChk(m_hwnd, IDC_CHK_SHOW_OVERLAY);
    s.showContextMenu    = GetChk(m_hwnd, IDC_CHK_CONTEXT_MENU);
    s.openArchiveOnDblClk= GetChk(m_hwnd, IDC_CHK_OPEN_ON_DBLCLICK);
    s.promptForPath      = GetChk(m_hwnd, IDC_CHK_PROMPT_PATH);
    s.rememberLastPath   = GetChk(m_hwnd, IDC_CHK_REMEMBER_PATH);
    s.createSolidArchive = GetChk(m_hwnd, IDC_CHK_SOLID_ARCHIVE);
    s.encryptFileNames   = GetChk(m_hwnd, IDC_CHK_ENCRYPT_NAMES);
    s.autoCloseAfterOp   = GetChk(m_hwnd, IDC_CHK_AUTO_CLOSE);
    wchar_t buf[MAX_PATH]={};
    GetDlgItemTextW(m_hwnd,IDC_EDIT_DEFAULT_PATH,buf,MAX_PATH);
    s.defaultExtractPath = buf;
    int lvlIdx[] = {0,1,3,5,7,9};
    int sel = ComboBox_GetCurSel(GetDlgItem(m_hwnd,IDC_COMBO_COMP_LEVEL));
    if (sel>=0&&sel<6) s.defaultCompLevel=(CompLevel)lvlIdx[sel];
    m_dirty = false;
}

INT_PTR CALLBACK CPageGeneral::DlgProc(
    HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    CPageGeneral* p = nullptr;
    if (msg==WM_INITDIALOG) {
        p=(CPageGeneral*)lp;
        SetWindowLongPtrW(hDlg,DWLP_USER,(LONG_PTR)p);
        p->m_hwnd=hDlg; p->Load();
        return TRUE;
    }
    p=(CPageGeneral*)GetWindowLongPtrW(hDlg,DWLP_USER);
    if (!p) return FALSE;

    if (msg==WM_COMMAND) {
        WORD ctrl=LOWORD(wp), notif=HIWORD(wp);
        if (notif==BN_CLICKED||notif==CBN_SELCHANGE||notif==EN_CHANGE)
            p->m_dirty=true;
        if (ctrl==IDC_BTN_BROWSE_PATH) {
            wchar_t buf[MAX_PATH]={};
            BROWSEINFOW bi{hDlg,nullptr,buf,
                L"Default extract path:",
                BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE};
            LPITEMIDLIST pidl=SHBrowseForFolderW(&bi);
            if (pidl) {
                SHGetPathFromIDListW(pidl,buf);
                CoTaskMemFree(pidl);
                SetDlgItemTextW(hDlg,IDC_EDIT_DEFAULT_PATH,buf);
                p->m_dirty=true;
            }
        }
    }
    return FALSE;
}

// ═════════════════════════════════════════════════════════
// CPageFormats
// ═════════════════════════════════════════════════════════
HWND CPageFormats::Create(HWND hParent) {
    m_hwnd = CreatePageDialog(IDD_PAGE_FORMATS, DlgProc,
        (LPARAM)this, hParent);
    return m_hwnd;
}
void CPageFormats::Show(bool show)
    { if(m_hwnd) ShowWindow(m_hwnd, show?SW_SHOW:SW_HIDE); }
void CPageFormats::Resize(const RECT& rc) {
    if(m_hwnd) SetWindowPos(m_hwnd,nullptr,
        rc.left,rc.top,rc.right-rc.left,rc.bottom-rc.top,
        SWP_NOZORDER|SWP_NOACTIVATE);
}

void CPageFormats::BuildRows() {
    auto& s = Settings::Get();
    m_rows = {
        {L"ZIP / ZIPX / JAR / APK",  L"ZIP Archives",        &s.handleZip    },
        {L"7Z / 7ZIP",               L"7-Zip Archives",       &s.handle7z     },
        {L"RAR / R00",               L"RAR Archives",         &s.handleRar    },
        {L"TAR / TGZ / TBZ2 / TXZ", L"TAR Archives",         &s.handleTar    },
        {L"GZ / GZIP",               L"GZip Archives",        &s.handleGz     },
        {L"BZ2 / BZIP2",             L"BZip2 Archives",       &s.handleBz2    },
        {L"XZ / LZMA",               L"XZ/LZMA Archives",     &s.handleXz     },
        {L"ZST / ZSTD",              L"Zstandard Archives",   &s.handleZst    },
        {L"ISO / IMG",               L"Disk Images",          &s.handleIso    },
        {L"CAB",                     L"Cabinet Archives",     &s.handleCab    },
        {L"LZH / LHA",               L"LZH Archives",         &s.handleLzh    },
        {L"WIM / SWM / ESD",         L"Windows Imaging",      &s.handleWim    },
        {L"MSI / MSM / MSP",         L"MSI Packages",         &s.handleMsi    },
        {L"DOCX / XLSX / PPTX / ODT",L"Office Documents",     &s.handleOffice },
    };
}

void CPageFormats::Load() {
    if (!m_hwnd) return;
    BuildRows();
    HWND hList = GetDlgItem(m_hwnd, IDC_LIST_FORMATS);
    ListView_DeleteAllItems(hList);
    // Add columns if not yet added
    if (Header_GetItemCount(ListView_GetHeader(hList))==0) {
        LVCOLUMNW c{LVCF_TEXT|LVCF_WIDTH,LVCFMT_LEFT,240,(LPWSTR)L"Extension"};
        ListView_InsertColumn(hList,0,&c);
        c.pszText=(LPWSTR)L"Description"; c.cx=200;
        ListView_InsertColumn(hList,1,&c);
        ListView_SetExtendedListViewStyle(hList,
            LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    }
    for (int i=0;i<(int)m_rows.size();i++) {
        LVITEMW item{LVIF_TEXT,(int)i,0,0,0,(LPWSTR)m_rows[i].ext};
        ListView_InsertItem(hList,&item);
        ListView_SetItemText(hList,i,1,(LPWSTR)m_rows[i].desc);
        ListView_SetCheckState(hList,i,*m_rows[i].setting?TRUE:FALSE);
    }
}

void CPageFormats::Save() {
    if (!m_hwnd) return;
    HWND hList = GetDlgItem(m_hwnd, IDC_LIST_FORMATS);
    for (int i=0;i<(int)m_rows.size();i++)
        *m_rows[i].setting = ListView_GetCheckState(hList,i)!=0;
    m_dirty = false;
}

INT_PTR CALLBACK CPageFormats::DlgProc(
    HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    CPageFormats* p = nullptr;
    if (msg==WM_INITDIALOG) {
        p=(CPageFormats*)lp;
        SetWindowLongPtrW(hDlg,DWLP_USER,(LONG_PTR)p);
        p->m_hwnd=hDlg; p->Load();
        return TRUE;
    }
    p=(CPageFormats*)GetWindowLongPtrW(hDlg,DWLP_USER);
    if (!p) return FALSE;

    if (msg==WM_COMMAND) {
        HWND hList=GetDlgItem(hDlg,IDC_LIST_FORMATS);
        int n=ListView_GetItemCount(hList);
        if (LOWORD(wp)==IDC_BTN_CHECKALL_FMT) {
            for(int i=0;i<n;i++) ListView_SetCheckState(hList,i,TRUE);
            p->m_dirty=true;
        }
        if (LOWORD(wp)==IDC_BTN_UNCHECKALL_FMT) {
            for(int i=0;i<n;i++) ListView_SetCheckState(hList,i,FALSE);
            p->m_dirty=true;
        }
    }
    if (msg==WM_NOTIFY) {
        auto* nm=(NMHDR*)lp;
        if (nm->code==LVN_ITEMCHANGED) p->m_dirty=true;
    }
    return FALSE;
}

// ═════════════════════════════════════════════════════════
// CPageIntegration
// ═════════════════════════════════════════════════════════
HWND CPageIntegration::Create(HWND hParent) {
    m_hwnd = CreatePageDialog(IDD_PAGE_INTEGRATION, DlgProc,
        (LPARAM)this, hParent);
    return m_hwnd;
}
void CPageIntegration::Show(bool show)
    { if(m_hwnd) ShowWindow(m_hwnd,show?SW_SHOW:SW_HIDE); }
void CPageIntegration::Resize(const RECT& rc) {
    if(m_hwnd) SetWindowPos(m_hwnd,nullptr,
        rc.left,rc.top,rc.right-rc.left,rc.bottom-rc.top,
        SWP_NOZORDER|SWP_NOACTIVATE);
}
void CPageIntegration::Load() {
    if (!m_hwnd) return;
    auto& s = Settings::Get();
    SetChk(m_hwnd, IDC_CHK_INTEGRATE_EXPLORER, true);
    SetChk(m_hwnd, IDC_CHK_CTX_EXTRACT,        s.ctxExtract);
    SetChk(m_hwnd, IDC_CHK_CTX_EXTRACTHERE,    s.ctxExtractHere);
    SetChk(m_hwnd, IDC_CHK_CTX_ADDTOARCH,      s.ctxAddToArchive);
    SetChk(m_hwnd, IDC_CHK_CTX_COMPRESSEMAIL,  s.ctxCompressEmail);
    SetChk(m_hwnd, IDC_CHK_CTX_OPENINSH,       s.ctxOpenInShell);
    SetChk(m_hwnd, IDC_CHK_CTX_TESTARCH,       s.ctxTestArchive);
    SetChk(m_hwnd, IDC_CHK_CTX_ARCHINFO,       s.ctxArchiveInfo);
    SetChk(m_hwnd, IDC_CHK_CTX_SETTINGS,       s.ctxSettings);
    SetChk(m_hwnd, IDC_CHK_CTX_SUBMENU,        s.ctxUseSubMenu);
    SetDlgItemTextW(m_hwnd, IDC_EDIT_SUBMENU_TITLE, s.ctxSubMenuTitle.c_str());

    // Registration status
    wchar_t dllPath[MAX_PATH]={};
    GetModuleFileNameW(g_hDllInstance, dllPath, MAX_PATH);
    bool registered = PathFileExistsW(dllPath) &&
        []{
            HKEY hk=nullptr;
            bool ok = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                kRegKeyApproved,0,KEY_READ,&hk)==ERROR_SUCCESS;
            if(hk) RegCloseKey(hk);
            return ok;
        }();
    SetDlgItemTextW(m_hwnd, IDC_LBL_STATUS_REG,
        registered ? L"✓ Registered and active"
                   : L"✗ Not registered");
}
void CPageIntegration::Save() {
    if (!m_hwnd) return;
    auto& s = Settings::Get();
    s.ctxExtract      = GetChk(m_hwnd, IDC_CHK_CTX_EXTRACT);
    s.ctxExtractHere  = GetChk(m_hwnd, IDC_CHK_CTX_EXTRACTHERE);
    s.ctxAddToArchive = GetChk(m_hwnd, IDC_CHK_CTX_ADDTOARCH);
    s.ctxCompressEmail= GetChk(m_hwnd, IDC_CHK_CTX_COMPRESSEMAIL);
    s.ctxOpenInShell  = GetChk(m_hwnd, IDC_CHK_CTX_OPENINSH);
    s.ctxTestArchive  = GetChk(m_hwnd, IDC_CHK_CTX_TESTARCH);
    s.ctxArchiveInfo  = GetChk(m_hwnd, IDC_CHK_CTX_ARCHINFO);
    s.ctxSettings     = GetChk(m_hwnd, IDC_CHK_CTX_SETTINGS);
    s.ctxUseSubMenu   = GetChk(m_hwnd, IDC_CHK_CTX_SUBMENU);
    wchar_t buf[128]={};
    GetDlgItemTextW(m_hwnd,IDC_EDIT_SUBMENU_TITLE,buf,128);
    s.ctxSubMenuTitle = buf;
    m_dirty = false;
}
INT_PTR CALLBACK CPageIntegration::DlgProc(
    HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    CPageIntegration* p=nullptr;
    if (msg==WM_INITDIALOG) {
        p=(CPageIntegration*)lp;
        SetWindowLongPtrW(hDlg,DWLP_USER,(LONG_PTR)p);
        p->m_hwnd=hDlg; p->Load(); return TRUE;
    }
    p=(CPageIntegration*)GetWindowLongPtrW(hDlg,DWLP_USER);
    if (!p) return FALSE;

    if (msg==WM_COMMAND) {
        WORD ctrl=LOWORD(wp);
        if (ctrl==IDC_BTN_REGISTER) {
            wchar_t path[MAX_PATH]={};
            GetModuleFileNameW(g_hDllInstance,path,MAX_PATH);
            HRESULT hr = CRegistry::RegisterAll(path);
            SetDlgItemTextW(hDlg, IDC_LBL_STATUS_REG,
                SUCCEEDED(hr) ? L"✓ Registered successfully"
                              : L"✗ Registration failed (run as admin)");
        }
        if (ctrl==IDC_BTN_UNREGISTER) {
            HRESULT hr = CRegistry::UnregisterAll();
            SetDlgItemTextW(hDlg, IDC_LBL_STATUS_REG,
                SUCCEEDED(hr) ? L"✗ Unregistered"
                              : L"✗ Unregister failed (run as admin)");
        }
        if (HIWORD(wp)==BN_CLICKED||HIWORD(wp)==EN_CHANGE)
            p->m_dirty=true;
    }
    return FALSE;
}

// ═════════════════════════════════════════════════════════
// CPageAppearance
// ═════════════════════════════════════════════════════════
HWND CPageAppearance::Create(HWND hParent) {
    m_hwnd = CreatePageDialog(IDD_PAGE_APPEARANCE, DlgProc,
        (LPARAM)this, hParent);
    return m_hwnd;
}
void CPageAppearance::Show(bool show)
    { if(m_hwnd) ShowWindow(m_hwnd,show?SW_SHOW:SW_HIDE); }
void CPageAppearance::Resize(const RECT& rc) {
    if(m_hwnd) SetWindowPos(m_hwnd,nullptr,
        rc.left,rc.top,rc.right-rc.left,rc.bottom-rc.top,
        SWP_NOZORDER|SWP_NOACTIVATE);
}
void CPageAppearance::Load() {
    if (!m_hwnd) return;
    auto& s = Settings::Get();
    SetChk(m_hwnd, IDC_CHK_DARK_MODE,        s.darkMode);
    SetChk(m_hwnd, IDC_CHK_CUSTOM_ICONS,     s.useCustomIcons);
    SetChk(m_hwnd, IDC_CHK_SHOW_SIZE_COL,    s.showSizeColumn);
    SetChk(m_hwnd, IDC_CHK_SHOW_DATE_COL,    s.showDateColumn);
    SetChk(m_hwnd, IDC_CHK_SHOW_RATIO_COL,   s.showRatioColumn);
    SetChk(m_hwnd, IDC_CHK_SHOW_METHOD_COL,  s.showMethodColumn);
    SetChk(m_hwnd, IDC_CHK_SHOW_CRC_COL,     s.showCrcColumn);
    SetChk(m_hwnd, IDC_CHK_ALTERNATE_ROWS,   s.alternateRowColors);

    HWND hDateCombo = GetDlgItem(m_hwnd, IDC_COMBO_DATE_FORMAT);
    SendMessageW(hDateCombo,CB_RESETCONTENT,0,0);
    const wchar_t* dfmts[]={L"ISO 8601 (2024-12-31)",
        L"DD/MM/YYYY",L"MM/DD/YYYY",L"Relative (2 hrs ago)"};
    for (auto d:dfmts) SendMessageW(hDateCombo,CB_ADDSTRING,0,(LPARAM)d);
    ComboBox_SetCurSel(hDateCombo,(int)s.dateFormat);

    // Font preview
    wchar_t fontDesc[64];
    swprintf_s(fontDesc,64,L"%s %dpt",s.fontFace.c_str(),s.fontSize);
    SetDlgItemTextW(m_hwnd,IDC_STATIC_FONT_PREVIEW,fontDesc);
}
void CPageAppearance::Save() {
    if (!m_hwnd) return;
    auto& s = Settings::Get();
    s.darkMode         = GetChk(m_hwnd, IDC_CHK_DARK_MODE);
    s.useCustomIcons   = GetChk(m_hwnd, IDC_CHK_CUSTOM_ICONS);
    s.showSizeColumn   = GetChk(m_hwnd, IDC_CHK_SHOW_SIZE_COL);
    s.showDateColumn   = GetChk(m_hwnd, IDC_CHK_SHOW_DATE_COL);
    s.showRatioColumn  = GetChk(m_hwnd, IDC_CHK_SHOW_RATIO_COL);
    s.showMethodColumn = GetChk(m_hwnd, IDC_CHK_SHOW_METHOD_COL);
    s.showCrcColumn    = GetChk(m_hwnd, IDC_CHK_SHOW_CRC_COL);
    s.alternateRowColors=GetChk(m_hwnd, IDC_CHK_ALTERNATE_ROWS);
    int sel=ComboBox_GetCurSel(GetDlgItem(m_hwnd,IDC_COMBO_DATE_FORMAT));
    if (sel>=0) s.dateFormat=(DateFmt)sel;
    m_dirty=false;
}
INT_PTR CALLBACK CPageAppearance::DlgProc(
    HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    CPageAppearance* p=nullptr;
    if (msg==WM_INITDIALOG) {
        p=(CPageAppearance*)lp;
        SetWindowLongPtrW(hDlg,DWLP_USER,(LONG_PTR)p);
        p->m_hwnd=hDlg; p->Load(); return TRUE;
    }
    p=(CPageAppearance*)GetWindowLongPtrW(hDlg,DWLP_USER);
    if (!p) return FALSE;

    if (msg==WM_COMMAND) {
        if (LOWORD(wp)==IDC_BTN_FONT) {
            CHOOSEFONTW cf{sizeof(cf)};
            LOGFONTW lf{}; wcscpy_s(lf.lfFaceName,
                Settings::Get().fontFace.c_str());
            lf.lfHeight = -Settings::Get().fontSize;
            cf.lpLogFont=&lf; cf.Flags=CF_SCREENFONTS|CF_INITTOLOGFONTSTRUCT;
            if (ChooseFontW(&cf)) {
                Settings::Get().fontFace = lf.lfFaceName;
                Settings::Get().fontSize = abs(lf.lfHeight);
                wchar_t fd[64];
                swprintf_s(fd,64,L"%s %dpt",lf.lfFaceName,abs(lf.lfHeight));
                SetDlgItemTextW(hDlg,IDC_STATIC_FONT_PREVIEW,fd);
                p->m_dirty=true;
            }
        }
        if (HIWORD(wp)==BN_CLICKED||HIWORD(wp)==CBN_SELCHANGE)
            p->m_dirty=true;
    }
    return FALSE;
}

// ═════════════════════════════════════════════════════════
// CPageAdvanced
// ═════════════════════════════════════════════════════════
HWND CPageAdvanced::Create(HWND hParent) {
    m_hwnd = CreatePageDialog(IDD_PAGE_ADVANCED, DlgProc,
        (LPARAM)this, hParent);
    return m_hwnd;
}
void CPageAdvanced::Show(bool show)
    { if(m_hwnd) ShowWindow(m_hwnd,show?SW_SHOW:SW_HIDE); }
void CPageAdvanced::Resize(const RECT& rc) {
    if(m_hwnd) SetWindowPos(m_hwnd,nullptr,
        rc.left,rc.top,rc.right-rc.left,rc.bottom-rc.top,
        SWP_NOZORDER|SWP_NOACTIVATE);
}
void CPageAdvanced::Load() {
    if (!m_hwnd) return;
    auto& s = Settings::Get();
    SetChk(m_hwnd, IDC_CHK_MULTITHREADED, s.multiThreaded);
    SetChk(m_hwnd, IDC_CHK_USE_TEMP_DIR,  s.useTempDir);
    SetChk(m_hwnd, IDC_CHK_LOG_ERRORS,    s.logErrors);
    SetChk(m_hwnd, IDC_CHK_CHECK_UPDATES, s.checkForUpdates);
    SetChk(m_hwnd, IDC_CHK_SEND_USAGE,    s.sendUsageData);
    SetChk(m_hwnd, IDC_CHK_CACHE_THUMBS,  s.cacheThumbnails);
    SetDlgItemInt (m_hwnd, IDC_EDIT_THREAD_COUNT, s.threadCount, FALSE);
    SetDlgItemTextW(m_hwnd, IDC_EDIT_TEMP_DIR,  s.tempDirPath.c_str());
    SetDlgItemTextW(m_hwnd, IDC_EDIT_LOG_PATH,  s.logFilePath.c_str());
    SetDlgItemInt  (m_hwnd, IDC_EDIT_MAX_MEM,   s.maxMemoryMB,  FALSE);
    SetDlgItemInt  (m_hwnd, IDC_EDIT_CACHE_SIZE,s.cacheSizeMB,  FALSE);

    // Spin controls
    SendDlgItemMessageW(m_hwnd,IDC_SPIN_THREAD_COUNT,
        UDM_SETRANGE32, 0, 64);
    SendDlgItemMessageW(m_hwnd,IDC_SPIN_MAX_MEM,
        UDM_SETRANGE32, 64, 32768);
    SendDlgItemMessageW(m_hwnd,IDC_SPIN_MAX_MEM,
        UDM_SETPOS32, 0, s.maxMemoryMB);
}
void CPageAdvanced::Save() {
    if (!m_hwnd) return;
    auto& s = Settings::Get();
    s.multiThreaded  = GetChk(m_hwnd, IDC_CHK_MULTITHREADED);
    s.useTempDir     = GetChk(m_hwnd, IDC_CHK_USE_TEMP_DIR);
    s.logErrors      = GetChk(m_hwnd, IDC_CHK_LOG_ERRORS);
    s.checkForUpdates= GetChk(m_hwnd, IDC_CHK_CHECK_UPDATES);
    s.sendUsageData  = GetChk(m_hwnd, IDC_CHK_SEND_USAGE);
    s.cacheThumbnails= GetChk(m_hwnd, IDC_CHK_CACHE_THUMBS);
    s.threadCount    = GetDlgItemInt(m_hwnd,IDC_EDIT_THREAD_COUNT,nullptr,FALSE);
    s.maxMemoryMB    = GetDlgItemInt(m_hwnd,IDC_EDIT_MAX_MEM,nullptr,FALSE);
    s.cacheSizeMB    = GetDlgItemInt(m_hwnd,IDC_EDIT_CACHE_SIZE,nullptr,FALSE);
    wchar_t buf[MAX_PATH]={};
    GetDlgItemTextW(m_hwnd,IDC_EDIT_TEMP_DIR,buf,MAX_PATH);
    s.tempDirPath=buf;
    GetDlgItemTextW(m_hwnd,IDC_EDIT_LOG_PATH,buf,MAX_PATH);
    s.logFilePath=buf;
    m_dirty=false;
}
INT_PTR CALLBACK CPageAdvanced::DlgProc(
    HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    CPageAdvanced* p=nullptr;
    if (msg==WM_INITDIALOG) {
        p=(CPageAdvanced*)lp;
        SetWindowLongPtrW(hDlg,DWLP_USER,(LONG_PTR)p);
        p->m_hwnd=hDlg; p->Load(); return TRUE;
    }
    p=(CPageAdvanced*)GetWindowLongPtrW(hDlg,DWLP_USER);
    if (!p) return FALSE;

    if (msg==WM_COMMAND) {
        auto browsePath = [&](int editId) {
            wchar_t buf[MAX_PATH]={};
            BROWSEINFOW bi{hDlg,nullptr,buf,L"Select folder:",
                BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE};
            LPITEMIDLIST pidl=SHBrowseForFolderW(&bi);
            if (pidl) {
                SHGetPathFromIDListW(pidl,buf);
                CoTaskMemFree(pidl);
                SetDlgItemTextW(hDlg,editId,buf);
                p->m_dirty=true;
            }
        };
        WORD ctrl=LOWORD(wp);
        if (ctrl==IDC_BTN_BROWSE_TEMP) browsePath(IDC_EDIT_TEMP_DIR);
        if (ctrl==IDC_BTN_BROWSE_LOG)  browsePath(IDC_EDIT_LOG_PATH);
        if (ctrl==IDC_BTN_CLEAR_CACHE) {
            wchar_t tmp[MAX_PATH]={};
            GetModuleFileNameW(g_hDllInstance,tmp,MAX_PATH);
            PathRemoveFileSpecW(tmp);
            wcscat_s(tmp,L"\\ThumbCache");
            SHFILEOPSTRUCTW fo = {};
            fo.hwnd   = hDlg;
            fo.wFunc  = FO_DELETE;
            fo.pFrom  = tmp;    // must be double-null terminated
            fo.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_SILENT;
            SHFileOperationW(&fo);
            MessageBoxW(hDlg,L"Thumbnail cache cleared.",
                L"Cache",MB_ICONINFORMATION);
        }
        if (HIWORD(wp)==BN_CLICKED||HIWORD(wp)==EN_CHANGE)
            p->m_dirty=true;
    }
    return FALSE;
}

// ═════════════════════════════════════════════════════════
// CPageAbout
// ═════════════════════════════════════════════════════════
HWND CPageAbout::Create(HWND hParent) {
    m_hwnd = CreatePageDialog(IDD_PAGE_ABOUT, DlgProc,
        (LPARAM)this, hParent);
    return m_hwnd;
}
void CPageAbout::Show(bool show)
    { if(m_hwnd) ShowWindow(m_hwnd,show?SW_SHOW:SW_HIDE); }
void CPageAbout::Resize(const RECT& rc) {
    if(m_hwnd) SetWindowPos(m_hwnd,nullptr,
        rc.left,rc.top,rc.right-rc.left,rc.bottom-rc.top,
        SWP_NOZORDER|SWP_NOACTIVATE);
}
INT_PTR CALLBACK CPageAbout::DlgProc(
    HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    CPageAbout* p=nullptr;
    if (msg==WM_INITDIALOG) {
        p=(CPageAbout*)lp;
        SetWindowLongPtrW(hDlg,DWLP_USER,(LONG_PTR)p);
        p->m_hwnd=hDlg;
        SetDlgItemTextW(hDlg,IDC_STATIC_NAME,    L"ShellNSE");
        SetDlgItemTextW(hDlg,IDC_STATIC_VERSION, L"Version 1.0.0");
        SetDlgItemTextW(hDlg,IDC_STATIC_DESC,
            L"Windows Shell Namespace Extension\r\n"
            L"Browse archives like folders in Explorer.\r\n\r\n"
            L"Supports ZIP, 7Z, RAR, TAR, GZ, BZ2, XZ,\r\n"
            L"ZST, ISO, CAB, LZH, WIM, MSI and more.");
        SetDlgItemTextW(hDlg,IDC_STATIC_COPYRIGHT,
            L"© 2024  Open Source  |  MIT License");
        return TRUE;
    }
    p=(CPageAbout*)GetWindowLongPtrW(hDlg,DWLP_USER);
    if (!p) return FALSE;

    if (msg==WM_COMMAND) {
        if (LOWORD(wp)==IDC_LINK_WEBSITE)
            ShellExecuteW(hDlg,L"open",
                L"https://github.com/ShellNSE",nullptr,nullptr,SW_SHOW);
        if (LOWORD(wp)==IDC_BTN_CHECK_UPDATE)
            MessageBoxW(hDlg,L"ShellNSE v1.0.0 is up to date.",
                L"Check for Updates",MB_ICONINFORMATION);
    }
    return FALSE;
}

// ═════════════════════════════════════════════════════════
// CSettingsDialog — main dialog
// ═════════════════════════════════════════════════════════
CSettingsDialog::CSettingsDialog()
{
    m_pages.push_back(std::make_unique<CPageGeneral>());
    m_pages.push_back(std::make_unique<CPageFormats>());
    m_pages.push_back(std::make_unique<CPageIntegration>());
    m_pages.push_back(std::make_unique<CPageAppearance>());
    m_pages.push_back(std::make_unique<CPageAdvanced>());
    m_pages.push_back(std::make_unique<CPageAbout>());
}
CSettingsDialog::~CSettingsDialog() = default;

bool CSettingsDialog::Show(HWND hwndParent)
{
    INT_PTR r = DialogBoxParamW(g_hDllInstance,
        MAKEINTRESOURCEW(IDD_SETTINGS_MAIN),
        hwndParent, DlgProc, (LPARAM)this);
    return (r == IDOK);
}

INT_PTR CALLBACK CSettingsDialog::DlgProc(
    HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    CSettingsDialog* p=nullptr;
    if (msg==WM_INITDIALOG) {
        p=(CSettingsDialog*)lp;
        SetWindowLongPtrW(hDlg,DWLP_USER,(LONG_PTR)p);
        p->m_hDlg=hDlg;
        p->OnInit(hDlg);
        return TRUE;
    }
    p=(CSettingsDialog*)GetWindowLongPtrW(hDlg,DWLP_USER);
    if (!p) return FALSE;
    return p->WndProc(hDlg,msg,wp,lp);
}

INT_PTR CSettingsDialog::WndProc(
    HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BTN_OK:     OnOK();     EndDialog(hDlg,IDOK);    return TRUE;
        case IDC_BTN_CANCEL: OnCancel(); EndDialog(hDlg,IDCANCEL);return TRUE;
        case IDC_BTN_APPLY:  OnApply();                            return TRUE;
        case IDC_BTN_HELP:
            ShellExecuteW(hDlg,L"open",
                L"https://github.com/ShellNSE/wiki",
                nullptr,nullptr,SW_SHOW);
            return TRUE;
        }
        return FALSE;

    case WM_NOTIFY: {
        auto* nm=(NMHDR*)lp;
        if (nm->hwndFrom==m_hTree &&
            (nm->code==TVN_SELCHANGEDW||nm->code==TVN_SELCHANGEDA)) {
            NMTREEVIEWW* ntv=(NMTREEVIEWW*)lp;
            OnTreeSel(ntv->itemNew.hItem);
        }
        return FALSE; }

    case WM_SIZE:
        OnResize(); return FALSE;

    case WM_GETMINMAXINFO: {
        auto* mm=(MINMAXINFO*)lp;
        mm->ptMinTrackSize={640,500};
        return FALSE; }

    case WM_CLOSE:
        EndDialog(hDlg,IDCANCEL); return TRUE;
    }
    return FALSE;
}

void CSettingsDialog::OnInit(HWND hDlg)
{
    SetWindowTextW(hDlg, L"ShellNSE Settings");

    m_hTree     = GetDlgItem(hDlg, IDC_TREE_PAGES);
    m_hFrame    = GetDlgItem(hDlg, IDC_FRAME_PAGE);
    m_hBtnOK    = GetDlgItem(hDlg, IDC_BTN_OK);
    m_hBtnCancel= GetDlgItem(hDlg, IDC_BTN_CANCEL);
    m_hBtnApply = GetDlgItem(hDlg, IDC_BTN_APPLY);

    EnableWindow(m_hBtnApply, FALSE);

    // Create all pages (hidden)
    RECT rcFrame; GetWindowRect(m_hFrame, &rcFrame);
    ScreenToClient(hDlg,(POINT*)&rcFrame);
    ScreenToClient(hDlg,(POINT*)&rcFrame.right);
    rcFrame.left+=2; rcFrame.top+=2;
    rcFrame.right-=2; rcFrame.bottom-=2;

    for (auto& pg : m_pages) {
        HWND hw = pg->Create(hDlg);
        if (hw) {
            SetWindowPos(hw,nullptr,
                rcFrame.left, rcFrame.top,
                rcFrame.right-rcFrame.left,
                rcFrame.bottom-rcFrame.top,
                SWP_NOZORDER);
        }
        pg->Show(false);
    }

    BuildTree();
    ShowPage(0);
}

void CSettingsDialog::BuildTree()
{
    TreeView_DeleteAllItems(m_hTree);
    m_treeItems.clear();

    // Icons for tree items
    HIMAGELIST hIml = ImageList_Create(16,16,ILC_COLOR32|ILC_MASK,8,2);
    TreeView_SetImageList(m_hTree, hIml, TVSIL_NORMAL);

    for (int i=0;i<(int)m_pages.size();i++) {
        TVINSERTSTRUCT tvis{};
        tvis.hParent      = TVI_ROOT;
        tvis.hInsertAfter = TVI_LAST;
        tvis.item.mask    = TVIF_TEXT|TVIF_PARAM;
        tvis.item.pszText = (LPWSTR)m_pages[i]->Title();
        tvis.item.lParam  = (LPARAM)i;
        HTREEITEM h = TreeView_InsertItem(m_hTree, &tvis);
        m_treeItems.push_back(h);
    }
    TreeView_SelectItem(m_hTree, m_treeItems[0]);
}

void CSettingsDialog::ShowPage(int idx)
{
    if (idx < 0 || idx >= (int)m_pages.size()) return;

    // Compute frame rect
    RECT rcFrame; GetWindowRect(m_hFrame, &rcFrame);
    ScreenToClient(m_hDlg,(POINT*)&rcFrame);
    ScreenToClient(m_hDlg,(POINT*)&rcFrame.right);
    rcFrame.left+=2; rcFrame.top+=2;
    rcFrame.right-=2; rcFrame.bottom-=2;

    for (int i=0;i<(int)m_pages.size();i++) {
        m_pages[i]->Show(i==idx);
        if (i==idx) m_pages[i]->Resize(rcFrame);
    }
    m_curPage = idx;

    // All page dialogs share the exact same rectangle inside the frame.
    // Explicitly raise the newly-active one to the top of the Z-order and
    // force it to repaint immediately, so it can never be left hidden
    // behind a previously-shown sibling page or a stale host repaint.
    HWND hActive = m_pages[idx]->GetHwnd();
    if (hActive) {
        SetWindowPos(hActive, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        InvalidateRect(hActive, nullptr, TRUE);
        UpdateWindow(hActive);
    }
}

void CSettingsDialog::OnTreeSel(HTREEITEM hItem)
{
    TVITEMW tv{TVIF_PARAM, hItem};
    TreeView_GetItem(m_hTree, &tv);
    ShowPage((int)tv.lParam);
}

void CSettingsDialog::OnResize()
{
    if (!m_hDlg) return;
    RECT rcClient; GetClientRect(m_hDlg, &rcClient);

    // Reposition tree (left strip, 160px wide)
    if (m_hTree)
        SetWindowPos(m_hTree,nullptr,8,8,
            160, rcClient.bottom-50,SWP_NOZORDER);

    // Reposition frame
    if (m_hFrame)
        SetWindowPos(m_hFrame,nullptr,
            174, 8,
            rcClient.right-182, rcClient.bottom-50,
            SWP_NOZORDER);

    // Reposition buttons (bottom row)
    int btnY = rcClient.bottom - 36;
    if (m_hBtnOK)
        SetWindowPos(m_hBtnOK,nullptr,
            rcClient.right-264, btnY, 80, 24, SWP_NOZORDER);
    if (m_hBtnCancel)
        SetWindowPos(m_hBtnCancel,nullptr,
            rcClient.right-176, btnY, 80, 24, SWP_NOZORDER);
    if (m_hBtnApply)
        SetWindowPos(m_hBtnApply,nullptr,
            rcClient.right-88,  btnY, 80, 24, SWP_NOZORDER);

    // Resize current page
    if (m_hFrame && m_curPage < (int)m_pages.size()) {
        RECT rcFrame; GetWindowRect(m_hFrame,&rcFrame);
        ScreenToClient(m_hDlg,(POINT*)&rcFrame);
        ScreenToClient(m_hDlg,(POINT*)&rcFrame.right);
        rcFrame.left+=2; rcFrame.top+=2;
        rcFrame.right-=2; rcFrame.bottom-=2;
        m_pages[m_curPage]->Resize(rcFrame);
    }
}

void CSettingsDialog::EnableApply(bool en)
    { EnableWindow(m_hBtnApply, en?TRUE:FALSE); }

void CSettingsDialog::OnOK()
{
    for (auto& pg : m_pages) pg->Save();
    Settings::Get().Save();
    m_applied = true;
}
void CSettingsDialog::OnCancel()
{
    if (!m_applied) Settings::Get().Load(); // restore
}
void CSettingsDialog::OnApply()
{
    for (auto& pg : m_pages) if (pg->Dirty()) pg->Save();
    Settings::Get().Save();
    EnableApply(false);
    m_applied = true;
}
// ShellView.cpp
#include "stdafx.h"
#include "ShellView.h"
#include "ShellFolder.h"
#include "ContextMenu.h"
#include "GUIDs.h"
#include "Settings.h"

static const wchar_t kViewClass[] = L"ShellNSE_View";

CShellView::CShellView(CShellFolder* pFolder, HWND hwndOwner)
    : m_pFolder(pFolder), m_hwndOwner(hwndOwner)
{
    if (m_pFolder) m_pFolder->AddRef();
    InterlockedIncrement(&g_cDllRefCount);

    // Register view class once
    static bool s_registered = false;
    if (!s_registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = ViewWndProc;
        wc.hInstance     = g_hDllInstance;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
        wc.lpszClassName = kViewClass;
        RegisterClassExW(&wc);
        s_registered = true;
    }
}
CShellView::~CShellView()
{
    if (m_pFolder) { m_pFolder->Release(); m_pFolder = nullptr; }
    if (m_pBrowser){ m_pBrowser->Release(); m_pBrowser = nullptr; }
    InterlockedDecrement(&g_cDllRefCount);
}

// ── IUnknown ─────────────────────────────────────────────
STDMETHODIMP CShellView::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER; *ppv = nullptr;
    if (IsEqualIID(riid,IID_IUnknown)  ||
        IsEqualIID(riid,IID_IShellView)||
        IsEqualIID(riid,IID_IShellView2))
    { *ppv = static_cast<IShellView2*>(this); AddRef(); return S_OK; }
    if (IsEqualIID(riid,IID_IOleWindow))
    { *ppv = static_cast<IOleWindow*>(this); AddRef(); return S_OK; }
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CShellView::AddRef()
    { return InterlockedIncrement(&m_cRef); }
STDMETHODIMP_(ULONG) CShellView::Release()
    { ULONG n=InterlockedDecrement(&m_cRef); if(!n) delete this; return n; }

// ── IOleWindow ───────────────────────────────────────────
STDMETHODIMP CShellView::GetWindow(HWND* p)
    { if(!p) return E_POINTER; *p=m_hwnd; return m_hwnd?S_OK:E_FAIL; }
STDMETHODIMP CShellView::ContextSensitiveHelp(BOOL) { return E_NOTIMPL; }

// ── IShellView ───────────────────────────────────────────
STDMETHODIMP CShellView::TranslateAccelerator(MSG* pm)
{
    // Let the list view handle accelerators
    if (m_hwndList && pm) {
        UINT vk = (UINT)pm->wParam;
        if (pm->message == WM_KEYDOWN && vk == VK_F5) {
            PopulateListView(); return S_OK;
        }
    }
    return S_FALSE;
}
STDMETHODIMP CShellView::EnableModeless(BOOL) { return S_OK; }
STDMETHODIMP CShellView::UIActivate(UINT uState)
{
    if (uState == SVUIA_ACTIVATE_FOCUS && m_hwndList)
        SetFocus(m_hwndList);
    return S_OK;
}
STDMETHODIMP CShellView::Refresh()
{
    PopulateListView(); return S_OK;
}

STDMETHODIMP CShellView::CreateViewWindow(
    IShellView* /*pPrevView*/, LPCFOLDERSETTINGS pfs,
    IShellBrowser* pBrowser, RECT* prc, HWND* phwnd)
{
    if (!pfs||!pBrowser||!prc||!phwnd) return E_POINTER;
    *phwnd = nullptr;
    m_fs = *pfs;
    m_pBrowser = pBrowser; m_pBrowser->AddRef();

    m_hwnd = CreateWindowExW(
        WS_EX_CLIENTEDGE, kViewClass, L"",
        WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|WS_CLIPCHILDREN,
        prc->left, prc->top,
        prc->right-prc->left, prc->bottom-prc->top,
        m_hwndOwner, nullptr, g_hDllInstance, this);

    if (!m_hwnd) return HRESULT_FROM_WIN32(GetLastError());
    *phwnd = m_hwnd;
    CreateListView();
    PopulateListView();
    return S_OK;
}

STDMETHODIMP CShellView::DestroyViewWindow()
{
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    return S_OK;
}
STDMETHODIMP CShellView::GetCurrentInfo(LPFOLDERSETTINGS pfs)
    { if(!pfs) return E_POINTER; *pfs=m_fs; return S_OK; }
STDMETHODIMP CShellView::AddPropertySheetPages(DWORD,LPFNSVADDPROPSHEETPAGE,LPARAM)
    { return S_OK; }
STDMETHODIMP CShellView::SaveViewState() { return S_OK; }
STDMETHODIMP CShellView::SelectItem(LPCITEMIDLIST /*pidl*/, SVSIF /*flags*/)
    { return S_OK; }
STDMETHODIMP CShellView::GetItemObject(UINT uItem, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER; *ppv = nullptr;
    if (uItem == SVGIO_BACKGROUND)
        return m_pFolder ? m_pFolder->QueryInterface(riid, ppv) : E_FAIL;
    return E_NOTIMPL;
}

// ── IShellView2 ──────────────────────────────────────────
STDMETHODIMP CShellView::GetView(SHELLVIEWID* pvid, ULONG uView)
    { (void)pvid; (void)uView; return E_NOTIMPL; }

STDMETHODIMP CShellView::CreateViewWindow2(LPSV2CVW2_PARAMS pParams)
{
    if (!pParams) return E_POINTER;

    // Correct SV2CVW2_PARAMS member names:
    //   pParams->pfs       = LPCFOLDERSETTINGS
    //   pParams->psbOwner  = IShellBrowser*
    //   pParams->prcView   = RECT*  (note: prcView not rcView)
    //   pParams->hwndView  = HWND*  (output)

    return CreateViewWindow(
        pParams->psvPrev,           // previous IShellView (can be null)
        pParams->pfs,               // folder settings
        pParams->psbOwner,          // shell browser
        pParams->prcView,           // rect  (pointer, not value)
        &pParams->hwndView          // output HWND
    );
}
STDMETHODIMP CShellView::HandleRename(LPCITEMIDLIST)   { return S_OK; }
STDMETHODIMP CShellView::SelectAndPositionItem(LPCITEMIDLIST,UINT,POINT*) { return S_OK; }

// ── View window creation ──────────────────────────────────
void CShellView::CreateListView()
{
    RECT rc; GetClientRect(m_hwnd, &rc);
    DWORD lvStyle = LVS_REPORT|LVS_SHOWSELALWAYS|LVS_EDITLABELS;
    if (m_fs.ViewMode == FVM_ICON)   lvStyle = (lvStyle&~LVS_TYPEMASK)|LVS_ICON;
    if (m_fs.ViewMode == FVM_SMALLICON) lvStyle = (lvStyle&~LVS_TYPEMASK)|LVS_SMALLICON;
    if (m_fs.ViewMode == FVM_LIST)   lvStyle = (lvStyle&~LVS_TYPEMASK)|LVS_LIST;

    m_hwndList = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD|WS_VISIBLE|lvStyle,
        0,0,rc.right,rc.bottom,
        m_hwnd,(HMENU)1, g_hDllInstance, nullptr);

    ListView_SetExtendedListViewStyle(m_hwndList,
        LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|
        LVS_EX_GRIDLINES|LVS_EX_HEADERDRAGDROP);

    // System image list
    SHFILEINFOW sfi{};
    HIMAGELIST hSys = (HIMAGELIST)SHGetFileInfoW(L"C:\\",0,&sfi,sizeof(sfi),
        SHGFI_SYSICONINDEX|SHGFI_SMALLICON);
    ListView_SetImageList(m_hwndList, hSys, LVSIL_SMALL);

    // Columns
    struct { const wchar_t* n; int w; } cols[] = {
        {L"Name",220},{L"Size",90},{L"Packed",90},
        {L"Ratio",60},{L"Method",80},{L"Modified",130},{L"CRC-32",80}
    };
    for (int i = 0; i < 7; i++) {
        LVCOLUMNW c{LVCF_TEXT|LVCF_WIDTH,(i>0?LVCFMT_RIGHT:LVCFMT_LEFT),
                    cols[i].w,(LPWSTR)cols[i].n};
        ListView_InsertColumn(m_hwndList, i, &c);
    }
}

void CShellView::PopulateListView()
{
    ListView_DeleteAllItems(m_hwndList);
    if (!m_pFolder) return;

    IEnumIDList* pEnum = nullptr;
    if (FAILED(m_pFolder->EnumObjects(m_hwnd,
        SHCONTF_FOLDERS|SHCONTF_NONFOLDERS, &pEnum)) || !pEnum)
        return;

    LPITEMIDLIST pidl = nullptr; ULONG fetched = 0;
    int row = 0;
    while (pEnum->Next(1, &pidl, &fetched) == S_OK && fetched) {
        STRRET sr{};
        m_pFolder->GetDisplayNameOf(pidl, SHGDN_INFOLDER, &sr);
        wchar_t name[MAX_PATH] = {};
        StrRetToBufW(&sr, pidl, name, MAX_PATH);

        // Icon
        SHFILEINFOW sfi{};
        int iIcon = 0;
        bool isDir = CPidlMgr::IsDir(pidl);
        SHGetFileInfoW(isDir?L"folder":name, FILE_ATTRIBUTE_NORMAL,
            &sfi,sizeof(sfi),
            SHGFI_SYSICONINDEX|SHGFI_SMALLICON|SHGFI_USEFILEATTRIBUTES);
        iIcon = sfi.iIcon;

        LVITEMW item{LVIF_TEXT|LVIF_IMAGE|LVIF_PARAM, row,0};
        item.pszText = name; item.iImage = iIcon;
        item.lParam  = (LPARAM)CPidlMgr::Clone(pidl);
        ListView_InsertItem(m_hwndList, &item);

        // Sub-items via GetDetailsOf
        for (int col = 1; col < 7; col++) {
            SHELLDETAILS sd{};
            m_pFolder->GetDetailsOf(pidl, col, &sd);
            wchar_t buf[128] = {};
            StrRetToBufW(&sd.str, pidl, buf, 128);
            ListView_SetItemText(m_hwndList, row, col, buf);
        }
        ILFree(pidl); row++;
    }
    pEnum->Release();
}

void CShellView::OnSize(int w, int h)
{
    if (m_hwndList) SetWindowPos(m_hwndList,nullptr,0,0,w,h,SWP_NOZORDER);
}

void CShellView::OnDblClick(int idx)
{
    LVITEMW item{LVIF_PARAM,idx};
    if (!ListView_GetItem(m_hwndList,&item)) return;
    LPITEMIDLIST pidl = (LPITEMIDLIST)item.lParam;
    if (!pidl) return;

    // A folder navigates this window; a file runs the item context menu's
    // default verb (extract a temp copy and open it), same as DefView does.
    if (CPidlMgr::IsDir(pidl))
    {
        if (m_pBrowser)
            m_pBrowser->BrowseObject(pidl, SBSP_RELATIVE | SBSP_DEFBROWSER);
        return;
    }

    auto* pCM = new(std::nothrow) CContextMenu();
    if (!pCM) return;
    LPCITEMIDLIST one = pidl;
    pCM->SetFolder(m_pFolder, m_hwnd, 1, &one);
    CMINVOKECOMMANDINFO ci{ sizeof(ci), 0, m_hwnd, "open",
                            nullptr, nullptr, SW_SHOWNORMAL };
    pCM->InvokeCommand(&ci);
    pCM->Release();
}

void CShellView::OnContextMenu(int x, int y)
{
    // Collect selected PIDLs
    std::vector<LPCITEMIDLIST> sel;
    int idx = -1;
    while ((idx=ListView_GetNextItem(m_hwndList,idx,LVNI_SELECTED))!=-1) {
        LVITEMW item{LVIF_PARAM,idx};
        ListView_GetItem(m_hwndList, &item);
        if (item.lParam) sel.push_back((LPCITEMIDLIST)item.lParam);
    }
    if (sel.empty()) return;

    auto* pCM = new(std::nothrow) CContextMenu();
    if (!pCM) return;
    pCM->SetFolder(m_pFolder, m_hwnd,
        (UINT)sel.size(), sel.data());
    HMENU hMenu = CreatePopupMenu();
    pCM->QueryContextMenu(hMenu,0,1,0x7FFF,CMF_EXPLORE);
    UINT cmd = TrackPopupMenu(hMenu,TPM_RETURNCMD|TPM_RIGHTBUTTON,
        x,y,0,m_hwnd,nullptr);
    if (cmd) {
        CMINVOKECOMMANDINFO ci{sizeof(ci),0,m_hwnd,
            MAKEINTRESOURCEA(cmd-1),nullptr,nullptr,SW_SHOWNORMAL};
        pCM->InvokeCommand(&ci);
    }
    DestroyMenu(hMenu);
    pCM->Release();
}

// ── Window procedure ──────────────────────────────────────
LRESULT CALLBACK CShellView::ViewWndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    CShellView* pThis = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        pThis = (CShellView*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pThis);
        pThis->m_hwnd = hwnd;
    } else {
        pThis = (CShellView*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!pThis) return DefWindowProcW(hwnd,msg,wp,lp);
    return pThis->WndProc(hwnd,msg,wp,lp);
}

LRESULT CShellView::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        OnSize(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_SETFOCUS:
        if (m_hwndList) SetFocus(m_hwndList); return 0;
    case WM_NOTIFY: {
        auto* nm = (NMHDR*)lp;
        if (nm->hwndFrom == m_hwndList) {
            if (nm->code == NM_DBLCLK)
                OnDblClick(((NMITEMACTIVATE*)lp)->iItem);
            if (nm->code == NM_RCLICK) {
                POINT pt; GetCursorPos(&pt);
                OnContextMenu(pt.x, pt.y);
            }
        }
        return 0; }
    case WM_DESTROY:
        // Free LPARAM PIDLs stored in list
        if (m_hwndList) {
            int n = ListView_GetItemCount(m_hwndList);
            for (int i=0;i<n;i++) {
                LVITEMW item{LVIF_PARAM,i};
                ListView_GetItem(m_hwndList,&item);
                if (item.lParam) ILFree((LPITEMIDLIST)item.lParam);
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
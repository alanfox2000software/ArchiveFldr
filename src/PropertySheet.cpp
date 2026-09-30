// PropertySheet.cpp
#include "stdafx.h"
#include "PropertySheet.h"
#include "ArchiveEngine.h"
#include "GUIDs.h"
#include "res/resource.h"

CPropertySheet::CPropertySheet()  { InterlockedIncrement(&g_cDllRefCount); }
CPropertySheet::~CPropertySheet() { InterlockedDecrement(&g_cDllRefCount); }

STDMETHODIMP CPropertySheet::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER; *ppv = nullptr;
    if (IsEqualIID(riid,IID_IUnknown)||
        IsEqualIID(riid,IID_IShellPropSheetExt))
    { *ppv=static_cast<IShellPropSheetExt*>(this); AddRef(); return S_OK; }
    if (IsEqualIID(riid,IID_IShellExtInit))
    { *ppv=static_cast<IShellExtInit*>(this); AddRef(); return S_OK; }
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CPropertySheet::AddRef()
    { return InterlockedIncrement(&m_cRef); }
STDMETHODIMP_(ULONG) CPropertySheet::Release()
    { ULONG n=InterlockedDecrement(&m_cRef); if(!n) delete this; return n; }

STDMETHODIMP CPropertySheet::Initialize(
    LPCITEMIDLIST /*pidl*/, IDataObject* pdtobj, HKEY /*hk*/)
{
    if (!pdtobj) return E_INVALIDARG;
    FORMATETC fe{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (FAILED(pdtobj->GetData(&fe,&sm))) return E_FAIL;
    HDROP hDrop = (HDROP)GlobalLock(sm.hGlobal);
    if (hDrop) {
        wchar_t path[MAX_PATH*2]={};
        DragQueryFileW(hDrop,0,path,MAX_PATH*2);
        m_filePath = path;
        GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
    return m_filePath.empty() ? E_FAIL : S_OK;
}

STDMETHODIMP CPropertySheet::AddPages(
    LPFNSVADDPROPSHEETPAGE pfnAddPage, LPARAM lParam)
{
    // Load archive info now (on background thread in production)
    auto engine = CreateArchiveEngine(m_filePath);
    if (engine && engine->Open(m_filePath)) {
        m_formatName  = engine->GetFormatName();
        m_fileCount   = engine->GetFileCount();
        m_totalSize   = engine->GetTotalSize();
        m_packedSize  = engine->GetPackedSize();
        m_comment     = engine->GetComment();
        m_isReadOnly  = engine->IsReadOnly();
        m_isEncrypted = false;  // engine->IsEncrypted()
        m_isSolid     = false;  // engine->IsSolid()
    }

    PROPSHEETPAGEW psp{sizeof(psp)};
    psp.dwFlags      = PSP_USETITLE | PSP_DEFAULT;
    psp.hInstance    = g_hDllInstance;
    psp.pszTemplate  = MAKEINTRESOURCEW(IDD_PAGE_ABOUT); // reuse layout
    psp.pszTitle     = L"Archive";
    psp.pfnDlgProc   = PageDlgProc;
    psp.lParam       = (LPARAM)this;
    psp.pfnCallback  = nullptr;

    HPROPSHEETPAGE hPage = CreatePropertySheetPageW(&psp);
    if (hPage) pfnAddPage(hPage, lParam);
    return S_OK;
}

STDMETHODIMP CPropertySheet::ReplacePage(UINT,LPFNSVADDPROPSHEETPAGE,LPARAM)
    { return E_NOTIMPL; }

INT_PTR CALLBACK CPropertySheet::PageDlgProc(
    HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    CPropertySheet* pThis = nullptr;
    if (msg == WM_INITDIALOG) {
        auto* psp = (PROPSHEETPAGEW*)lp;
        pThis = (CPropertySheet*)psp->lParam;
        SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)pThis);
        pThis->InitPage(hDlg);
        pThis->FillPage(hDlg);
        return TRUE;
    }
    pThis = (CPropertySheet*)GetWindowLongPtrW(hDlg, DWLP_USER);
    if (!pThis) return FALSE;

    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_BTN_CHECK_UPDATE) {
            // Retest
            auto engine = CreateArchiveEngine(pThis->m_filePath);
            if (engine && engine->Open(pThis->m_filePath)) {
                bool ok = engine->Test(nullptr);
                MessageBoxW(hDlg,
                    ok ? L"Archive integrity: OK"
                       : L"Archive integrity: FAILED!",
                    L"Test Archive",
                    ok ? MB_ICONINFORMATION : MB_ICONERROR);
            }
        }
        break;
    case WM_NOTIFY: {
        auto* nm = (NMHDR*)lp;
        if (nm->code == PSN_APPLY) SetWindowLongPtrW(hDlg,DWLP_MSGRESULT,PSNRET_NOERROR);
        break; }
    }
    return FALSE;
}

void CPropertySheet::InitPage(HWND hDlg)
{
    // Dynamically create labels since we reuse IDD_PAGE_ABOUT layout.
    // In a proper .rc this would be a dedicated dialog template.
    // Here we build a simple info panel via SetDlgItemText calls.
    (void)hDlg;
}

void CPropertySheet::FillPage(HWND hDlg)
{
    // Build property text
    wchar_t szTotal[32], szPacked[32];
    StrFormatByteSizeW(m_totalSize,  szTotal,  32);
    StrFormatByteSizeW(m_packedSize, szPacked, 32);

    double ratio = m_totalSize > 0 ?
        100.0*(1.0-(double)m_packedSize/m_totalSize) : 0.0;

    wchar_t info[1024];
    swprintf_s(info, 1024,
        L"Format:       %s\r\n"
        L"Files:        %llu\r\n"
        L"Total Size:   %s\r\n"
        L"Packed Size:  %s\r\n"
        L"Ratio:        %.1f%%\r\n"
        L"Read-Only:    %s\r\n"
        L"Encrypted:    %s\r\n"
        L"Solid:        %s\r\n"
        L"Comment:      %s",
        m_formatName.c_str(),
        m_fileCount,
        szTotal, szPacked, ratio,
        m_isReadOnly  ? L"Yes" : L"No",
        m_isEncrypted ? L"Yes" : L"No",
        m_isSolid     ? L"Yes" : L"No",
        m_comment.empty() ? L"(none)" : m_comment.c_str());

    SetDlgItemTextW(hDlg, IDC_STATIC_DESC, info);
    SetDlgItemTextW(hDlg, IDC_STATIC_NAME,
        PathFindFileNameW(m_filePath.c_str()));
}
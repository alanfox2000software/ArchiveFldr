// PropertySheet.h — IShellPropSheetExt
// Adds an "Archive" tab to the file Properties dialog
#pragma once
#include "stdafx.h"

class CPropertySheet :
    public IShellPropSheetExt,
    public IShellExtInit
{
public:
    CPropertySheet();

    STDMETHODIMP QueryInterface(REFIID, void**) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;

    // IShellExtInit
    STDMETHODIMP Initialize(LPCITEMIDLIST, IDataObject*, HKEY) override;

    // IShellPropSheetExt
    STDMETHODIMP AddPages       (LPFNSVADDPROPSHEETPAGE, LPARAM) override;
    STDMETHODIMP ReplacePage    (UINT, LPFNSVADDPROPSHEETPAGE, LPARAM) override;

private:
    ~CPropertySheet();

    static INT_PTR CALLBACK PageDlgProc(HWND,UINT,WPARAM,LPARAM);
    void InitPage (HWND hDlg);
    void FillPage (HWND hDlg);

    long         m_cRef     = 1;
    std::wstring m_filePath;

    // Archive info cache
    std::wstring m_formatName;
    uint64_t     m_fileCount  = 0;
    uint64_t     m_totalSize  = 0;
    uint64_t     m_packedSize = 0;
    std::wstring m_comment;
    bool         m_isReadOnly = false;
    bool         m_isEncrypted= false;
    bool         m_isSolid    = false;
};
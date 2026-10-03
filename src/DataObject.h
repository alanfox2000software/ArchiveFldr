// DataObject.h
// IDataObject for items INSIDE an archive — this is what makes copy (Ctrl+C)
// and drag-out of the ArchiveFldr view produce real files.
//
// Formats served:
//   CFSTR_FILEDESCRIPTORW  flattened list of the selection (folders expand)
//   CFSTR_FILECONTENTS     one IStream per descriptor, extracted on demand
//   CF_HDROP               same selection staged in a temp folder
//   CFSTR_SHELLIDLIST      the shell's own identity format (CIDA)
//   CFSTR_PREFERREDDROPEFFECT = DROPEFFECT_COPY (an archive item is never
//                               "moved" out: we cannot delete the original)
#pragma once
#include "stdafx.h"
#include "ArchiveEngine.h"

class CShellFolder;

class CArchiveDataObject : public IDataObject
{
public:
    // Builds a data object for `cidl` children of `folder`.
    static HRESULT Create(CShellFolder* folder, UINT cidl,
                          LPCITEMIDLIST* apidl, REFIID riid, void** ppv);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID, void**) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;

    // IDataObject
    STDMETHODIMP GetData        (FORMATETC*, STGMEDIUM*) override;
    STDMETHODIMP GetDataHere    (FORMATETC*, STGMEDIUM*) override;
    STDMETHODIMP QueryGetData   (FORMATETC*)             override;
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override;
    STDMETHODIMP SetData        (FORMATETC*, STGMEDIUM*, BOOL) override;
    STDMETHODIMP EnumFormatEtc  (DWORD, IEnumFORMATETC**) override;
    STDMETHODIMP DAdvise        (FORMATETC*, DWORD, IAdviseSink*, DWORD*) override;
    STDMETHODIMP DUnadvise      (DWORD) override;
    STDMETHODIMP EnumDAdvise    (IEnumSTATDATA**) override;

private:
    CArchiveDataObject();
    ~CArchiveDataObject();

    struct Item {
        ArchiveEntry entry;
        std::wstring rel;      // path relative to the selection's parent
        std::wstring staged;   // on-disk path once extracted ("" = not yet)
    };
    struct Stored {            // whatever the shell hands back via SetData
        FORMATETC fe;
        STGMEDIUM sm;
    };

    HRESULT RenderIdList      (STGMEDIUM* pmed);
    HRESULT RenderDescriptor  (STGMEDIUM* pmed);
    HRESULT RenderContents    (LONG index, STGMEDIUM* pmed);
    HRESULT RenderHDrop       (STGMEDIUM* pmed);
    HRESULT RenderDropEffect  (STGMEDIUM* pmed);

    bool EnsureStaged(Item& it);
    bool EnsureTempRoot();

    long  m_cRef = 1;

    std::shared_ptr<IArchiveEngine> m_engine;
    std::wstring              m_archivePath;
    std::wstring              m_internalPath;  // folder the selection lives in
    std::wstring              m_tempRoot;

    std::vector<Item>         m_items;   // flattened, one per descriptor
    std::vector<ArchiveEntry> m_roots;   // what the user actually selected

    LPITEMIDLIST              m_pidlFolder = nullptr;
    std::vector<LPITEMIDLIST> m_pidlItems;

    std::vector<Stored>       m_stored;
};

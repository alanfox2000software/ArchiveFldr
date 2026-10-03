// DropTarget.h — legacy drop-handler COM class
#pragma once
#include "stdafx.h"

// ArchiveFldr no longer accepts files dragged or pasted into an archive.
// The class remains as a defensive no-op so an old DropHandler registration
// cannot modify an archive before the next registration cleanup runs.
class CDropTarget final : public IDropTarget,
                          public IPersistFile
{
public:
    CDropTarget();

    STDMETHODIMP QueryInterface(REFIID, void**) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP DragEnter(IDataObject*, DWORD, POINTL, DWORD*) override;
    STDMETHODIMP DragOver (DWORD, POINTL, DWORD*) override;
    STDMETHODIMP DragLeave() override;
    STDMETHODIMP Drop(IDataObject*, DWORD, POINTL, DWORD*) override;

    STDMETHODIMP GetClassID(CLSID*) override;
    STDMETHODIMP IsDirty() override;
    STDMETHODIMP Load(LPCOLESTR, DWORD) override;
    STDMETHODIMP Save(LPCOLESTR, BOOL) override;
    STDMETHODIMP SaveCompleted(LPCOLESTR) override;
    STDMETHODIMP GetCurFile(LPOLESTR*) override;

private:
    ~CDropTarget();

    long         m_cRef = 1;
    std::wstring m_archive;
};

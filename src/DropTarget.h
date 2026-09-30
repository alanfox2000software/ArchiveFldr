// DropTarget.h — IDropTarget for dropping files INTO an archive
#pragma once
#include "stdafx.h"

class CShellFolder;

class CDropTarget : public IDropTarget
{
public:
    CDropTarget();
    void SetFolder(CShellFolder* pFolder);

    STDMETHODIMP QueryInterface(REFIID, void**) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP DragEnter(IDataObject*, DWORD, POINTL, DWORD*) override;
    STDMETHODIMP DragOver (DWORD,        POINTL, DWORD*) override;
    STDMETHODIMP DragLeave()                             override;
    STDMETHODIMP Drop     (IDataObject*, DWORD, POINTL, DWORD*) override;

private:
    ~CDropTarget();

    bool  CanAccept(IDataObject* pObj) const;
    DWORD ComputeEffect(DWORD grfKeyState) const;
    HRESULT PerformDrop(IDataObject* pObj);

    long           m_cRef          = 1;
    CShellFolder*  m_pFolder       = nullptr;
    bool           m_canDrop       = false;
    DWORD          m_lastEffect    = DROPEFFECT_NONE;
};
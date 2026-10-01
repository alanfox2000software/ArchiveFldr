// DropTarget.h — IDropTarget for dropping files INTO an archive folder
#pragma once
#include "stdafx.h"
#include "ArchiveEngine.h"

class CShellFolder;

// Shared drop logic, used both by this object (items/background of the view)
// and by CShellFolder's own IDropTarget implementation.
namespace ArchiveDrop {

// What the user may do with this payload over an archive folder. Always
// DROPEFFECT_COPY or NONE: ShellNSE never asks the source to delete its
// originals, because it cannot guarantee the data landed in the archive.
DWORD EffectFor(IDataObject* pdo);

// Add everything the data object carries to `folder`'s current directory.
// Explains itself (and does nothing) when the engine cannot write.
HRESULT Perform(HWND hwnd, CShellFolder* folder, IDataObject* pdo);

} // namespace ArchiveDrop

class CDropTarget : public IDropTarget
{
public:
    CDropTarget();
    void SetFolder(CShellFolder* pFolder);
    void SetSite(HWND hwnd) { m_hwnd = hwnd; }

    STDMETHODIMP QueryInterface(REFIID, void**) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP DragEnter(IDataObject*, DWORD, POINTL, DWORD*) override;
    STDMETHODIMP DragOver (DWORD,        POINTL, DWORD*) override;
    STDMETHODIMP DragLeave()                             override;
    STDMETHODIMP Drop     (IDataObject*, DWORD, POINTL, DWORD*) override;

private:
    ~CDropTarget();

    long           m_cRef       = 1;
    HWND           m_hwnd       = nullptr;
    CShellFolder*  m_pFolder    = nullptr;
    DWORD          m_lastEffect = DROPEFFECT_NONE;
    IDataObject*   m_pDataObj   = nullptr;   // held between Enter and Drop
};

// DropTarget.h — IDropTarget for dropping files INTO an archive folder
#pragma once
#include "stdafx.h"
#include "ArchiveEngine.h"

class CShellFolder;

// Shared drop logic, used both by this object (items/background of the view)
// and by CShellFolder's own IDropTarget implementation.
namespace ArchiveDrop {

// What the user may do with this payload over an archive folder. Always
// DROPEFFECT_COPY or NONE: ArchiveFldr never asks the source to delete its
// originals, because it cannot guarantee the data landed in the archive.
DWORD EffectFor(IDataObject* pdo);

// Add everything the data object carries to `folder`'s current directory.
// Explains itself (and does nothing) when the engine cannot write.
HRESULT Perform(HWND hwnd, CShellFolder* folder, IDataObject* pdo);

} // namespace ArchiveDrop

// IPersistFile is not decoration.
//
// This class is registered as the shell's DropHandler for every archive
// type (shellex\DropHandler, written by RegisterShellExOnBase). A drop
// handler is created by CLSID and then told which file it is standing in
// for — and the shell does that through IPersistFile::Load, before the
// first DragEnter. Without it the QueryInterface fails, the shell drops
// the handler on the floor, and dragging files onto an archive icon
// silently does nothing at all.
class CDropTarget : public IDropTarget,
                    public IPersistFile
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

    // ── IPersist / IPersistFile ──────────────────────────
    STDMETHODIMP GetClassID(CLSID* pClassID) override;
    STDMETHODIMP IsDirty() override;
    STDMETHODIMP Load(LPCOLESTR pszFileName, DWORD dwMode) override;
    STDMETHODIMP Save(LPCOLESTR pszFileName, BOOL fRemember) override;
    STDMETHODIMP SaveCompleted(LPCOLESTR pszFileName) override;
    STDMETHODIMP GetCurFile(LPOLESTR* ppszFileName) override;

private:
    ~CDropTarget();

    long           m_cRef       = 1;
    HWND           m_hwnd       = nullptr;
    CShellFolder*  m_pFolder    = nullptr;
    DWORD          m_lastEffect = DROPEFFECT_NONE;
    IDataObject*   m_pDataObj   = nullptr;   // held between Enter and Drop
    std::wstring   m_archive;                // set by IPersistFile::Load
};

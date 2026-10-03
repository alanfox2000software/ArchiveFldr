// ClassFactory.h — Generic COM Class Factory
#pragma once
#include "stdafx.h"

// Creator function prototype
using FnCreate = HRESULT(*)(REFIID riid, LPVOID* ppv);

// ─────────────────────────────────────────────────────────
// CClassFactory
// Implements IClassFactory; delegates object creation to a
// stateless creator function so we only need one factory class.
// ─────────────────────────────────────────────────────────
class CClassFactory final : public IClassFactory
{
public:
    explicit CClassFactory(FnCreate fn) noexcept
        : m_fnCreate(fn), m_cRef(1)
    {
        InterlockedIncrement(&g_cDllRefCount);
    }

    // ── IUnknown ─────────────────────────────────────────
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;

    // ── IClassFactory ────────────────────────────────────
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter,
                                REFIID    riid,
                                void**    ppv) override;
    STDMETHODIMP LockServer(BOOL fLock) override;

    // ── Static creator helpers ────────────────────────────
    static HRESULT CreateShellFolder      (REFIID, LPVOID*);
    static HRESULT CreateDropTarget       (REFIID, LPVOID*);
#ifndef ARCHIVEFLDR_NO_VISTA_HANDLERS
    static HRESULT CreateThumbnailProvider(REFIID, LPVOID*);
    static HRESULT CreatePreviewHandler   (REFIID, LPVOID*);
#endif // ARCHIVEFLDR_NO_VISTA_HANDLERS

private:
    ~CClassFactory() { InterlockedDecrement(&g_cDllRefCount); }
    FnCreate m_fnCreate;
    long     m_cRef;
};
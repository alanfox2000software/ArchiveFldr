// IconOverlay.h — IShellIconOverlayIdentifier
#pragma once
#include "stdafx.h"

class CIconOverlay : public IShellIconOverlayIdentifier
{
public:
    CIconOverlay();

    STDMETHODIMP QueryInterface(REFIID, void**) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;

    // IShellIconOverlayIdentifier
    STDMETHODIMP IsMemberOf    (LPCWSTR pwszPath, DWORD dwAttrib) override;
    STDMETHODIMP GetOverlayInfo(LPWSTR  pwszIconFile, int cchMax,
                                int*    pIndex, DWORD* pdwFlags) override;
    STDMETHODIMP GetPriority   (int*    pPriority) override;

private:
    ~CIconOverlay();
    long m_cRef = 1;
};
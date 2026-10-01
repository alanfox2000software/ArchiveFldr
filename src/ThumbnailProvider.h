// ThumbnailProvider.h — IThumbnailProvider
// Generates a thumbnail for archive files showing a
// composite of the first image/icon found inside.
#pragma once
#include "stdafx.h"

// Note: IInitializeWithStream is deliberately NOT implemented. The shell
// prefers it over IInitializeWithFile, and advertising it only to fail the
// initialisation call wastes a surrogate activation per archive.
class CThumbnailProvider :
    public IThumbnailProvider,
    public IInitializeWithFile
{
public:
    CThumbnailProvider();

    STDMETHODIMP QueryInterface(REFIID, void**) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;

    // IInitializeWithFile
    STDMETHODIMP Initialize(LPCWSTR pszFilePath, DWORD grfMode) override;

    // IThumbnailProvider
    STDMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override;

private:
    ~CThumbnailProvider();

    HBITMAP CreateArchiveThumbnail(UINT cx);
    HBITMAP RenderThumbnailGDI(UINT cx,
                                const std::wstring& archiveName,
                                const std::wstring& formatName,
                                uint64_t fileBytes);

    long         m_cRef     = 1;
    std::wstring m_filePath;
};
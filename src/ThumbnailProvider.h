// ThumbnailProvider.h — IThumbnailProvider
// Generates a thumbnail for archive files showing a
// composite of the first image/icon found inside.
#pragma once
#include "stdafx.h"

class CThumbnailProvider :
    public IThumbnailProvider,
    public IInitializeWithFile,
    public IInitializeWithStream
{
public:
    CThumbnailProvider();

    STDMETHODIMP QueryInterface(REFIID, void**) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;

    // IInitializeWithFile
    STDMETHODIMP Initialize(LPCWSTR pszFilePath, DWORD grfMode) override;

    // IInitializeWithStream
    STDMETHODIMP Initialize(IStream* pstream, DWORD grfMode) override;

    // IThumbnailProvider
    STDMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override;

private:
    ~CThumbnailProvider();

    HBITMAP CreateArchiveThumbnail(UINT cx);
    HBITMAP RenderThumbnailGDI(UINT cx,
                                const std::wstring& archiveName,
                                const std::wstring& formatName,
                                UINT fileCount);

    long         m_cRef     = 1;
    std::wstring m_filePath;
};
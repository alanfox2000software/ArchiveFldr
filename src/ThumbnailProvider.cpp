// ThumbnailProvider.cpp
#include "stdafx.h"
#include "ThumbnailProvider.h"
#include "ArchiveEngine.h"
#include "GUIDs.h"

CThumbnailProvider::CThumbnailProvider()
    { InterlockedIncrement(&g_cDllRefCount); }
CThumbnailProvider::~CThumbnailProvider()
    { InterlockedDecrement(&g_cDllRefCount); }

STDMETHODIMP CThumbnailProvider::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER; *ppv = nullptr;
    if (IsEqualIID(riid,IID_IUnknown)||
        IsEqualIID(riid,IID_IThumbnailProvider))
    { *ppv=static_cast<IThumbnailProvider*>(this); AddRef(); return S_OK; }
    if (IsEqualIID(riid,IID_IInitializeWithFile))
    { *ppv=static_cast<IInitializeWithFile*>(this); AddRef(); return S_OK; }
    if (IsEqualIID(riid,IID_IInitializeWithStream))
    { *ppv=static_cast<IInitializeWithStream*>(this); AddRef(); return S_OK; }
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CThumbnailProvider::AddRef()
    { return InterlockedIncrement(&m_cRef); }
STDMETHODIMP_(ULONG) CThumbnailProvider::Release()
    { ULONG n=InterlockedDecrement(&m_cRef); if(!n) delete this; return n; }

STDMETHODIMP CThumbnailProvider::Initialize(LPCWSTR pszFilePath, DWORD /*grfMode*/)
{
    if (!pszFilePath) return E_POINTER;
    m_filePath = pszFilePath; return S_OK;
}
STDMETHODIMP CThumbnailProvider::Initialize(IStream* /*pstream*/, DWORD /*grfMode*/)
{
    // Stream init: would read from stream; simplified here
    return E_NOTIMPL;
}

STDMETHODIMP CThumbnailProvider::GetThumbnail(
    UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha)
{
    if (!phbmp||!pdwAlpha) return E_POINTER;
    *phbmp = nullptr; *pdwAlpha = WTSAT_UNKNOWN;

    if (m_filePath.empty()) return E_FAIL;

    *phbmp    = CreateArchiveThumbnail(cx);
    *pdwAlpha = WTSAT_ARGB;
    return *phbmp ? S_OK : E_FAIL;
}

// ── GDI+ thumbnail rendering ──────────────────────────────
HBITMAP CThumbnailProvider::CreateArchiveThumbnail(UINT cx)
{
    auto engine = CreateArchiveEngine(m_filePath);
    UINT fileCount = 0;
    std::wstring formatName = L"Archive";
    if (engine && engine->Open(m_filePath)) {
        fileCount  = (UINT)engine->GetFileCount();
        formatName = engine->GetFormatName();
    }
    std::wstring archName = PathFindFileNameW(m_filePath.c_str());
    return RenderThumbnailGDI(cx, archName, formatName, fileCount);
}

HBITMAP CThumbnailProvider::RenderThumbnailGDI(
    UINT cx, const std::wstring& archiveName,
    const std::wstring& formatName, UINT fileCount)
{
    // Create a 32bpp DIB section
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = (LONG)cx;
    bmi.bmiHeader.biHeight      = -(LONG)cx;  // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pvBits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hBmp) return nullptr;

    HDC hdcMem = CreateCompatibleDC(nullptr);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBmp);

    // GDI+ rendering
    Gdiplus::Graphics g(hdcMem);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    float W = (float)cx, H = (float)cx;

    // Background gradient
    Gdiplus::LinearGradientBrush bgBrush(
        Gdiplus::PointF(0,0), Gdiplus::PointF(W,H),
        Gdiplus::Color(255,35,35,45),
        Gdiplus::Color(255,55,55,75));
    g.FillRectangle(&bgBrush, 0.0f, 0.0f, W, H);

    // Archive icon box
    float boxPad = W*0.12f;
    float boxW   = W - boxPad*2;
    float boxH   = H * 0.52f;
    Gdiplus::SolidBrush boxBrush(Gdiplus::Color(200,255,165,0));
    Gdiplus::GraphicsPath path;
    float r = W*0.06f;
    path.AddArc(boxPad,       boxPad,        r*2,r*2, 180,90);
    path.AddArc(boxPad+boxW-r*2,boxPad,      r*2,r*2, 270,90);
    path.AddArc(boxPad+boxW-r*2,boxPad+boxH-r*2,r*2,r*2,0,90);
    path.AddArc(boxPad,         boxPad+boxH-r*2,r*2,r*2,90,90);
    path.CloseFigure();
    g.FillPath(&boxBrush, &path);

    // Zip lines decoration
    Gdiplus::Pen linePen(Gdiplus::Color(180,255,255,255), W*0.025f);
    for (int i=0;i<4;i++) {
        float y = boxPad + boxH*0.2f + i*(boxH*0.18f);
        g.DrawLine(&linePen, boxPad+boxW*0.2f, y, boxPad+boxW*0.8f, y);
    }

    // Format label
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font fontLarge(&ff, W*0.14f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font fontSmall(&ff, W*0.09f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush white(Gdiplus::Color(255,255,255,255));
    Gdiplus::SolidBrush gray(Gdiplus::Color(200,180,180,200));

    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);

    // Format name centered in box
    Gdiplus::RectF rcFmt(boxPad, boxPad+boxH*0.1f, boxW, boxH*0.5f);
    g.DrawString(formatName.c_str(), -1, &fontLarge, rcFmt, &sf, &white);

    // Archive name below box
    Gdiplus::RectF rcName(boxPad, boxPad+boxH+H*0.04f, boxW, H*0.18f);
    g.DrawString(archiveName.c_str(), -1, &fontSmall, rcName, &sf, &white);

    // File count
    wchar_t cnt[32]; swprintf_s(cnt,32,L"%u files", fileCount);
    Gdiplus::RectF rcCnt(boxPad, boxPad+boxH+H*0.24f, boxW, H*0.14f);
    g.DrawString(cnt, -1, &fontSmall, rcCnt, &sf, &gray);

    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    return hBmp;
}
// ThumbnailProvider.cpp
#include "stdafx.h"
#include "ThumbnailProvider.h"
#include "ArchiveEngine.h"
#include "GUIDs.h"
#include "Settings.h"

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
STDMETHODIMP CThumbnailProvider::GetThumbnail(
    UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha)
{
    if (!phbmp||!pdwAlpha) return E_POINTER;
    *phbmp = nullptr; *pdwAlpha = WTSAT_UNKNOWN;

    if (m_filePath.empty()) return E_FAIL;

    // Honour the user's choice, and never draw without GDI+. Returning a
    // failure here is harmless: the shell falls back to the file type icon.
    if (!Settings::Get().showThumbnails) return E_NOTIMPL;
    if (!EnsureGdiPlus())                return E_FAIL;

    *phbmp    = CreateArchiveThumbnail(cx);
    *pdwAlpha = WTSAT_ARGB;
    return *phbmp ? S_OK : E_FAIL;
}

// ── GDI+ thumbnail rendering ──────────────────────────────
static std::wstring HumanSize(uint64_t bytes)
{
    const wchar_t* kUnits[] = { L"bytes", L"KB", L"MB", L"GB", L"TB" };
    double v = (double)bytes;
    int    u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    wchar_t buf[64];
    swprintf_s(buf, 64, (u == 0) ? L"%.0f %s" : L"%.1f %s", v, kUnits[u]);
    return buf;
}

HBITMAP CThumbnailProvider::CreateArchiveThumbnail(UINT cx)
{
    // This deliberately does NOT open the archive.
    //
    // Thumbnails are produced inside the shell's surrogate host for every
    // archive in every folder the user merely looks at. Opening each one
    // loaded the 7-Zip backend and parsed the whole index just to print a
    // file count — an expensive, blocking operation in a process the user
    // never asked for, and the reason archive-heavy folders left a trail
    // of busy DllHost.exe instances behind.
    //
    // Everything drawn below comes from the directory entry instead.
    std::wstring archName = PathFindFileNameW(m_filePath.c_str());

    std::wstring label = PathFindExtensionW(m_filePath.c_str());
    if (!label.empty() && label.front() == L'.') label.erase(0, 1);
    for (auto& ch : label) ch = (wchar_t)towupper(ch);
    if (label.empty()) label = L"ARCHIVE";

    uint64_t bytes = 0;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(m_filePath.c_str(), GetFileExInfoStandard, &fad))
        bytes = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;

    return RenderThumbnailGDI(cx, archName, label, bytes);
}

HBITMAP CThumbnailProvider::RenderThumbnailGDI(
    UINT cx, const std::wstring& archiveName,
    const std::wstring& formatName, uint64_t fileBytes)
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

    // Scoped: a Gdiplus::Graphics must be destroyed, which is what flushes
    // its drawing, while the DC it was built on is still alive and still
    // has the bitmap selected into it.
    {
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

    // Size on disk — free, unlike the entry count it replaced.
    std::wstring size = HumanSize(fileBytes);
    Gdiplus::RectF rcCnt(boxPad, boxPad+boxH+H*0.24f, boxW, H*0.14f);
    g.DrawString(size.c_str(), -1, &fontSmall, rcCnt, &sf, &gray);
    }

    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    return hBmp;
}
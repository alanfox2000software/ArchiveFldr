// PreviewHandler.cpp
#include "stdafx.h"
#include "PreviewHandler.h"
#include "ArchiveEngine.h"
#include "GUIDs.h"

static const wchar_t kPreviewClass[] = L"ShellNSE_Preview";

CPreviewHandler::CPreviewHandler()
{
    ZeroMemory(&m_lf, sizeof(m_lf));
    wcscpy_s(m_lf.lfFaceName, L"Segoe UI");
    m_lf.lfHeight = -13;
    InterlockedIncrement(&g_cDllRefCount);
    RegisterClass();
}
CPreviewHandler::~CPreviewHandler()
{
    DestroyPreviewWindow();
    InterlockedDecrement(&g_cDllRefCount);
}

void CPreviewHandler::RegisterClass()
{
    static bool s_done = false;
    if (s_done) return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc   = PreviewWndProc;
    wc.hInstance     = g_hDllInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName = kPreviewClass;
    wc.style         = CS_HREDRAW|CS_VREDRAW;
    RegisterClassExW(&wc);
    s_done = true;
}

// ── IUnknown ─────────────────────────────────────────────
STDMETHODIMP CPreviewHandler::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER; *ppv = nullptr;
    if (IsEqualIID(riid,IID_IUnknown)||
        IsEqualIID(riid,IID_IPreviewHandler))
    { *ppv=static_cast<IPreviewHandler*>(this); AddRef(); return S_OK; }
    if (IsEqualIID(riid,IID_IPreviewHandlerVisuals))
    { *ppv=static_cast<IPreviewHandlerVisuals*>(this); AddRef(); return S_OK; }
    if (IsEqualIID(riid,IID_IInitializeWithFile))
    { *ppv=static_cast<IInitializeWithFile*>(this); AddRef(); return S_OK; }
    if (IsEqualIID(riid,IID_IInitializeWithStream))
    { *ppv=static_cast<IInitializeWithStream*>(this); AddRef(); return S_OK; }
    if (IsEqualIID(riid,IID_IOleWindow))
    { *ppv=static_cast<IOleWindow*>(this); AddRef(); return S_OK; }
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) CPreviewHandler::AddRef()
    { return InterlockedIncrement(&m_cRef); }
STDMETHODIMP_(ULONG) CPreviewHandler::Release()
    { ULONG n=InterlockedDecrement(&m_cRef); if(!n) delete this; return n; }

// ── IInitializeWithFile ───────────────────────────────────
STDMETHODIMP CPreviewHandler::Initialize(LPCWSTR pszFilePath, DWORD)
{
    if (!pszFilePath) return E_POINTER;
    m_filePath = pszFilePath;
    return S_OK;
}
STDMETHODIMP CPreviewHandler::Initialize(IStream*, DWORD) { return E_NOTIMPL; }

// ── IOleWindow ────────────────────────────────────────────
STDMETHODIMP CPreviewHandler::GetWindow(HWND* p)
    { if(!p) return E_POINTER; *p=m_hwnd; return m_hwnd?S_OK:E_FAIL; }
STDMETHODIMP CPreviewHandler::ContextSensitiveHelp(BOOL) { return S_OK; }

// ── IPreviewHandler ───────────────────────────────────────
STDMETHODIMP CPreviewHandler::SetWindow(HWND hwnd, const RECT* prc)
{
    if (!prc) return E_POINTER;
    m_hwndParent = hwnd;
    m_rc         = *prc;
    if (m_hwnd)
        SetWindowPos(m_hwnd,nullptr,
            m_rc.left, m_rc.top,
            m_rc.right-m_rc.left, m_rc.bottom-m_rc.top,
            SWP_NOZORDER|SWP_NOACTIVATE);
    return S_OK;
}
STDMETHODIMP CPreviewHandler::SetRect(const RECT* prc)
    { return SetWindow(m_hwndParent, prc); }

STDMETHODIMP CPreviewHandler::DoPreview()
{
    if (m_filePath.empty()) return E_FAIL;
    LoadEntries();
    CreatePreviewWindow();
    return S_OK;
}
STDMETHODIMP CPreviewHandler::Unload()
    { DestroyPreviewWindow(); m_rows.clear(); m_filePath.clear(); return S_OK; }
STDMETHODIMP CPreviewHandler::SetFocus()
    { if(m_hwnd) ::SetFocus(m_hwnd); return S_OK; }
STDMETHODIMP CPreviewHandler::QueryFocus(HWND* p)
    { if(!p) return E_POINTER; *p=GetFocus(); return S_OK; }
STDMETHODIMP CPreviewHandler::TranslateAccelerator(MSG*) { return S_FALSE; }

// ── IPreviewHandlerVisuals ────────────────────────────────
STDMETHODIMP CPreviewHandler::SetBackgroundColor(COLORREF c)
    { m_bgColor=c; if(m_hwnd) InvalidateRect(m_hwnd,nullptr,TRUE); return S_OK; }
STDMETHODIMP CPreviewHandler::SetFont(const LOGFONTW* plf)
    { if(plf) m_lf=*plf; return S_OK; }
STDMETHODIMP CPreviewHandler::SetTextColor(COLORREF c)
    { m_txtColor=c; if(m_hwnd) InvalidateRect(m_hwnd,nullptr,TRUE); return S_OK; }

// ── Internal ──────────────────────────────────────────────
void CPreviewHandler::LoadEntries()
{
    m_rows.clear();
    auto engine = CreateArchiveEngine(m_filePath);
    if (!engine || !engine->Open(m_filePath)) return;
    m_formatName = engine->GetFormatName();
    m_fileCount  = (UINT)engine->GetFileCount();
    m_totalSize  = engine->GetTotalSize();
    m_packedSize = engine->GetPackedSize();

    auto entries = engine->List(L"");
    for (auto& e : entries) {
        Row r;
        r.name     = e.fullPath;
        r.isDir    = e.isDirectory;
        r.method   = e.compressionMethod;
        if (e.isDirectory) {
            r.size   = L"<DIR>";
            r.packed = L"";
        } else {
            wchar_t bSz[32] = {}, bPk[32] = {};
            StrFormatByteSizeW(e.uncompressedSize, bSz, 32);
            StrFormatByteSizeW(e.compressedSize,   bPk, 32);
            r.size   = bSz;
            r.packed = bPk;
        }
        SYSTEMTIME st; FILETIME lft;
        FileTimeToLocalFileTime(&e.modifiedTime,&lft);
        FileTimeToSystemTime(&lft,&st);
        wchar_t dtbuf[32];
        swprintf_s(dtbuf,32,L"%04d-%02d-%02d",st.wYear,st.wMonth,st.wDay);
        r.modified = dtbuf;
        m_rows.push_back(std::move(r));
    }
}

void CPreviewHandler::CreatePreviewWindow()
{
    if (m_hwnd) return;
    m_hwnd = CreateWindowExW(0, kPreviewClass, L"",
        WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_CLIPCHILDREN,
        m_rc.left, m_rc.top,
        m_rc.right-m_rc.left, m_rc.bottom-m_rc.top,
        m_hwndParent, nullptr, g_hDllInstance, this);
}

void CPreviewHandler::DestroyPreviewWindow()
{
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
}

// ── Painting ──────────────────────────────────────────────
void CPreviewHandler::PaintPreview(HDC hdc, const RECT& rc)
{
    float W = (float)(rc.right-rc.left);
    float H = (float)(rc.bottom-rc.top);

    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    // Background
    BYTE r=(BYTE)GetRValue(m_bgColor),
         gg=(BYTE)GetGValue(m_bgColor),
         b=(BYTE)GetBValue(m_bgColor);
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255,r,gg,b));
    g.FillRectangle(&bgBrush, 0.0f,0.0f,W,H);

    DrawHeader(g, W, H);
    DrawTable (g, W, H);
}

void CPreviewHandler::DrawHeader(Gdiplus::Graphics& g, float W, float /*H*/)
{
    // Header bar
    Gdiplus::LinearGradientBrush hdrBrush(
        Gdiplus::PointF(0,0),Gdiplus::PointF(0,48),
        Gdiplus::Color(255,40,40,55),
        Gdiplus::Color(255,60,60,80));
    g.FillRectangle(&hdrBrush,0.0f,0.0f,W,48.0f);

    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font fBig(&ff,15,Gdiplus::FontStyleBold,Gdiplus::UnitPixel);
    Gdiplus::Font fSm (&ff, 9,Gdiplus::FontStyleRegular,Gdiplus::UnitPixel);
    Gdiplus::SolidBrush white(Gdiplus::Color(255,255,255,255));
    Gdiplus::SolidBrush gold (Gdiplus::Color(255,255,200,80));

    // Archive name
    std::wstring name = PathFindFileNameW(m_filePath.c_str());
    g.DrawString(name.c_str(),-1,&fBig,Gdiplus::PointF(12,6),&white);

    // Stats
    wchar_t stat[128];
    swprintf_s(stat,128,L"%s  •  %u files  •  total: ",
        m_formatName.c_str(), m_fileCount);
    wchar_t szBuf[32]; StrFormatByteSizeW(m_totalSize,szBuf,32);
    std::wstring statStr = stat + std::wstring(szBuf);
    g.DrawString(statStr.c_str(),-1,&fSm,Gdiplus::PointF(12,28),&gold);
}

void CPreviewHandler::DrawTable(Gdiplus::Graphics& g, float W, float H)
{
    float tableTop = 54.0f;
    float rowH     = (float)m_rowHeight;

    // Column widths (proportional)
    float cw[] = { W*0.40f, W*0.12f, W*0.12f, W*0.14f, W*0.15f };
    const wchar_t* ch[] = { L"Name",L"Size",L"Packed",L"Method",L"Modified"};

    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font fHdr(&ff, 9,Gdiplus::FontStyleBold,    Gdiplus::UnitPixel);
    Gdiplus::Font fRow(&ff, 9,Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

    Gdiplus::SolidBrush hdrBg (Gdiplus::Color(255, 50, 50, 65));
    Gdiplus::SolidBrush hdrTxt(Gdiplus::Color(255,200,200,220));
    Gdiplus::SolidBrush rowBg1(Gdiplus::Color(255,248,248,252));
    Gdiplus::SolidBrush rowBg2(Gdiplus::Color(255,238,238,248));
    Gdiplus::SolidBrush rowTxt(Gdiplus::Color(255, 30, 30, 30));
    Gdiplus::SolidBrush dirTxt(Gdiplus::Color(255,  0, 80,200));
    Gdiplus::Pen        sep   (Gdiplus::Color(255,220,220,230), 1.0f);

    // Draw column headers
    g.FillRectangle(&hdrBg, 0.0f, tableTop, W, rowH);
    float x = 4.0f;
    for (int c=0;c<5;c++) {
        g.DrawString(ch[c],-1,&fHdr,
            Gdiplus::PointF(x, tableTop+2), &hdrTxt);
        x += cw[c];
    }
    tableTop += rowH;

    // Draw rows
    int startRow = m_scrollY / m_rowHeight;
    int visRows  = (int)((H - tableTop) / rowH) + 1;
    for (int i = startRow;
         i < (int)m_rows.size() && i < startRow+visRows; i++)
    {
        float y = tableTop + (i-startRow)*rowH;
        auto& row = m_rows[i];

        // Row background
        Gdiplus::SolidBrush& bg = (i%2==0) ? rowBg1 : rowBg2;
        g.FillRectangle(&bg, 0.0f, y, W, rowH);

        // Columns
        const wchar_t* cols[] = {
            row.name.c_str(), row.size.c_str(),
            row.packed.c_str(), row.method.c_str(), row.modified.c_str()
        };
        Gdiplus::SolidBrush& txtBrush = row.isDir ? dirTxt : rowTxt;
        x = 4.0f;
        for (int c=0;c<5;c++) {
            // Clip per column
            Gdiplus::RectF clip(x, y, cw[c]-4, rowH);
            g.SetClip(clip);
            g.DrawString(cols[c],-1,&fRow,
                Gdiplus::PointF(x, y+2), (c==0)?&txtBrush:&rowTxt);
            g.ResetClip();
            x += cw[c];
        }
        // Row separator
        g.DrawLine(&sep, 0.0f, y+rowH, W, y+rowH);
    }
}

// ── Window Procedure ──────────────────────────────────────
LRESULT CALLBACK CPreviewHandler::PreviewWndProc(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CPreviewHandler* p = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        p = (CPreviewHandler*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)p);
        p->m_hwnd = hwnd;
    } else {
        p = (CPreviewHandler*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!p) return DefWindowProcW(hwnd,msg,wp,lp);
    return p->WndProc(hwnd,msg,wp,lp);
}

LRESULT CPreviewHandler::WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd,&ps);
        RECT rc; GetClientRect(hwnd,&rc);
        // Double-buffer
        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hBmp = CreateCompatibleBitmap(hdc,rc.right,rc.bottom);
        HBITMAP hOld = (HBITMAP)SelectObject(hdcMem,hBmp);
        PaintPreview(hdcMem, rc);
        BitBlt(hdc,0,0,rc.right,rc.bottom,hdcMem,0,0,SRCCOPY);
        SelectObject(hdcMem,hOld);
        DeleteObject(hBmp); DeleteDC(hdcMem);
        EndPaint(hwnd,&ps);
        return 0; }

    case WM_VSCROLL: {
        SCROLLINFO si{sizeof(si),SIF_ALL};
        GetScrollInfo(hwnd,SB_VERT,&si);
        switch (LOWORD(wp)) {
        case SB_LINEUP:   si.nPos -= m_rowHeight; break;
        case SB_LINEDOWN: si.nPos += m_rowHeight; break;
        case SB_PAGEUP:   si.nPos -= si.nPage;    break;
        case SB_PAGEDOWN: si.nPos += si.nPage;    break;
        case SB_THUMBTRACK: si.nPos = si.nTrackPos; break;
        }
        si.nPos = std::clamp(si.nPos,si.nMin,(int)(si.nMax-si.nPage));
        m_scrollY = si.nPos;
        SetScrollInfo(hwnd,SB_VERT,&si,TRUE);
        InvalidateRect(hwnd,nullptr,FALSE);
        return 0; }

    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd,&rc);
        int total = (int)m_rows.size() * m_rowHeight + 60;
        SCROLLINFO si{sizeof(si),SIF_PAGE|SIF_RANGE,
            0, total, (UINT)(rc.bottom), m_scrollY};
        SetScrollInfo(hwnd,SB_VERT,&si,TRUE);
        InvalidateRect(hwnd,nullptr,TRUE);
        return 0; }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        m_scrollY = std::max(0, m_scrollY - delta/3);
        InvalidateRect(hwnd,nullptr,FALSE);
        return 0; }
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
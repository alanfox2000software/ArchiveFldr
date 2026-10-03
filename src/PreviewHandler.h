// PreviewHandler.h
// Implements IPreviewHandler + IPreviewHandlerVisuals
// Renders a styled archive content list inside Explorer's
// preview pane (right panel) using GDI+.
#pragma once
#include "stdafx.h"

// Deliberately NOT IInitializeWithStream.
//
// The preview host asks for the stream initialiser first and uses it if
// the QueryInterface succeeds — it does not fall back to
// IInitializeWithFile when the Initialize that follows fails. This class
// used to advertise IInitializeWithStream and answer E_NOTIMPL from it,
// which is the one combination that guarantees an empty preview pane for
// every archive. An archive is read by path (the engines open files, not
// streams), so the file initialiser is the only one offered and the host
// picks it. See the same note in ThumbnailProvider.h.
class CPreviewHandler :
    public IPreviewHandler,
    public IPreviewHandlerVisuals,
    public IInitializeWithFile,
    public IOleWindow
{
public:
    CPreviewHandler();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID, void**) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;

    // IInitializeWithFile
    STDMETHODIMP Initialize(LPCWSTR pszFilePath, DWORD grfMode) override;

    // IOleWindow
    STDMETHODIMP GetWindow(HWND*) override;
    STDMETHODIMP ContextSensitiveHelp(BOOL) override;

    // IPreviewHandler
    STDMETHODIMP SetWindow   (HWND hwnd, const RECT* prc) override;
    STDMETHODIMP SetRect     (const RECT* prc)            override;
    STDMETHODIMP DoPreview   ()                           override;
    STDMETHODIMP Unload      ()                           override;
    STDMETHODIMP SetFocus    ()                           override;
    STDMETHODIMP QueryFocus  (HWND*)                      override;
    STDMETHODIMP TranslateAccelerator(MSG*)               override;

    // IPreviewHandlerVisuals
    STDMETHODIMP SetBackgroundColor(COLORREF color) override;
    STDMETHODIMP SetFont           (const LOGFONTW* plf) override;
    STDMETHODIMP SetTextColor      (COLORREF color)      override;

private:
    ~CPreviewHandler();

    // Window management
    static LRESULT CALLBACK PreviewWndProc(HWND,UINT,WPARAM,LPARAM);
    LRESULT WndProc(HWND,UINT,WPARAM,LPARAM);
    void RegisterClass();
    void CreatePreviewWindow();
    void DestroyPreviewWindow();
    // Scroll to an absolute position, clamped to the current range, and
    // keep the scrollbar and the drawing in step.
    void ApplyScrollPos(HWND hwnd, int pos);

    // Rendering
    void PaintPreview(HDC hdc, const RECT& rc);
    void DrawHeader  (Gdiplus::Graphics& g, float W, float H);
    void DrawTable   (Gdiplus::Graphics& g, float W, float H);
    void LoadEntries ();

    long         m_cRef         = 1;
    HWND         m_hwndParent   = nullptr;
    HWND         m_hwnd         = nullptr;
    RECT         m_rc           = {};
    std::wstring m_filePath;
    COLORREF     m_bgColor      = RGB(255,255,255);
    COLORREF     m_txtColor     = RGB(0,0,0);
    LOGFONTW     m_lf           = {};

    // Archive data
    struct Row { std::wstring name, size, packed, method, modified; bool isDir; };
    std::vector<Row> m_rows;
    std::wstring     m_formatName;
    uint64_t         m_totalSize  = 0;
    uint64_t         m_packedSize = 0;
    UINT             m_fileCount  = 0;

    // Scrolling
    int  m_scrollY   = 0;
    int  m_rowHeight = 18;
};
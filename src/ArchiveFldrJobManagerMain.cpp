#include "stdafx.h"
#include <shellapi.h>
#include "../res/resource.h"
#include "ArchiveJobPipe.h"

static HANDLE g_stop = nullptr;
static HANDLE g_serverThread = nullptr;

static DWORD WINAPI PipeThread(void*)
{
    while (WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0) {
        ArchiveJobPipe::Server server;
        if (!server.Listen()) break;
        if (!server.Accept()) continue;
        ArchiveJobProtocol::MessageType type{}; std::wstring payload;
        if (ArchiveJobPipe::Receive(server.Handle(), type, payload) &&
            type == ArchiveJobProtocol::MessageType::Submit) {
            ArchiveJobProtocol::JobRequest request;
            if (ArchiveJobProtocol::Decode(payload, request)) {
                std::wstring reply = L"id=" + ArchiveJobProtocol::GuidText(request.id) + L"\nstate=queued\n";
                ArchiveJobPipe::Send(server.Handle(), ArchiveJobProtocol::MessageType::State, reply);
                // Dispatching the queued request to a worker is the next job-manager phase.
            }
        }
    }
    return 0;
}

static const wchar_t* kClass = L"ArchiveFldrJobManagerWindow";
static const UINT WM_TRAY = WM_APP + 1;
static const UINT ID_TRAY = 1001;
static HANDLE g_mutex = nullptr;
static NOTIFYICONDATAW g_tray{};

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TRAY && wp == ID_TRAY) {
        if (lp == WM_LBUTTONDBLCLK) ShowWindow(hwnd, SW_SHOW);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == ID_TRAY) { ShowWindow(hwnd, SW_SHOW); return 0; }
    if (msg == WM_CLOSE) { ShowWindow(hwnd, SW_HIDE); return 0; }
    if (msg == WM_DESTROY) { Shell_NotifyIconW(NIM_DELETE, &g_tray); if (g_stop) { SetEvent(g_stop); HANDLE wake = CreateFileW(ArchiveJobProtocol::kPipeName, GENERIC_READ|GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr); if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake); } if (g_serverThread) { WaitForSingleObject(g_serverThread, 3000); CloseHandle(g_serverThread); g_serverThread = nullptr; } PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    g_mutex = CreateMutexW(nullptr, TRUE, L"Local\\ArchiveFldrJobManager.Singleton");
    if (!g_mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    WNDCLASSW wc{}; wc.hInstance = instance; wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = kClass; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, kClass, L"ArchiveFldr Jobs", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 420, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    // The manager is intentionally hidden after startup; double-click the tray
    // icon to show this window. A real job list/protocol is added independently
    // of the Explorer shell extension process.
    g_tray.cbSize = sizeof(g_tray); g_tray.hWnd = hwnd; g_tray.uID = ID_TRAY;
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_ARCHIVEFLDR));
    if (!g_tray.hIcon) g_tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_tray.szTip, L"ArchiveFldr jobs");
    Shell_NotifyIconW(NIM_ADD, &g_tray);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop) return 1;
    g_serverThread = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
    ShowWindow(hwnd, SW_HIDE);
    MSG msg{}; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    CloseHandle(g_mutex); return 0;
}

#include "stdafx.h"
#include <shellapi.h>
#include "../res/resource.h"
#include "ArchiveJobPipe.h"

static HANDLE g_stop = nullptr;
static HANDLE g_serverThread = nullptr;
static HANDLE g_queueThread = nullptr;
static HANDLE g_queueEvent = nullptr;
static CRITICAL_SECTION g_queueLock;
static std::vector<ArchiveJobProtocol::JobRequest> g_queue;

static std::wstring QuoteArg(const std::wstring& v)
{
    std::wstring s=L"\""; size_t bs=0;
    for(wchar_t c:v){if(c==L'\\'){++bs;continue;}if(c==L'\"'){s.append(bs*2+1,L'\\');s+=c;bs=0;}else{s.append(bs,L'\\');bs=0;s+=c;}}
    s.append(bs*2,L'\\'); return s+L"\"";
}
static bool StartJob(const ArchiveJobProtocol::JobRequest& j)
{
    wchar_t mod[MAX_PATH]={}; if(!GetModuleFileNameW(nullptr,mod,ARRAYSIZE(mod))) return false;
    std::wstring base=mod; size_t slash=base.find_last_of(L"\\/"); base=(slash==std::wstring::npos?L"":base.substr(0,slash+1));
    std::wstring exe=base+(j.kind==ArchiveJobProtocol::JobKind::Compress?L"ArchiveFldrCompress.exe":L"ArchiveFldrExtract.exe");
    std::wstring cmd=QuoteArg(exe);
    if(j.kind==ArchiveJobProtocol::JobKind::Compress){cmd+=L" --out "+QuoteArg(j.output)+L" --format "+QuoteArg(j.format)+L" --level "+std::to_wstring(j.level)+L" --threads "+std::to_wstring(j.threads);if(j.solid)cmd+=L" --solid";if(j.encryptNames)cmd+=L" --encrypt-names";for(auto&s:j.sources)cmd+=L" "+QuoteArg(s);}
    else cmd+=L" --archive "+QuoteArg(j.archive)+L" --entry "+QuoteArg(j.output)+L" --dest "+QuoteArg(j.output);
    std::vector<wchar_t> buf(cmd.begin(),cmd.end());buf.push_back(L'\0');STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.c_str(),buf.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))return false;CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return true;
}
static DWORD WINAPI QueueThread(void*)
{
    while(WaitForSingleObject(g_stop,0)!=WAIT_OBJECT_0){WaitForSingleObject(g_queueEvent,500);ArchiveJobProtocol::JobRequest j;bool have=false;EnterCriticalSection(&g_queueLock);if(!g_queue.empty()){j=g_queue.front();g_queue.erase(g_queue.begin());have=true;}if(g_queue.empty())ResetEvent(g_queueEvent);LeaveCriticalSection(&g_queueLock);if(have)StartJob(j);}return 0;
}

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
                EnterCriticalSection(&g_queueLock);
                g_queue.push_back(request);
                SetEvent(g_queueEvent);
                LeaveCriticalSection(&g_queueLock);
                ArchiveJobPipe::Send(server.Handle(), ArchiveJobProtocol::MessageType::State, reply);
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
    if (msg == WM_DESTROY) { Shell_NotifyIconW(NIM_DELETE, &g_tray); if (g_stop) { SetEvent(g_stop); HANDLE wake = CreateFileW(ArchiveJobProtocol::kPipeName, GENERIC_READ|GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr); if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake); } if (g_serverThread) { WaitForSingleObject(g_serverThread, 3000); CloseHandle(g_serverThread); g_serverThread = nullptr; } if (g_queueThread) { WaitForSingleObject(g_queueThread, 3000); CloseHandle(g_queueThread); g_queueThread = nullptr; } DeleteCriticalSection(&g_queueLock); CloseHandle(g_stop); CloseHandle(g_queueEvent); PostQuitMessage(0); return 0; }
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
    g_queueEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    InitializeCriticalSection(&g_queueLock);
    if (!g_stop || !g_queueEvent) return 1;
    g_serverThread = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
    g_queueThread = CreateThread(nullptr, 0, QueueThread, nullptr, 0, nullptr);
    ShowWindow(hwnd, SW_HIDE);
    MSG msg{}; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    CloseHandle(g_mutex); return 0;
}

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
struct ActiveJob { GUID id{}; HANDLE process = nullptr; HANDLE output = nullptr; ArchiveJobProtocol::JobKind kind{}; int percent = 0; std::wstring current; std::wstring outputPath; bool outputExisted = false; };
static std::vector<ActiveJob> g_active;
static std::map<std::wstring, DWORD> g_finishedResults;
static constexpr size_t kMaxCompressJobs = 1;
static constexpr size_t kMaxExtractJobs = 2;
static bool HasCapacity(ArchiveJobProtocol::JobKind kind)
{
    size_t count = 0; for (const auto& job : g_active) if (job.kind == kind) ++count;
    return count < (kind == ArchiveJobProtocol::JobKind::Compress ? kMaxCompressJobs : kMaxExtractJobs);
}

static HANDLE StartJob(const ArchiveJobProtocol::JobRequest& j, HANDLE* output)
{
    wchar_t mod[MAX_PATH]={}; if(!GetModuleFileNameW(nullptr,mod,ARRAYSIZE(mod))) return false;
    std::wstring base=mod; size_t slash=base.find_last_of(L"\\/"); base=(slash==std::wstring::npos?L"":base.substr(0,slash+1));
    std::wstring exe=base+(j.kind==ArchiveJobProtocol::JobKind::Compress?L"ArchiveFldrCompress.exe":L"ArchiveFldrExtract.exe");
    std::wstring cmd=QuoteArg(exe);
    if(j.kind==ArchiveJobProtocol::JobKind::Compress){cmd+=L" --out "+QuoteArg(j.output)+L" --format "+QuoteArg(j.format)+L" --level "+std::to_wstring(j.level)+L" --threads "+std::to_wstring(j.threads);if(j.solid)cmd+=L" --solid";if(j.encryptNames)cmd+=L" --encrypt-names";for(auto&s:j.sources)cmd+=L" "+QuoteArg(s);}
    else cmd+=L" --archive "+QuoteArg(j.archive)+L" --entry "+QuoteArg(j.output)+L" --dest "+QuoteArg(j.output);
    std::vector<wchar_t> buf(cmd.begin(),cmd.end());buf.push_back(L'\0');
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE}; HANDLE readPipe=nullptr, writePipe=nullptr;
    if(!CreatePipe(&readPipe,&writePipe,&sa,0)) return nullptr;
    SetHandleInformation(readPipe,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=writePipe;si.hStdError=writePipe;PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.c_str(),buf.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)){CloseHandle(readPipe);CloseHandle(writePipe);return nullptr;}
    CloseHandle(writePipe); if(output)*output=readPipe; else CloseHandle(readPipe); CloseHandle(pi.hThread); return pi.hProcess;
}
static DWORD WINAPI WorkerMonitor(void* raw)
{
    auto* job = static_cast<ActiveJob*>(raw);
    char buffer[512] = {}; DWORD got = 0; std::string line;
    while (ReadFile(job->output, buffer, sizeof(buffer)-1, &got, nullptr) && got) {
        line.append(buffer, got);
        size_t end = 0;
        while ((end = line.find('\n')) != std::string::npos) {
            std::string one = line.substr(0, end); line.erase(0, end + 1);
            if (one.rfind("PROGRESS ", 0) == 0) { int pct = atoi(one.c_str() + 9); EnterCriticalSection(&g_queueLock); for(auto& a:g_active) if(IsEqualGUID(a.id,job->id)) a.percent=pct; LeaveCriticalSection(&g_queueLock); }
        }
    }
    CloseHandle(job->output); WaitForSingleObject(job->process, INFINITE);
    DWORD exitCode = 1; GetExitCodeProcess(job->process, &exitCode);
    const std::wstring id = ArchiveJobProtocol::GuidText(job->id);
    EnterCriticalSection(&g_queueLock);
    g_finishedResults[id] = exitCode;
    if (exitCode != 0 && exitCode != ERROR_CANCELLED) {
        OutputDebugStringW((L"ArchiveFldr worker failed or crashed: " + id + L" exit=" + std::to_wstring(exitCode) + L"\n").c_str());
    }
    for (auto it = g_active.begin(); it != g_active.end(); ++it) {
        if (IsEqualGUID(it->id, job->id)) {
            if (exitCode != 0 && !it->outputExisted && !it->outputPath.empty())
                DeleteFileW(it->outputPath.c_str());
            CloseHandle(it->process); g_active.erase(it); break;
        }
    }
    LeaveCriticalSection(&g_queueLock);
    delete job;
    return exitCode;
}

static DWORD WINAPI QueueThread(void*)
{
    while(WaitForSingleObject(g_stop,0)!=WAIT_OBJECT_0){WaitForSingleObject(g_queueEvent,500);ArchiveJobProtocol::JobRequest j;bool have=false;EnterCriticalSection(&g_queueLock);if(!g_queue.empty()){j=g_queue.front();g_queue.erase(g_queue.begin());have=true;}if(g_queue.empty())ResetEvent(g_queueEvent);LeaveCriticalSection(&g_queueLock);if(have){EnterCriticalSection(&g_queueLock);const bool capacity=HasCapacity(j.kind);if(!capacity){g_queue.insert(g_queue.begin(),j);SetEvent(g_queueEvent);}LeaveCriticalSection(&g_queueLock);if(!capacity){Sleep(250);continue;}HANDLE output=nullptr;HANDLE process=StartJob(j,&output);if(process){auto* active=new ActiveJob;active->id=j.id;active->process=process;active->output=output;active->kind=j.kind;active->outputPath=j.output;active->outputExisted=GetFileAttributesW(j.output.c_str())!=INVALID_FILE_ATTRIBUTES;EnterCriticalSection(&g_queueLock);g_active.push_back(*active);LeaveCriticalSection(&g_queueLock);HANDLE monitor=CreateThread(nullptr,0,WorkerMonitor,active,0,nullptr);if(monitor)CloseHandle(monitor);else{EnterCriticalSection(&g_queueLock);if(!g_active.empty())g_active.pop_back();LeaveCriticalSection(&g_queueLock);CloseHandle(process);delete active;}}} }return 0;
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
        } else if (type == ArchiveJobProtocol::MessageType::Control) {
            GUID id{}; ArchiveJobProtocol::ParseGuid(ArchiveJobProtocol::Get(payload,L"id"), id);
            auto command=(ArchiveJobProtocol::Control)_wtoi(ArchiveJobProtocol::Get(payload,L"command").c_str());
            if(command==ArchiveJobProtocol::Control::Cancel){EnterCriticalSection(&g_queueLock);for(auto it=g_queue.begin();it!=g_queue.end();)if(IsEqualGUID(it->id,id))it=g_queue.erase(it);else++it;for(auto&a:g_active)if(IsEqualGUID(a.id,id))TerminateProcess(a.process,ERROR_CANCELLED);LeaveCriticalSection(&g_queueLock);}
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

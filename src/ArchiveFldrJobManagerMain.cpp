#include "stdafx.h"
#include <shellapi.h>
#include "../res/resource.h"
#include "ArchiveJobPipe.h"

static HANDLE g_stop = nullptr;
static HANDLE g_serverThread = nullptr;
static HANDLE g_queueThread = nullptr;
static std::vector<HANDLE> g_monitors;
static HANDLE g_queueEvent = nullptr;
static CRITICAL_SECTION g_queueLock;
static std::vector<ArchiveJobProtocol::JobRequest> g_queue;

static std::wstring QuoteArg(const std::wstring& v)
{
    std::wstring s=L"\""; size_t bs=0;
    for(wchar_t c:v){if(c==L'\\'){++bs;continue;}if(c==L'\"'){s.append(bs*2+1,L'\\');s+=c;bs=0;}else{s.append(bs,L'\\');bs=0;s+=c;}}
    s.append(bs*2,L'\\'); return s+L"\"";
}
struct ActiveJob { GUID id{}; HANDLE process = nullptr; HANDLE output = nullptr; HANDLE cancelEvent = nullptr; ArchiveJobProtocol::JobKind kind{}; int percent = 0; std::wstring current; ArchiveJobProtocol::JobState state = ArchiveJobProtocol::JobState::Running; std::wstring outputPath; bool outputExisted = false; };
static std::vector<ActiveJob> g_active;
struct FinishedResult { ArchiveJobProtocol::JobState state; DWORD exitCode; std::wstring error; };
static std::map<std::wstring, FinishedResult> g_finishedResults;
static constexpr size_t kMaxCompressJobs = 1;
static constexpr size_t kMaxExtractJobs = 2;
static bool HasCapacity(ArchiveJobProtocol::JobKind kind)
{
    size_t count = 0; for (const auto& job : g_active) if (job.kind == kind) ++count;
    return count < (kind == ArchiveJobProtocol::JobKind::Compress ? kMaxCompressJobs : kMaxExtractJobs);
}

static HANDLE StartJob(const ArchiveJobProtocol::JobRequest& j, HANDLE* output, HANDLE* cancelEvent)
{
    wchar_t mod[MAX_PATH]={}; if(!GetModuleFileNameW(nullptr,mod,ARRAYSIZE(mod))) return false;
    std::wstring base=mod; size_t slash=base.find_last_of(L"\\/"); base=(slash==std::wstring::npos?L"":base.substr(0,slash+1));
    std::wstring exe=base+(j.kind==ArchiveJobProtocol::JobKind::Compress?L"ArchiveFldrCompress.exe":L"ArchiveFldrExtract.exe");
    std::wstring cancelName = L"Local\\ArchiveFldrCancel-" + ArchiveJobProtocol::GuidText(j.id);
    std::wstring cmd=QuoteArg(exe) + L" --cancel-event " + QuoteArg(cancelName);
    if(j.kind==ArchiveJobProtocol::JobKind::Compress){cmd+=L" --out "+QuoteArg(j.output)+L" --format "+QuoteArg(j.format)+L" --level "+std::to_wstring(j.level)+L" --threads "+std::to_wstring(j.threads);if(j.solid)cmd+=L" --solid";if(j.encryptNames)cmd+=L" --encrypt-names";if(!j.password.empty())cmd+=L" --password-stdin";for(auto&s:j.sources)cmd+=L" "+QuoteArg(s);}
    else cmd+=L" --archive "+QuoteArg(j.archive)+L" --entry "+QuoteArg(j.entry)+L" --dest "+QuoteArg(j.output);
    std::vector<wchar_t> buf(cmd.begin(),cmd.end());buf.push_back(L'\0');
    HANDLE cancellation = CreateEventW(nullptr, TRUE, FALSE, cancelName.c_str());
    if (!cancellation) return nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE}; HANDLE readPipe=nullptr, writePipe=nullptr, passRead=nullptr, passWrite=nullptr;
    if(!CreatePipe(&readPipe,&writePipe,&sa,0)){CloseHandle(cancellation);return nullptr;}
    SetHandleInformation(readPipe,HANDLE_FLAG_INHERIT,0);
    if(!j.password.empty()){if(!CreatePipe(&passRead,&passWrite,&sa,0)){CloseHandle(readPipe);CloseHandle(writePipe);return nullptr;}SetHandleInformation(passWrite,HANDLE_FLAG_INHERIT,0);}
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=writePipe;si.hStdError=writePipe;si.hStdInput=passRead?passRead:GetStdHandle(STD_INPUT_HANDLE);PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.c_str(),buf.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)){CloseHandle(readPipe);CloseHandle(writePipe);if(passRead)CloseHandle(passRead);if(passWrite)CloseHandle(passWrite);return nullptr;}
    CloseHandle(writePipe);if(passRead)CloseHandle(passRead);if(passWrite){DWORD bytes=0;WriteFile(passWrite,j.password.data(),(DWORD)(j.password.size()*sizeof(wchar_t)),&bytes,nullptr);CloseHandle(passWrite);} if(output)*output=readPipe;else CloseHandle(readPipe);if(cancelEvent)*cancelEvent=cancellation;else CloseHandle(cancellation);CloseHandle(pi.hThread);return pi.hProcess;
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
            if (one.rfind("PROGRESS ", 0) == 0) {
                int pct = atoi(one.c_str() + 9); std::wstring current;
                size_t split = one.find(' ', 9); if (split != std::string::npos) {
                    std::string name = one.substr(split + 1);
                    int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), (int)name.size(), nullptr, 0);
                    current.resize(n); if (n) MultiByteToWideChar(CP_UTF8, 0, name.c_str(), (int)name.size(), &current[0], n);
                }
                EnterCriticalSection(&g_queueLock); for(auto& a:g_active) if(IsEqualGUID(a.id,job->id)){a.percent=pct;a.current=current;a.state=ArchiveJobProtocol::JobState::Running;} LeaveCriticalSection(&g_queueLock);
            }
        }
    }
    CloseHandle(job->output); WaitForSingleObject(job->process, INFINITE);
    DWORD exitCode = 1; GetExitCodeProcess(job->process, &exitCode);
    const std::wstring id = ArchiveJobProtocol::GuidText(job->id);
    EnterCriticalSection(&g_queueLock);
    FinishedResult result{}; result.exitCode = exitCode; result.state = exitCode == 0 ? ArchiveJobProtocol::JobState::Completed : (exitCode == ERROR_CANCELLED ? ArchiveJobProtocol::JobState::Cancelled : ArchiveJobProtocol::JobState::Failed); g_finishedResults[id] = result;
    if (exitCode != 0 && exitCode != ERROR_CANCELLED) {
        OutputDebugStringW((L"ArchiveFldr worker failed or crashed: " + id + L" exit=" + std::to_wstring(exitCode) + L"\n").c_str());
    }
    for (auto it = g_active.begin(); it != g_active.end(); ++it) {
        if (IsEqualGUID(it->id, job->id)) {
            if (exitCode != 0 && !it->outputExisted && !it->outputPath.empty()) {
                if (it->kind == ArchiveJobProtocol::JobKind::Extract) {
                    std::vector<wchar_t> from(it->outputPath.begin(), it->outputPath.end());
                    from.push_back(L'\0'); from.push_back(L'\0');
                    SHFILEOPSTRUCTW op{}; op.wFunc=FO_DELETE; op.pFrom=from.data();
                    op.fFlags=FOF_NOCONFIRMATION|FOF_NOERRORUI|FOF_SILENT|FOF_NOCONFIRMMKDIR;
                    SHFileOperationW(&op);
                } else DeleteFileW(it->outputPath.c_str());
            }
            CloseHandle(it->process); if(it->cancelEvent) CloseHandle(it->cancelEvent); g_active.erase(it); break;
        }
    }
    LeaveCriticalSection(&g_queueLock);
    delete job;
    return exitCode;
}

static DWORD WINAPI QueueThread(void*)
{
    while(WaitForSingleObject(g_stop,0)!=WAIT_OBJECT_0){WaitForSingleObject(g_queueEvent,500);ArchiveJobProtocol::JobRequest j;bool have=false;EnterCriticalSection(&g_queueLock);for(auto it=g_queue.begin();it!=g_queue.end();++it){if(HasCapacity(it->kind)){j=*it;g_queue.erase(it);have=true;break;}}if(g_queue.empty())ResetEvent(g_queueEvent);LeaveCriticalSection(&g_queueLock);if(have){HANDLE output=nullptr;HANDLE cancelEvent=nullptr;HANDLE process=StartJob(j,&output,&cancelEvent);if(process){auto* active=new ActiveJob;active->id=j.id;active->process=process;active->output=output;active->cancelEvent=cancelEvent;active->kind=j.kind;active->outputPath=j.output;active->outputExisted=GetFileAttributesW(j.output.c_str())!=INVALID_FILE_ATTRIBUTES;EnterCriticalSection(&g_queueLock);g_active.push_back(*active);LeaveCriticalSection(&g_queueLock);HANDLE monitor=CreateThread(nullptr,0,WorkerMonitor,active,0,nullptr);if(monitor){EnterCriticalSection(&g_queueLock);g_monitors.push_back(monitor);LeaveCriticalSection(&g_queueLock);}else{EnterCriticalSection(&g_queueLock);if(!g_active.empty())g_active.pop_back();LeaveCriticalSection(&g_queueLock);CloseHandle(process);delete active;}}} }return 0;
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
                std::wstring reply;
                EnterCriticalSection(&g_queueLock);
                if (g_queue.size() >= 128) {
                    reply = L"state=failed\nerror=job queue is full\n";
                    LeaveCriticalSection(&g_queueLock);
                    ArchiveJobPipe::Send(server.Handle(), ArchiveJobProtocol::MessageType::State, reply);
                    continue;
                }
                reply = L"id=" + ArchiveJobProtocol::GuidText(request.id) + L"\nstate=queued\n";
                g_queue.push_back(request);
                SetEvent(g_queueEvent);
                LeaveCriticalSection(&g_queueLock);
                ArchiveJobPipe::Send(server.Handle(), ArchiveJobProtocol::MessageType::State, reply);
            } else {
                ArchiveJobPipe::Send(server.Handle(), ArchiveJobProtocol::MessageType::State,
                                     L"state=failed\nerror=invalid job request\n");
            }
        } else if (type == ArchiveJobProtocol::MessageType::List) {
            GUID id{}; ArchiveJobProtocol::ParseGuid(ArchiveJobProtocol::Get(payload,L"id"), id);
            const std::wstring key=ArchiveJobProtocol::GuidText(id); bool active=false, queued=false;
            EnterCriticalSection(&g_queueLock);
            for(const auto& a:g_active) if(IsEqualGUID(a.id,id)) active=true;
            for(const auto& q:g_queue) if(IsEqualGUID(q.id,id)) queued=true;
            auto done=g_finishedResults.find(key);
            if(done!=g_finishedResults.end()) ArchiveJobPipe::Send(server.Handle(),ArchiveJobProtocol::MessageType::Result,ArchiveJobProtocol::EncodeResult(id,done->second.state,done->second.exitCode,done->second.error));
            else { std::wstring state=L"state="; state += active?L"running":queued?L"queued":L"unknown"; state += L"\nid="; state += key; state += L"\n"; ArchiveJobPipe::Send(server.Handle(),ArchiveJobProtocol::MessageType::State,state); }
            LeaveCriticalSection(&g_queueLock);
        } else if (type == ArchiveJobProtocol::MessageType::Control) {
            GUID id{}; ArchiveJobProtocol::ParseGuid(ArchiveJobProtocol::Get(payload,L"id"), id);
            auto command=(ArchiveJobProtocol::Control)_wtoi(ArchiveJobProtocol::Get(payload,L"command").c_str());
            if(command==ArchiveJobProtocol::Control::Cancel){
                HANDLE process=nullptr;
                EnterCriticalSection(&g_queueLock);
                for(auto it=g_queue.begin();it!=g_queue.end();) if(IsEqualGUID(it->id,id)) it=g_queue.erase(it); else ++it;
                for(auto&a:g_active) if(IsEqualGUID(a.id,id)){ process=a.process; a.state=ArchiveJobProtocol::JobState::Cancelling; }
                LeaveCriticalSection(&g_queueLock);
                if(process){
                    // Give a worker that has reached a safe archive boundary a
                    // chance to finish and close its temporary output first.
                    if(WaitForSingleObject(process, 5000)==WAIT_TIMEOUT)
                        TerminateProcess(process, ERROR_CANCELLED);
                }
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
static HWND g_list = nullptr;
static HWND g_cancel = nullptr;
static std::vector<GUID> g_visibleIds;
static constexpr int ID_CANCEL_JOB = 2001;

static void CancelJob(const GUID& id)
{
    HANDLE process=nullptr;
    EnterCriticalSection(&g_queueLock);
    for(auto it=g_queue.begin();it!=g_queue.end();) if(IsEqualGUID(it->id,id)) it=g_queue.erase(it); else ++it;
    for(auto& a:g_active) if(IsEqualGUID(a.id,id)){process=a.process;a.state=ArchiveJobProtocol::JobState::Cancelling;if(a.cancelEvent) SetEvent(a.cancelEvent);}
    LeaveCriticalSection(&g_queueLock);
    if(process && WaitForSingleObject(process,5000)==WAIT_TIMEOUT) TerminateProcess(process,ERROR_CANCELLED);
}

static void RefreshList()
{
    if (!g_list) return;
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    g_visibleIds.clear();
    EnterCriticalSection(&g_queueLock);
    for (const auto& j : g_active) {
        std::wstring row = L"Running  " + ArchiveJobProtocol::GuidText(j.id) + L"  " + std::to_wstring(j.percent) + L"%  " + j.current;
        SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)row.c_str());
        g_visibleIds.push_back(j.id);
    }
    for (const auto& j : g_queue) {
        std::wstring row = L"Queued   " + ArchiveJobProtocol::GuidText(j.id);
        SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)row.c_str());
        g_visibleIds.push_back(j.id);
    }
    for (const auto& r : g_finishedResults) {
        std::wstring row = L"Finished " + r.first + L"  exit=" + std::to_wstring(r.second.exitCode);
        SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)row.c_str());
    }
    LeaveCriticalSection(&g_queueLock);
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TRAY && wp == ID_TRAY) {
        if (lp == WM_LBUTTONDBLCLK) ShowWindow(hwnd, SW_SHOW);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == ID_TRAY) { ShowWindow(hwnd, SW_SHOW); return 0; }
    if (msg == WM_COMMAND && LOWORD(wp) == ID_CANCEL_JOB) { int sel=(int)SendMessageW(g_list,LB_GETCURSEL,0,0); if(sel>=0 && sel<(int)g_visibleIds.size()) CancelJob(g_visibleIds[sel]); return 0; }
    if (msg == WM_TIMER) { RefreshList(); return 0; }
    if (msg == WM_SIZE && g_list) { MoveWindow(g_list, 8, 8, LOWORD(lp) - 16, HIWORD(lp) - 16, TRUE); return 0; }
    if (msg == WM_CLOSE) { ShowWindow(hwnd, SW_HIDE); return 0; }
    if (msg == WM_DESTROY) { Shell_NotifyIconW(NIM_DELETE, &g_tray); if (g_stop) { SetEvent(g_stop); HANDLE wake = CreateFileW(ArchiveJobProtocol::kPipeName, GENERIC_READ|GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr); if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake); } if (g_serverThread) { WaitForSingleObject(g_serverThread, 3000); CloseHandle(g_serverThread); g_serverThread = nullptr; } if (g_queueThread) { WaitForSingleObject(g_queueThread, 3000); CloseHandle(g_queueThread); g_queueThread = nullptr; }
        // Stop and reap every worker before destroying the queue lock.
        EnterCriticalSection(&g_queueLock);
        std::vector<HANDLE> workers; for (auto& job : g_active) workers.push_back(job.process);
        LeaveCriticalSection(&g_queueLock);
        for (HANDLE process : workers) TerminateProcess(process, ERROR_CANCELLED);
        EnterCriticalSection(&g_queueLock);
        std::vector<HANDLE> monitors = g_monitors;
        LeaveCriticalSection(&g_queueLock);
        for (HANDLE monitor : monitors) WaitForSingleObject(monitor, 3000);
        for (HANDLE monitor : monitors) CloseHandle(monitor);
        EnterCriticalSection(&g_queueLock);
        for (auto& job : g_active) { if (job.outputPath.empty() || job.outputExisted) continue;
            if (job.kind == ArchiveJobProtocol::JobKind::Extract) {
                std::vector<wchar_t> from(job.outputPath.begin(), job.outputPath.end()); from.push_back(L'\0'); from.push_back(L'\0');
                SHFILEOPSTRUCTW op{}; op.wFunc=FO_DELETE; op.pFrom=from.data(); op.fFlags=FOF_NOCONFIRMATION|FOF_NOERRORUI|FOF_SILENT|FOF_NOCONFIRMMKDIR; SHFileOperationW(&op);
            } else if (GetFileAttributesW(job.outputPath.c_str()) != INVALID_FILE_ATTRIBUTES) DeleteFileW(job.outputPath.c_str()); }
        LeaveCriticalSection(&g_queueLock);
        DeleteCriticalSection(&g_queueLock); CloseHandle(g_stop); CloseHandle(g_queueEvent); PostQuitMessage(0); return 0; }
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
    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
        8, 8, 608, 380, hwnd, nullptr, instance, nullptr);
    g_cancel = CreateWindowW(L"BUTTON", L"Cancel selected job", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        8, 390, 150, 28, hwnd, (HMENU)ID_CANCEL_JOB, instance, nullptr);
    SetTimer(hwnd, 1, 500, nullptr);
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

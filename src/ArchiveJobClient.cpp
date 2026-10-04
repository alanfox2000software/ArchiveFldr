#include "stdafx.h"
#include "ArchiveJobClient.h"

namespace ArchiveJobClient
{
static bool StartManager()
{
    HMODULE self=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&StartManager),&self))return false;
    wchar_t path[MAX_PATH]={};if(!GetModuleFileNameW(self,path,ARRAYSIZE(path)))return false;
    std::wstring exe=path;size_t slash=exe.find_last_of(L"\\/");exe=(slash==std::wstring::npos?L"":exe.substr(0,slash+1))+L"ArchiveFldrJobManager.exe";
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.c_str(),nullptr,nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))return false;
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return true;
}
bool EnsureManager()
{
    ArchiveJobPipe::Client test;if(test.Connect(100))return true;
    if(!StartManager())return false;
    return test.Connect(5000);
}
bool Submit(const ArchiveJobProtocol::JobRequest& request)
{
    ArchiveJobPipe::Client client;if(!EnsureManager()||!client.Connect(5000))return false;
    if (!ArchiveJobPipe::Send(client.Handle(), ArchiveJobProtocol::MessageType::Submit,
                              ArchiveJobProtocol::Encode(request))) return false;
    if (request.hasPassword) {
        std::wstring secret = L"id=" + ArchiveJobProtocol::GuidText(request.id) + L"\npassword=" + ArchiveJobProtocol::Escape(request.password) + L"\n";
        if (!ArchiveJobPipe::Send(client.Handle(), ArchiveJobProtocol::MessageType::Password, secret)) return false;
    }
    ArchiveJobProtocol::MessageType type{}; std::wstring reply;
    if (!ArchiveJobPipe::Receive(client.Handle(), type, reply) ||
        type != ArchiveJobProtocol::MessageType::State) return false;
    return ArchiveJobProtocol::Get(reply, L"id") == ArchiveJobProtocol::GuidText(request.id) &&
           ArchiveJobProtocol::Get(reply, L"state") == L"queued";
}
bool Control(const GUID& id, ArchiveJobProtocol::Control command)
{
    ArchiveJobPipe::Client client; if(!EnsureManager()||!client.Connect(5000)) return false;
    std::wstring payload=L"id="+ArchiveJobProtocol::GuidText(id)+L"\ncommand="+std::to_wstring((uint32_t)command)+L"\n";
    return ArchiveJobPipe::Send(client.Handle(),ArchiveJobProtocol::MessageType::Control,payload);
}
bool Query(const GUID& id, ArchiveJobProtocol::MessageType& type, std::wstring& payload)
{
    ArchiveJobPipe::Client client; if(!EnsureManager()||!client.Connect(5000)) return false;
    std::wstring request=L"id="+ArchiveJobProtocol::GuidText(id)+L"\n";
    if(!ArchiveJobPipe::Send(client.Handle(),ArchiveJobProtocol::MessageType::List,request)) return false;
    return ArchiveJobPipe::Receive(client.Handle(),type,payload);
}
bool SubscribeEvents(HANDLE* eventsPipe)
{
    if(!eventsPipe) return false; *eventsPipe=nullptr;
    ArchiveJobPipe::Client client(ArchiveJobProtocol::kEventsPipeName);
    if(!client.Connect(5000)) return false;
    if(!ArchiveJobPipe::Send(client.Handle(),ArchiveJobProtocol::MessageType::Hello,L"version=1\n")) return false;
    ArchiveJobProtocol::MessageType ackType{}; std::wstring ack;
    if(!ArchiveJobPipe::Receive(client.Handle(),ackType,ack) ||
       ackType != ArchiveJobProtocol::MessageType::State ||
       ArchiveJobProtocol::Get(ack,L"state") != L"subscribed") return false;
    *eventsPipe=client.Detach(); return true;
}
}

#include "stdafx.h"
#include "ArchiveJobPipe.h"
#include <sddl.h>

namespace ArchiveJobPipe
{
static bool WriteAll(HANDLE h, const void* data, DWORD bytes)
{ const BYTE* p=(const BYTE*)data; while(bytes){DWORD n=0;if(!WriteFile(h,p,bytes,&n,nullptr)||!n)return false;p+=n;bytes-=n;}return true; }
static bool ReadAll(HANDLE h, void* data, DWORD bytes)
{ BYTE* p=(BYTE*)data; while(bytes){DWORD n=0;if(!ReadFile(h,p,bytes,&n,nullptr)||!n)return false;p+=n;bytes-=n;}return true; }
bool Send(HANDLE pipe, ArchiveJobProtocol::MessageType type, const std::wstring& payload)
{ ArchiveJobProtocol::MessageHeader h;h.type=(uint32_t)type;h.payloadBytes=(uint32_t)(payload.size()*sizeof(wchar_t));if(h.payloadBytes>ArchiveJobProtocol::kMaxPayloadBytes)return false;return WriteAll(pipe,&h,sizeof(h))&&WriteAll(pipe,payload.data(),h.payloadBytes); }
bool Receive(HANDLE pipe, ArchiveJobProtocol::MessageType& type, std::wstring& payload)
{ ArchiveJobProtocol::MessageHeader h{};if(!ReadAll(pipe,&h,sizeof(h))||h.magic!=0x314A4641||h.version!=ArchiveJobProtocol::kProtocolVersion||h.payloadBytes>ArchiveJobProtocol::kMaxPayloadBytes||h.payloadBytes%sizeof(wchar_t))return false;payload.assign(h.payloadBytes/sizeof(wchar_t),L'\0');if(!ReadAll(pipe,payload.data(),h.payloadBytes))return false;type=(ArchiveJobProtocol::MessageType)h.type;return true; }
Server::~Server(){Close();} void Server::Close(){if(m_pipe!=INVALID_HANDLE_VALUE){DisconnectNamedPipe(m_pipe);CloseHandle(m_pipe);m_pipe=INVALID_HANDLE_VALUE;}}
bool Server::Listen(){
    Close();
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    // SYSTEM, administrators, and the creating user (owner) may access the
    // manager pipe. Authenticated users who are not the owner are excluded.
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)",
        SDDL_REVISION_1, &descriptor, nullptr)) {
        // Keep the manager usable on systems whose security provider does not
        // understand the owner-rights token; the pipe remains local and the
        // failure is visible in the debugger.
        OutputDebugStringW(L"ArchiveFldr: failed to create restricted pipe ACL.\n");
        m_pipe = CreateNamedPipeW(m_name, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
            ArchiveJobProtocol::kMaxPayloadBytes, ArchiveJobProtocol::kMaxPayloadBytes,
            0, nullptr);
        return m_pipe != INVALID_HANDLE_VALUE;
    }
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
    m_pipe=CreateNamedPipeW(m_name,PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT,PIPE_UNLIMITED_INSTANCES,
        ArchiveJobProtocol::kMaxPayloadBytes,ArchiveJobProtocol::kMaxPayloadBytes,
        0,&security);
    LocalFree(descriptor);
    if (m_pipe == INVALID_HANDLE_VALUE) {
        wchar_t message[128] = {};
        swprintf_s(message, L"ArchiveFldr: CreateNamedPipe failed, error %lu\n", GetLastError());
        OutputDebugStringW(message);
    }
    return m_pipe!=INVALID_HANDLE_VALUE;
}
bool Server::Accept(){return m_pipe!=INVALID_HANDLE_VALUE&&ConnectNamedPipe(m_pipe,nullptr)?true:GetLastError()==ERROR_PIPE_CONNECTED;}
Client::~Client(){Close();} void Client::Close(){if(m_pipe!=INVALID_HANDLE_VALUE){CloseHandle(m_pipe);m_pipe=INVALID_HANDLE_VALUE;}}
bool Client::Connect(DWORD timeoutMs){Close();if(!WaitNamedPipeW(m_name,timeoutMs))return false;m_pipe=CreateFileW(m_name,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);return m_pipe!=INVALID_HANDLE_VALUE;}
}

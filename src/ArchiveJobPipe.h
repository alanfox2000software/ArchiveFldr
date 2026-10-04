#pragma once
#include "ArchiveJobProtocol.h"

namespace ArchiveJobPipe
{
class Server
{
public:
    explicit Server(const wchar_t* name = ArchiveJobProtocol::kPipeName) : m_name(name) {} ~Server();
    bool Listen();
    bool Accept();
    HANDLE Handle() const { return m_pipe; }
    HANDLE Detach() { HANDLE h=m_pipe; m_pipe=INVALID_HANDLE_VALUE; return h; }
    void Close();
private: HANDLE m_pipe = INVALID_HANDLE_VALUE; const wchar_t* m_name;
};
class Client
{
public:
    explicit Client(const wchar_t* name = ArchiveJobProtocol::kPipeName) : m_name(name) {} ~Client();
    bool Connect(DWORD timeoutMs = 5000);
    HANDLE Handle() const { return m_pipe; }
    HANDLE Detach() { HANDLE h=m_pipe; m_pipe=INVALID_HANDLE_VALUE; return h; }
    void Close();
private: HANDLE m_pipe = INVALID_HANDLE_VALUE; const wchar_t* m_name;
};
bool Send(HANDLE pipe, ArchiveJobProtocol::MessageType type, const std::wstring& payload);
bool Receive(HANDLE pipe, ArchiveJobProtocol::MessageType& type, std::wstring& payload);
}

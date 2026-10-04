#pragma once
#include "ArchiveJobProtocol.h"

namespace ArchiveJobPipe
{
class Server
{
public:
    Server() = default; ~Server();
    bool Listen();
    bool Accept();
    HANDLE Handle() const { return m_pipe; }
    void Close();
private: HANDLE m_pipe = INVALID_HANDLE_VALUE;
};
class Client
{
public:
    Client() = default; ~Client();
    bool Connect(DWORD timeoutMs = 5000);
    HANDLE Handle() const { return m_pipe; }
    void Close();
private: HANDLE m_pipe = INVALID_HANDLE_VALUE;
};
bool Send(HANDLE pipe, ArchiveJobProtocol::MessageType type, const std::wstring& payload);
bool Receive(HANDLE pipe, ArchiveJobProtocol::MessageType& type, std::wstring& payload);
}

#pragma once
#include "ArchiveJobPipe.h"
namespace ArchiveJobClient
{
bool EnsureManager();
bool Submit(const ArchiveJobProtocol::JobRequest& request);
bool Control(const GUID& id, ArchiveJobProtocol::Control command);
bool Query(const GUID& id, ArchiveJobProtocol::MessageType& type, std::wstring& payload);
}

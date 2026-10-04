#pragma once
#include "ArchiveJobPipe.h"
namespace ArchiveJobClient
{
bool EnsureManager();
bool Submit(const ArchiveJobProtocol::JobRequest& request);
}

// Shared wire protocol for ArchiveFldrJobManager and worker processes.
#pragma once
#include "stdafx.h"

namespace ArchiveJobProtocol
{
constexpr DWORD kProtocolVersion = 1;
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\ArchiveFldrJobManager";
constexpr DWORD kMaxPayloadBytes = 1024 * 1024;

enum class MessageType : uint32_t
{
    Submit = 1, Progress = 2, State = 3, Control = 4,
    Result = 5, Hello = 6, List = 7
};
enum class JobKind : uint32_t { Compress = 1, Extract = 2 };
enum class JobState : uint32_t
{
    Queued = 1, Running = 2, Pausing = 3, Paused = 4,
    Cancelling = 5, Cancelled = 6, Completed = 7, Failed = 8
};
enum class Control : uint32_t { Cancel = 1, Pause = 2, Resume = 3 };

#pragma pack(push, 1)
struct MessageHeader
{
    uint32_t magic = 0x314A4641; // AFJ1
    uint32_t version = kProtocolVersion;
    uint32_t type = 0;
    uint32_t payloadBytes = 0;
};
#pragma pack(pop)

struct JobRequest
{
    GUID id{};
    JobKind kind = JobKind::Compress;
    std::wstring output;
    std::wstring archive;
    std::vector<std::wstring> sources;
    std::wstring format;
    int level = 5;
    int threads = 0;
    bool solid = false;
    bool encryptNames = false;
};

struct Progress
{
    GUID id{};
    JobState state = JobState::Queued;
    uint64_t completed = 0;
    uint64_t total = 0;
    int percent = 0;
    std::wstring current;
    std::wstring error;
};

inline std::wstring GuidText(const GUID& id)
{
    wchar_t s[40] = {}; StringFromGUID2(id, s, ARRAYSIZE(s)); return s;
}
inline bool ParseGuid(const std::wstring& s, GUID& id)
{ return SUCCEEDED(CLSIDFromString(s.c_str(), &id)); }
inline std::wstring Escape(const std::wstring& s)
{
    std::wstring r; for (wchar_t c : s) { if (c == L'%' || c == L'\n' || c == L'\r' || c == L'=') r += L'%'; r += c; } return r;
}
inline std::wstring Unescape(const std::wstring& s)
{
    std::wstring r; bool esc = false; for (wchar_t c : s) { if (esc) { r += c; esc = false; } else if (c == L'%') esc = true; else r += c; } return r;
}
inline void Put(std::wstring& p, const wchar_t* key, const std::wstring& value) { p += key; p += L"="; p += Escape(value); p += L"\n"; }
inline void Put(std::wstring& p, const wchar_t* key, uint64_t value) { Put(p, key, std::to_wstring(value)); }
inline std::wstring Get(const std::wstring& p, const std::wstring& key)
{
    const std::wstring prefix = key + L"="; size_t at = 0;
    while (at < p.size()) { size_t end = p.find(L'\n', at); if (end == std::wstring::npos) end = p.size(); if (p.compare(at, prefix.size(), prefix) == 0) return Unescape(p.substr(at + prefix.size(), end - at - prefix.size())); at = end + 1; }
    return L"";
}
inline std::wstring Encode(const JobRequest& j)
{
    std::wstring p; Put(p,L"id",GuidText(j.id)); Put(p,L"kind",(uint64_t)j.kind); Put(p,L"output",j.output); Put(p,L"archive",j.archive); Put(p,L"format",j.format); Put(p,L"level",(uint64_t)j.level); Put(p,L"threads",(uint64_t)j.threads); Put(p,L"solid",(uint64_t)j.solid); Put(p,L"encryptNames",(uint64_t)j.encryptNames); for(auto& s:j.sources) Put(p,L"source",s); return p;
}
inline bool Decode(const std::wstring& p, JobRequest& j)
{
    if (!ParseGuid(Get(p,L"id"),j.id)) return false; j.kind=(JobKind)_wtoi(Get(p,L"kind").c_str()); j.output=Get(p,L"output"); j.archive=Get(p,L"archive"); j.format=Get(p,L"format"); j.level=_wtoi(Get(p,L"level").c_str()); j.threads=_wtoi(Get(p,L"threads").c_str()); j.solid=_wtoi(Get(p,L"solid").c_str())!=0; j.encryptNames=_wtoi(Get(p,L"encryptNames").c_str())!=0; size_t at=0; while((at=p.find(L"source=",at))!=std::wstring::npos){size_t e=p.find(L'\n',at);j.sources.push_back(Unescape(p.substr(at+7,(e==std::wstring::npos?p.size():e)-at-7)));at=e==std::wstring::npos?p.size():e+1;} return !j.output.empty() || !j.archive.empty();
}
inline bool Write(HANDLE pipe, MessageType type, const std::wstring& payload)
{
    MessageHeader h; h.type=(uint32_t)type; h.payloadBytes=(uint32_t)(payload.size()*sizeof(wchar_t)); if(h.payloadBytes>kMaxPayloadBytes)return false; DWORD n=0; if(!WriteFile(pipe,&h,sizeof(h),&n,nullptr)||n!=sizeof(h))return false; return !h.payloadBytes || (WriteFile(pipe,payload.data(),h.payloadBytes,&n,nullptr)&&n==h.payloadBytes);
}
inline bool Read(HANDLE pipe, MessageType& type, std::wstring& payload)
{
    MessageHeader h{}; DWORD n=0; if(!ReadFile(pipe,&h,sizeof(h),&n,nullptr)||n!=sizeof(h)||h.magic!=0x314A4641||h.version!=kProtocolVersion||h.payloadBytes>kMaxPayloadBytes||h.payloadBytes%sizeof(wchar_t))return false; payload.assign(h.payloadBytes/sizeof(wchar_t),L'\0'); if(h.payloadBytes&&!ReadFile(pipe,payload.data(),h.payloadBytes,&n,nullptr))return false; type=(MessageType)h.type; return true;
}
}

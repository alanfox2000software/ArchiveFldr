// UnrarEngine.cpp — see UnrarEngine.h
#include "stdafx.h"
#include "UnrarEngine.h"
#include "ThirdParty.h"

#include <mutex>

// ═════════════════════════════════════════════════════════════════════════
// unrar.dll ABI
//
// Transcribed from RARLAB's dll.hpp. Two details matter and are easy to
// get wrong, both of which would corrupt memory inside Explorer:
//
//   1. The header is #pragma pack(1). Without it `CmtBuf` and everything
//      after it sits four bytes further along in a 64-bit build.
//   2. The entry points are PASCAL, i.e. __stdcall, not __cdecl.
//
// RARHeaderDataEx is transcribed at its newest (RAR_DLL_VERSION 10) size.
// That is the safe direction: an older unrar.dll simply writes fewer
// fields and leaves the tail as we zeroed it, whereas a struct smaller
// than the DLL expects would be written past its end.
// ═════════════════════════════════════════════════════════════════════════
namespace {

#pragma pack(push, 1)

struct RARHeaderDataEx
{
    char         ArcName[1024];
    wchar_t      ArcNameW[1024];
    char         FileName[1024];
    wchar_t      FileNameW[1024];
    unsigned int Flags;
    unsigned int PackSize;
    unsigned int PackSizeHigh;
    unsigned int UnpSize;
    unsigned int UnpSizeHigh;
    unsigned int HostOS;
    unsigned int FileCRC;
    unsigned int FileTime;
    unsigned int UnpVer;
    unsigned int Method;
    unsigned int FileAttr;
    char*        CmtBuf;
    unsigned int CmtBufSize;
    unsigned int CmtSize;
    unsigned int CmtState;
    unsigned int DictSize;
    unsigned int HashType;
    char         Hash[32];
    unsigned int RedirType;
    wchar_t*     RedirName;
    unsigned int RedirNameSize;
    unsigned int DirTarget;
    unsigned int MtimeLow;
    unsigned int MtimeHigh;
    unsigned int CtimeLow;
    unsigned int CtimeHigh;
    unsigned int AtimeLow;
    unsigned int AtimeHigh;
    wchar_t*     ArcNameEx;
    unsigned int ArcNameExSize;
    wchar_t*     FileNameEx;
    unsigned int FileNameExSize;
    unsigned int Reserved[982];
};

typedef int (CALLBACK* UNRARCALLBACK)(UINT msg, LPARAM UserData,
                                      LPARAM P1, LPARAM P2);

struct RAROpenArchiveDataEx
{
    char*         ArcName;
    wchar_t*      ArcNameW;
    unsigned int  OpenMode;
    unsigned int  OpenResult;
    char*         CmtBuf;
    unsigned int  CmtBufSize;
    unsigned int  CmtSize;
    unsigned int  CmtState;
    unsigned int  Flags;
    UNRARCALLBACK Callback;
    LPARAM        UserData;
    unsigned int  OpFlags;
    wchar_t*      CmtBufW;
    wchar_t*      MarkOfTheWeb;
    unsigned int  Reserved[23];
};

#pragma pack(pop)

// The layout above is load-bearing: unrar.dll writes into these fields by
// offset, so a single wrong one corrupts memory inside Explorer. Pin the
// offsets that move if the packing pragma is ever lost or a field is
// mistyped — all four of the arrays are byte/UTF-16 so they are fixed, but
// everything after the first pointer shifts without pack(1).
static_assert(sizeof(wchar_t) == 2, "unrar.dll's wchar_t is UTF-16");
static_assert(offsetof(RARHeaderDataEx, FileNameW) == 4096, "RAR ABI");
static_assert(offsetof(RARHeaderDataEx, Flags)     == 6144, "RAR ABI");
static_assert(offsetof(RARHeaderDataEx, FileAttr)  == 6184, "RAR ABI");
static_assert(offsetof(RARHeaderDataEx, CmtBuf)    == 6188, "RAR ABI: packing");
static_assert(offsetof(RARHeaderDataEx, MtimeLow)  ==
              (sizeof(void*) == 8 ? 6268 : 6260), "RAR ABI: packing");
static_assert(sizeof(RARHeaderDataEx) ==
              (sizeof(void*) == 8 ? 10244 : 10228), "RAR ABI: total size");
static_assert(offsetof(RAROpenArchiveDataEx, Callback) ==
              (sizeof(void*) == 8 ? 48 : 36), "RAR ABI: packing");

// ── Return codes ────────────────────────────────────────────────────────
constexpr int ERAR_SUCCESS          = 0;
constexpr int ERAR_END_ARCHIVE      = 10;
constexpr int ERAR_BAD_DATA         = 12;
constexpr int ERAR_BAD_ARCHIVE      = 13;
constexpr int ERAR_UNKNOWN_FORMAT   = 14;
constexpr int ERAR_EOPEN            = 15;
constexpr int ERAR_MISSING_PASSWORD = 22;
constexpr int ERAR_BAD_PASSWORD     = 24;

// ── Open modes / operations ─────────────────────────────────────────────
constexpr int RAR_OM_LIST_INCSPLIT = 2;
constexpr int RAR_OM_EXTRACT       = 1;
constexpr int RAR_SKIP             = 0;
constexpr int RAR_TEST             = 1;
constexpr int RAR_EXTRACT          = 2;

// ── Header flags ────────────────────────────────────────────────────────
constexpr unsigned RHDF_ENCRYPTED = 0x04;
constexpr unsigned RHDF_SOLID     = 0x10;
constexpr unsigned RHDF_DIRECTORY = 0x20;

// ── Archive flags ───────────────────────────────────────────────────────
constexpr unsigned ROADF_ENCHEADERS = 0x0080;

// ── Callback messages ───────────────────────────────────────────────────
constexpr UINT UCM_PROCESSDATA  = 1;
constexpr UINT UCM_NEEDPASSWORD = 2;
constexpr UINT UCM_NEEDPASSWORDW = 4;

struct UnrarApi
{
    HANDLE (PASCAL* OpenArchiveEx)(RAROpenArchiveDataEx*);
    int    (PASCAL* CloseArchive)(HANDLE);
    int    (PASCAL* ReadHeaderEx)(HANDLE, RARHeaderDataEx*);
    int    (PASCAL* ProcessFileW)(HANDLE, int, wchar_t*, wchar_t*);
    void   (PASCAL* SetCallback)(HANDLE, UNRARCALLBACK, LPARAM);
    int    (PASCAL* GetDllVersion)();
};

struct UnrarLib
{
    bool         tried  = false;
    HMODULE      module = nullptr;
    std::wstring path;
    std::wstring error;
    UnrarApi     api{};
    int          version = 0;
};

std::mutex g_mutex;
UnrarLib   g_lib;

UnrarLib& Lib()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_lib.tried) return g_lib;
    g_lib.tried = true;

    g_lib.module = ThirdParty::LoadComponent(L"Unrar", &g_lib.path);
    if (!g_lib.module)
    {
        g_lib.error = g_lib.path.empty()
            ? L"unrar.dll was not found."
            : L"unrar.dll was found but Windows refused to load it "
              L"(most often a 32-bit DLL in a 64-bit Explorer, or the "
              L"other way round).";
        return g_lib;
    }

    auto get = [](const char* n) { return GetProcAddress(g_lib.module, n); };

    g_lib.api.OpenArchiveEx = reinterpret_cast<decltype(g_lib.api.OpenArchiveEx)>(get("RAROpenArchiveEx"));
    g_lib.api.CloseArchive  = reinterpret_cast<decltype(g_lib.api.CloseArchive)>(get("RARCloseArchive"));
    g_lib.api.ReadHeaderEx  = reinterpret_cast<decltype(g_lib.api.ReadHeaderEx)>(get("RARReadHeaderEx"));
    g_lib.api.ProcessFileW  = reinterpret_cast<decltype(g_lib.api.ProcessFileW)>(get("RARProcessFileW"));
    g_lib.api.SetCallback   = reinterpret_cast<decltype(g_lib.api.SetCallback)>(get("RARSetCallback"));
    g_lib.api.GetDllVersion = reinterpret_cast<decltype(g_lib.api.GetDllVersion)>(get("RARGetDllVersion"));

    if (!g_lib.api.OpenArchiveEx || !g_lib.api.CloseArchive ||
        !g_lib.api.ReadHeaderEx  || !g_lib.api.ProcessFileW)
    {
        g_lib.error = L"That unrar.dll does not export the Unicode entry "
                      L"points (RAROpenArchiveEx / RARProcessFileW). It is "
                      L"too old — version 4 or newer is needed.";
        g_lib.module = nullptr;
        return g_lib;
    }

    if (g_lib.api.GetDllVersion) g_lib.version = g_lib.api.GetDllVersion();
    return g_lib;
}

// Normalise a RAR path to the engine's convention: forward slashes, no
// leading slash. RAR stores backslashes on Windows.
std::wstring Normalise(const wchar_t* raw)
{
    std::wstring s = raw ? raw : L"";
    for (auto& ch : s) if (ch == L'\\') ch = L'/';
    while (!s.empty() && s.front() == L'/') s.erase(s.begin());
    return s;
}

uint64_t Combine(unsigned lo, unsigned hi)
{
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

// Progress plumbing for RARProcessFileW, which reports through the
// callback rather than a return value.
struct CallbackCtx
{
    ProgressFn*  cb       = nullptr;
    std::wstring current;
    uint64_t     done     = 0;
    uint64_t     total    = 0;
    int          lastPct  = -1;
    bool         needPassword = false;
};

int CALLBACK RarCallback(UINT msg, LPARAM userData, LPARAM p1, LPARAM p2)
{
    auto* ctx = reinterpret_cast<CallbackCtx*>(userData);
    if (!ctx) return 0;

    switch (msg)
    {
    case UCM_PROCESSDATA:
        ctx->done += (uint64_t)p2;
        if (ctx->cb && *ctx->cb && ctx->total)
        {
            int pct = (int)((ctx->done * 100) / ctx->total);
            if (pct > 100) pct = 100;
            if (pct != ctx->lastPct)
            {
                ctx->lastPct = pct;
                (*ctx->cb)(pct, ctx->current);
            }
        }
        return 1;

    case UCM_NEEDPASSWORD:
    case UCM_NEEDPASSWORDW:
        // Returning -1 aborts cleanly. Prompting from inside a shell
        // callback would mean a modal dialog on Explorer's UI thread
        // during a drag, so encrypted archives are reported instead.
        ctx->needPassword = true;
        (void)p1; (void)p2;
        return -1;

    default:
        return 0;
    }
}

// Synthesise the parent chain for "a/b/c.txt" so folders exist in the view
// even when the archive stores no directory headers for them.
void EnsureDirs(std::vector<ArchiveEntry>& out,
                std::map<std::wstring, bool>& known,
                const std::wstring& dirWithSlash)
{
    if (dirWithSlash.empty() || known.count(dirWithSlash)) return;

    std::wstring trimmed = dirWithSlash.substr(0, dirWithSlash.size() - 1);
    size_t slash = trimmed.rfind(L'/');
    if (slash != std::wstring::npos)
        EnsureDirs(out, known, trimmed.substr(0, slash + 1));

    known[dirWithSlash] = true;
    ArchiveEntry d;
    d.fullPath    = dirWithSlash;
    d.isDirectory = true;
    d.engineIndex = -1;
    d.name        = (slash == std::wstring::npos) ? trimmed
                                                  : trimmed.substr(slash + 1);
    out.push_back(std::move(d));
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════
// Public helpers
// ═════════════════════════════════════════════════════════════════════════
bool IsUnrarAvailable()      { return Lib().module != nullptr; }
std::wstring GetUnrarPath()  { return Lib().path; }

// ═════════════════════════════════════════════════════════════════════════
// CUnrarEngine
// ═════════════════════════════════════════════════════════════════════════
CUnrarEngine::CUnrarEngine()  { m_readOnly = true; }
CUnrarEngine::~CUnrarEngine() { Close(); }

bool CUnrarEngine::Open(const std::wstring& path)
{
    Close();
    m_filePath = path;
    if (!Lib().module) return false;
    if (!PathFileExistsW(path.c_str())) return false;

    if (!BuildEntryList()) return false;
    m_open = true;
    return true;
}

bool CUnrarEngine::Create(const std::wstring&) { return false; }

void CUnrarEngine::Close()
{
    m_open = false;
    m_allEntries.clear();
    m_comment.clear();
    m_encryptedNames = false;
}

bool CUnrarEngine::BuildEntryList()
{
    UnrarLib& lib = Lib();
    m_allEntries.clear();

    std::vector<char> cmt(16384);

    RAROpenArchiveDataEx od{};
    od.ArcNameW    = const_cast<wchar_t*>(m_filePath.c_str());
    od.OpenMode    = RAR_OM_LIST_INCSPLIT;
    od.CmtBuf      = cmt.data();
    od.CmtBufSize  = (unsigned)cmt.size();

    HANDLE h = lib.api.OpenArchiveEx(&od);
    if (!h || od.OpenResult != ERAR_SUCCESS) return false;

    if (od.Flags & ROADF_ENCHEADERS) m_encryptedNames = true;

    if (od.CmtSize > 0 && od.CmtSize <= cmt.size())
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, cmt.data(),
                                    (int)od.CmtSize, nullptr, 0);
        if (n > 0)
        {
            m_comment.resize((size_t)n);
            MultiByteToWideChar(CP_UTF8, 0, cmt.data(), (int)od.CmtSize,
                                &m_comment[0], n);
        }
    }

    // Abort any password prompt rather than blocking the shell.
    CallbackCtx ctx;
    if (lib.api.SetCallback)
        lib.api.SetCallback(h, RarCallback, reinterpret_cast<LPARAM>(&ctx));

    std::map<std::wstring, bool> known;
    auto* hd = new (std::nothrow) RARHeaderDataEx();   // ~6 KB — not on the stack
    if (!hd) { lib.api.CloseArchive(h); return false; }

    int rc;
    while ((rc = lib.api.ReadHeaderEx(h, hd)) == ERAR_SUCCESS)
    {
        const std::wstring full = Normalise(hd->FileNameW);
        const bool isDir = (hd->Flags & RHDF_DIRECTORY) != 0;

        if (!full.empty())
        {
            std::wstring p = full;
            if (isDir && p.back() != L'/') p += L'/';

            size_t slash = (isDir ? p.substr(0, p.size() - 1) : p).rfind(L'/');
            std::wstring parent = (slash == std::wstring::npos)
                                ? L"" : p.substr(0, slash + 1);
            if (!parent.empty()) EnsureDirs(m_allEntries, known, parent);

            if (isDir)
            {
                if (!known.count(p))
                {
                    known[p] = true;
                    ArchiveEntry e;
                    e.isDirectory = true;
                    e.fullPath    = p;
                    e.name        = p.substr(parent.size(),
                                             p.size() - parent.size() - 1);
                    e.engineIndex = -1;
                    m_allEntries.push_back(std::move(e));
                }
            }
            else
            {
                ArchiveEntry e;
                e.isDirectory       = false;
                e.fullPath          = p;
                e.name              = p.substr(parent.size());
                e.uncompressedSize  = Combine(hd->UnpSize,  hd->UnpSizeHigh);
                e.compressedSize    = Combine(hd->PackSize, hd->PackSizeHigh);
                e.crc32             = hd->FileCRC;
                e.hasCrc            = true;   // RAR always stores one
                e.isEncrypted       = (hd->Flags & RHDF_ENCRYPTED) != 0;
                e.engineIndex       = (int64_t)m_allEntries.size();

                // RAR gives mtime either as a 64-bit FILETIME (newer DLLs)
                // or as a DOS stamp; prefer the former when present.
                if (hd->MtimeLow || hd->MtimeHigh)
                {
                    e.modifiedTime.dwLowDateTime  = hd->MtimeLow;
                    e.modifiedTime.dwHighDateTime = hd->MtimeHigh;
                }
                else
                {
                    FILETIME ft{};
                    WORD date = (WORD)(hd->FileTime >> 16);
                    WORD time = (WORD)(hd->FileTime & 0xFFFF);
                    if (DosDateTimeToFileTime(date, time, &ft)) e.modifiedTime = ft;
                }

                wchar_t meth[32];
                swprintf_s(meth, 32, L"RAR%d m%u",
                           hd->UnpVer >= 50 ? 5 : 4,
                           hd->Method >= 0x30 ? hd->Method - 0x30 : hd->Method);
                e.compressionMethod = meth;
                if (hd->Flags & RHDF_SOLID) e.compressionMethod += L" solid";

                m_allEntries.push_back(std::move(e));
            }
        }

        if (lib.api.ProcessFileW(h, RAR_SKIP, nullptr, nullptr) != ERAR_SUCCESS)
            break;
    }

    delete hd;
    lib.api.CloseArchive(h);

    // A header-encrypted archive we could not open reads as empty; say so
    // rather than presenting an empty folder as if that were the truth.
    if (m_allEntries.empty() && rc != ERAR_END_ARCHIVE && !m_encryptedNames)
        return false;

    return true;
}

std::vector<ArchiveEntry> CUnrarEngine::List(const std::wstring& dirPath)
{
    std::vector<ArchiveEntry> result;
    if (!m_open) return result;

    std::wstring prefix = dirPath;
    if (!prefix.empty() && prefix.back() != L'/') prefix += L'/';

    for (const auto& e : m_allEntries)
    {
        if (e.fullPath.size() <= prefix.size()) continue;
        if (_wcsnicmp(e.fullPath.c_str(), prefix.c_str(), prefix.size()) != 0)
            continue;

        // Direct children only: no further '/' beyond the prefix (ignoring
        // the trailing one a directory carries).
        std::wstring rest = e.fullPath.substr(prefix.size());
        if (e.isDirectory && !rest.empty() && rest.back() == L'/')
            rest.pop_back();
        if (rest.find(L'/') != std::wstring::npos) continue;

        result.push_back(e);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────
// Run — one pass over the archive, unpacking or testing what was asked for
// ─────────────────────────────────────────────────────────────────────────
bool CUnrarEngine::Run(const std::set<std::wstring>& wanted,
                       const std::wstring& destDir, ProgressFn cb)
{
    UnrarLib& lib = Lib();
    if (!lib.module) return false;

    const bool testOnly = destDir.empty();

    RAROpenArchiveDataEx od{};
    od.ArcNameW = const_cast<wchar_t*>(m_filePath.c_str());
    od.OpenMode = RAR_OM_EXTRACT;

    HANDLE h = lib.api.OpenArchiveEx(&od);
    if (!h || od.OpenResult != ERAR_SUCCESS) return false;

    CallbackCtx ctx;
    ctx.cb = &cb;
    for (const auto& e : m_allEntries)
        if (!e.isDirectory && (wanted.empty() || wanted.count(e.fullPath)))
            ctx.total += e.uncompressedSize;

    if (lib.api.SetCallback)
        lib.api.SetCallback(h, RarCallback, reinterpret_cast<LPARAM>(&ctx));

    auto* hd = new (std::nothrow) RARHeaderDataEx();
    if (!hd) { lib.api.CloseArchive(h); return false; }

    bool ok = true;
    std::wstring dest = destDir;
    if (!dest.empty() && dest.back() == L'\\') dest.pop_back();

    while (lib.api.ReadHeaderEx(h, hd) == ERAR_SUCCESS)
    {
        const std::wstring full = Normalise(hd->FileNameW);
        const bool take = wanted.empty() || wanted.count(full) ||
                          // a wanted directory pulls in everything beneath it
                          [&] {
                              for (const auto& w : wanted)
                                  if (!w.empty() && w.back() == L'/' &&
                                      full.size() > w.size() &&
                                      _wcsnicmp(full.c_str(), w.c_str(),
                                                w.size()) == 0)
                                      return true;
                              return false;
                          }();

        int op = RAR_SKIP;
        if (take) op = testOnly ? RAR_TEST : RAR_EXTRACT;

        ctx.current = full;

        int rc = lib.api.ProcessFileW(
            h, op,
            (op == RAR_EXTRACT) ? const_cast<wchar_t*>(dest.c_str()) : nullptr,
            nullptr);

        if (rc != ERAR_SUCCESS)
        {
            ok = false;
            if (rc == ERAR_MISSING_PASSWORD || rc == ERAR_BAD_PASSWORD ||
                ctx.needPassword)
            {
                MessageBoxW(nullptr,
                    L"This RAR archive is encrypted.\n\n"
                    L"ShellNSE cannot prompt for a password from inside "
                    L"Explorer. Open the archive in WinRAR or 7-Zip to "
                    L"extract it.",
                    L"ShellNSE", MB_ICONINFORMATION | MB_OK);
            }
            break;
        }
    }

    delete hd;
    lib.api.CloseArchive(h);

    if (ok && cb) cb(100, L"");
    return ok;
}

bool CUnrarEngine::ExtractFile(const ArchiveEntry& e,
                               const std::wstring& destDir, ProgressFn cb)
{
    if (!m_open) return false;
    SHCreateDirectoryExW(nullptr, destDir.c_str(), nullptr);
    return Run({ e.fullPath }, destDir, cb);
}

bool CUnrarEngine::ExtractAll(const std::wstring& destDir, ProgressFn cb)
{
    if (!m_open) return false;
    SHCreateDirectoryExW(nullptr, destDir.c_str(), nullptr);
    return Run({}, destDir, cb);
}

bool CUnrarEngine::Test(ProgressFn cb)
{
    return m_open && Run({}, L"", cb);
}

EngineCaps CUnrarEngine::GetCaps() const
{
    EngineCaps caps;
    caps.engineName = L"RAR";

    UnrarLib& lib = Lib();
    caps.backendPath = lib.path;

    if (lib.module)
    {
        caps.isStub     = false;
        caps.canExtract = true;
        caps.canTest    = true;
        // unrar.dll is a decoder. Creating or editing RAR archives needs a
        // WinRAR licence and rar.exe, so those stay off.
        caps.canAdd = caps.canDelete = caps.canRename = false;
    }
    else
    {
        caps.isStub = true;
        caps.unavailableReason =
            (lib.error.empty() ? std::wstring() : lib.error + L"\n\n") +
            ThirdParty::DescribeSearch(L"Unrar");
    }
    return caps;
}

uint64_t CUnrarEngine::GetFileCount() const
{
    uint64_t n = 0;
    for (const auto& e : m_allEntries) if (!e.isDirectory) ++n;
    return n;
}

uint64_t CUnrarEngine::GetTotalSize() const
{
    uint64_t n = 0;
    for (const auto& e : m_allEntries) n += e.uncompressedSize;
    return n;
}

uint64_t CUnrarEngine::GetPackedSize() const
{
    uint64_t n = 0;
    for (const auto& e : m_allEntries) n += e.compressedSize;
    return n;
}

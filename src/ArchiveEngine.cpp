// ArchiveEngine.cpp
#include "stdafx.h"
#include "ArchiveEngine.h"
#include "GUIDs.h"
#include "SevenZipEngine.h"
#include "CodecEngine.h"
#include "UnrarEngine.h"
#include "Formats.h"

// ─────────────────────────────────────────────────────────
// CStubArchiveEngine
// ─────────────────────────────────────────────────────────
std::wstring CStubArchiveEngine::ExtToFormatName(const wchar_t* ext)
{
    return Formats::NameFor(ext);
}

bool CStubArchiveEngine::Open(const std::wstring& path)
{
    m_filePath   = path;
    LPCWSTR ext  = PathFindExtensionW(path.c_str());
    m_formatName = ExtToFormatName(ext);
    m_open       = PathFileExistsW(path.c_str()) != 0;
    m_readOnly   = false;
    if (m_open) BuildSampleEntries();
    return m_open;
}

bool CStubArchiveEngine::Create(const std::wstring& path)
{
    m_filePath   = path;
    LPCWSTR ext  = PathFindExtensionW(path.c_str());
    m_formatName = ExtToFormatName(ext);
    m_open       = true;
    m_readOnly   = false;
    return true;
}

void CStubArchiveEngine::Close()
{
    m_open = false;
    m_allEntries.clear();
}

void CStubArchiveEngine::BuildSampleEntries()
{
    m_allEntries.clear();
    // Simulate a realistic archive tree
    struct Def {
        const wchar_t* path; bool dir;
        uint64_t sz; uint64_t csz; const wchar_t* meth;
    } defs[] = {
        {L"docs/",              true,       0,       0, L"Store"  },
        {L"docs/readme.txt",   false,   4096,    1500, L"Deflate" },
        {L"docs/license.txt",  false,   8192,    2800, L"Deflate" },
        {L"docs/manual.pdf",   false, 524288,  420000, L"Deflate" },
        {L"src/",               true,       0,       0, L"Store"  },
        {L"src/main.cpp",      false,  32768,    9200, L"Deflate" },
        {L"src/utils.cpp",     false,  16384,    5100, L"Deflate" },
        {L"src/utils.h",       false,   4096,    1200, L"Deflate" },
        {L"src/resource.rc",   false,   8192,    2100, L"Deflate" },
        {L"src/stdafx.h",      false,   6144,    1800, L"Deflate" },
        {L"bin/",               true,       0,       0, L"Store"  },
        {L"bin/app.exe",       false,1048576,  620000, L"LZMA"   },
        {L"bin/app.dll",       false, 262144,  180000, L"LZMA"   },
        {L"bin/app.pdb",       false, 524288,  400000, L"LZMA"   },
        {L"images/",            true,       0,       0, L"Store"  },
        {L"images/icon.ico",   false,  16384,   15500, L"Store"  },
        {L"images/banner.png", false, 102400,   98000, L"Store"  },
        {L"images/splash.bmp", false, 307200,  290000, L"Deflate"},
        {L"config.ini",        false,   1024,     480, L"Deflate" },
        {L"changelog.txt",     false,  20480,    6200, L"Deflate" },
        {L"install.bat",       false,   2048,     760, L"Deflate" },
        {L"uninstall.bat",     false,   1536,     600, L"Deflate" },
    };

    SYSTEMTIME st; GetLocalTime(&st);
    FILETIME ft; SystemTimeToFileTime(&st, &ft);

    for (auto& d : defs) {
        ArchiveEntry e;
        e.fullPath          = d.path;
        e.isDirectory       = d.dir;
        e.uncompressedSize  = d.sz;
        e.compressedSize    = d.csz;
        e.compressionMethod = d.meth;
        e.modifiedTime      = ft;
        e.crc32             = d.dir ? 0 :
            (uint32_t)(d.sz * 0x5A3C9F17ULL + 0xDEADBEEF);

        // Extract name (last component)
        std::wstring fp = d.path;
        if (!fp.empty() && fp.back()==L'/') fp.pop_back();
        size_t sl = fp.rfind(L'/');
        e.name = (sl==std::wstring::npos) ? fp : fp.substr(sl+1);
        m_allEntries.push_back(std::move(e));
    }
}

// ── List ──────────────────────────────────────────────────
std::vector<ArchiveEntry> CStubArchiveEngine::List(
    const std::wstring& dirPath)
{
    std::vector<ArchiveEntry> result;
    if (!m_open) return result;

    std::wstring prefix = dirPath;
    if (!prefix.empty() && prefix.back()!=L'/') prefix+=L'/';

    std::unordered_set<std::wstring> seen;

    for (auto& e : m_allEntries) {
        std::wstring fp = e.fullPath;

        if (prefix.empty()) {
            // Root level: no slash in path (or dir with single slash at end)
            std::wstring noTrail = fp;
            if (!noTrail.empty()&&noTrail.back()==L'/') noTrail.pop_back();
            if (noTrail.find(L'/') == std::wstring::npos) {
                if (!seen.count(fp)) { seen.insert(fp); result.push_back(e); }
            }
        } else {
            // Must start with prefix
            if (fp.size() <= prefix.size()) continue;
            if (fp.substr(0,prefix.size()) != prefix) continue;
            std::wstring rest = fp.substr(prefix.size());
            if (rest.empty()) continue;
            size_t sl = rest.find(L'/');
            if (sl == std::wstring::npos) {
                // Direct file child
                if (!seen.count(fp)) { seen.insert(fp); result.push_back(e); }
            } else {
                // Subdirectory: emit only the first component
                std::wstring sub = prefix + rest.substr(0,sl+1);
                if (!seen.count(sub)) {
                    seen.insert(sub);
                    for (auto& de : m_allEntries)
                        if (de.fullPath == sub) { result.push_back(de); break; }
                }
            }
        }
    }

    // Sort: directories first, then alphabetical
    std::sort(result.begin(), result.end(),
        [](const ArchiveEntry& a, const ArchiveEntry& b){
            if (a.isDirectory != b.isDirectory)
                return a.isDirectory > b.isDirectory;
            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });
    return result;
}

// ── Extract ───────────────────────────────────────────────
bool CStubArchiveEngine::ExtractAll(
    const std::wstring& destDir, ProgressFn cb)
{
    SHCreateDirectoryExW(nullptr, destDir.c_str(), nullptr);
    int n = (int)m_allEntries.size();
    for (int i=0;i<n;i++) {
        Sleep(15);
        if (cb) cb((i+1)*100/n, m_allEntries[i].fullPath);
    }
    return true;
}

bool CStubArchiveEngine::ExtractFile(
    const ArchiveEntry& e, const std::wstring& destDir, ProgressFn cb)
{
    SHCreateDirectoryExW(nullptr, destDir.c_str(), nullptr);
    for (int p=0;p<=100;p+=10) { Sleep(10); if(cb) cb(p, e.fullPath); }
    return true;
}

// ── Add ───────────────────────────────────────────────────
bool CStubArchiveEngine::AddFile(
    const std::wstring& src, const std::wstring& archPath, ProgressFn cb)
{
    ArchiveEntry e;
    e.fullPath          = archPath.empty()
        ? PathFindFileNameW(src.c_str())
        : archPath + PathFindFileNameW(src.c_str());
    e.name              = PathFindFileNameW(src.c_str());
    e.isDirectory       = false;
    e.compressionMethod = L"Deflate";
    SYSTEMTIME st; GetLocalTime(&st); FILETIME ft;
    SystemTimeToFileTime(&st,&ft); e.modifiedTime = ft;

    // Get actual file size if exists
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fa)) {
        e.uncompressedSize = ((uint64_t)fa.nFileSizeHigh<<32)|fa.nFileSizeLow;
        e.compressedSize   = e.uncompressedSize * 6 / 10;
    }
    m_allEntries.push_back(e);
    for (int p=0;p<=100;p+=20) { Sleep(10); if(cb) cb(p, src); }
    return true;
}

// ── Delete ────────────────────────────────────────────────
bool CStubArchiveEngine::DeleteFile(const ArchiveEntry& e)
{
    auto it = std::remove_if(m_allEntries.begin(), m_allEntries.end(),
        [&](const ArchiveEntry& x){ return x.fullPath == e.fullPath; });
    m_allEntries.erase(it, m_allEntries.end());
    return true;
}

// ── Rename ────────────────────────────────────────────────
bool CStubArchiveEngine::Rename(
    const ArchiveEntry& e, const std::wstring& newName)
{
    for (auto& x : m_allEntries) {
        if (x.fullPath == e.fullPath) {
            // Replace last component
            std::wstring fp = x.fullPath;
            if (!fp.empty()&&fp.back()==L'/') fp.pop_back();
            size_t sl = fp.rfind(L'/');
            if (sl==std::wstring::npos) fp = newName;
            else fp = fp.substr(0,sl+1) + newName;
            x.fullPath = fp;
            x.name     = newName;
            return true;
        }
    }
    return false;
}

// ── Test ─────────────────────────────────────────────────
bool CStubArchiveEngine::Test(ProgressFn cb)
{
    int n = (int)m_allEntries.size();
    for (int i=0;i<n;i++) {
        Sleep(20);
        if (cb) cb((i+1)*100/n, m_allEntries[i].fullPath);
    }
    return true;
}

// ── Metadata ─────────────────────────────────────────────
std::wstring CStubArchiveEngine::GetFormatName() const { return m_formatName; }

// The stub has no decoder behind it: it lists plausible sample entries so
// the shell plumbing can be exercised, but it cannot produce one byte of
// real data. Say so, loudly, instead of letting the shell "succeed" with
// empty files.
EngineCaps CStubArchiveEngine::GetCaps() const
{
    EngineCaps c;
    c.engineName = m_formatName;
    c.isStub     = true;
    c.unavailableReason =
        L"ShellNSE has no engine wired up for " + m_formatName +
        L" archives yet, so it cannot read their real contents.\n\n"
        L"Engines are supplied as third-party DLLs under the ShellNSE "
        L"\"thirdparty\" folder (see thirdparty\\README.md). Only .7z is "
        L"implemented today, via thirdparty\\7z\\7z." +
        std::wstring((sizeof(void*) == 8) ? L"64" : L"32") + L".dll.";
    return c;
}
uint64_t CStubArchiveEngine::GetFileCount() const {
    return std::count_if(m_allEntries.begin(),m_allEntries.end(),
        [](const ArchiveEntry& e){ return !e.isDirectory; });
}
uint64_t CStubArchiveEngine::GetTotalSize() const {
    uint64_t s=0;
    for (auto& e:m_allEntries) s+=e.uncompressedSize;
    return s;
}
uint64_t CStubArchiveEngine::GetPackedSize() const {
    uint64_t s=0;
    for (auto& e:m_allEntries) s+=e.compressedSize;
    return s;
}

// ArchiveEngine.cpp — add this above CreateArchiveEngine()
// as a local fallback if GUIDs.h include doesn't resolve it:


// ─────────────────────────────────────────────────────────
// Factory
// ─────────────────────────────────────────────────────────
std::shared_ptr<IArchiveEngine> CreateArchiveEngine(const std::wstring& path)
{
    LPCWSTR ext = PathFindExtensionW(path.c_str());
    const Formats::Format* f = Formats::Find(ext);
    if (!f) return nullptr;

    // Note that the engine is chosen even when its DLL is missing. Each one
    // reports that honestly through GetCaps().unavailableReason — naming the
    // DLL and every path it was looked for — which is far more use than
    // quietly handing back a stub full of invented entries.
    switch (f->engine)
    {
    case Formats::EngineKind::Codec:
        // Brotli / LZ4 / LZ5 / Lizard / Zstandard: one stream, one file.
        return std::make_shared<CCodecEngine>(f->codec);

    case Formats::EngineKind::Unrar:
        return std::make_shared<CUnrarEngine>();

    case Formats::EngineKind::SevenZip:
        // The 7-Zip engine currently binds the .7z format class only, so
        // the other container types it could handle still fall through to
        // the placeholder below.
        if (_wcsicmp(ext, L".7z") == 0 || _wcsicmp(ext, L".7zip") == 0)
            return std::make_shared<C7zArchiveEngine>();
        break;

    case Formats::EngineKind::Wim:
        // wimlib is resolved and diagnosed, but the engine that drives it
        // is not written yet.
        break;
    }

    return std::make_shared<CStubArchiveEngine>();
}

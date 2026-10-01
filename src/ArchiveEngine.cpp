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
    // Deliberately empty.
    //
    // This used to invent a plausible-looking tree (bin/, docs/, images/,
    // changelog.txt, install.bat …) so the shell plumbing could be
    // exercised before any real engine existed. That was a mistake once
    // the engines landed: a format that fell through to this class showed
    // Explorer a directory listing of files that do not exist, and every
    // attempt to open one failed. Showing nothing, plus the explanation
    // in GetCaps(), is the honest answer.
    m_allEntries.clear();
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
        L"ShellNSE has no engine for " + m_formatName +
        L" archives, so it cannot show what is inside this file.\n\n"
        L"Engines are third-party DLLs placed under the ShellNSE "
        L"\"thirdparty\" folder — see thirdparty\\README.md. Installing "
        L"7-Zip's 7z." + std::wstring((sizeof(void*) == 8) ? L"64" : L"32") +
        L".dll covers most container formats, including this one.";
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
        if (IsUnrarAvailable()) return std::make_shared<CUnrarEngine>();
        // No unrar.dll — 7-Zip reads RAR too, so try that before giving up.
        if (Is7zEngineAvailable()) return std::make_shared<C7zArchiveEngine>();
        return std::make_shared<CUnrarEngine>();

    case Formats::EngineKind::Wim:
    case Formats::EngineKind::SevenZip:
        // Everything 7z.dll can read: .7z, .zip, .tar, .wim, .iso, .cab,
        // .gz, .xz, … The engine asks 7z.dll which handler fits rather
        // than assuming the .7z one, so these all open properly.
        return std::make_shared<C7zArchiveEngine>();
    }

    return std::make_shared<CStubArchiveEngine>();
}

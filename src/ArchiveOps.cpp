// ArchiveOps.cpp — see ArchiveOps.h
#include "stdafx.h"
#include "ArchiveOps.h"

namespace ArchiveOps {

// ─────────────────────────────────────────────────────────
// Path helpers
// ─────────────────────────────────────────────────────────
std::wstring Join(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty()) return name;
    std::wstring d = dir;
    if (d.back() != L'/') d += L'/';
    return d + name;
}

std::wstring ToWin32(const std::wstring& internalPath)
{
    std::wstring s = internalPath;
    while (!s.empty() && (s.back() == L'/' || s.back() == L'\\')) s.pop_back();
    for (auto& ch : s) if (ch == L'/') ch = L'\\';
    return s;
}

// ─────────────────────────────────────────────────────────
// Entry lookup
// ─────────────────────────────────────────────────────────
bool FindEntry(const EnginePtr& eng, const std::wstring& dir,
               const std::wstring& name, ArchiveEntry& out)
{
    if (!eng || name.empty()) return false;
    for (auto& e : eng->List(dir))
        if (_wcsicmp(e.name.c_str(), name.c_str()) == 0) { out = e; return true; }
    return false;
}

void Flatten(const EnginePtr& eng, const ArchiveEntry& e,
             std::vector<ArchiveEntry>& out)
{
    if (!eng) return;
    if (!e.isDirectory) { out.push_back(e); return; }

    auto children = eng->List(e.fullPath);
    if (children.empty())
    {
        out.push_back(e);          // empty folder — keep it
        return;
    }
    for (auto& c : children) Flatten(eng, c, out);
}

// ─────────────────────────────────────────────────────────
// Temp staging
// ─────────────────────────────────────────────────────────
std::wstring MakeTempDir(const std::wstring& tag)
{
    wchar_t tmp[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tmp)) return L"";

    std::wstring clean;
    for (wchar_t ch : tag)
        clean += (wcschr(L"\\/:*?\"<>|", ch) ? L'_' : ch);
    if (clean.size() > 48) clean.resize(48);
    if (clean.empty()) clean = L"archive";

    for (int attempt = 0; attempt < 64; ++attempt)
    {
        wchar_t suffix[32] = {};
        swprintf_s(suffix, 32, L"%04X%04X",
                   (unsigned)(GetCurrentProcessId() & 0xFFFF),
                   (unsigned)((GetTickCount() + attempt * 7919) & 0xFFFF));

        std::wstring dir = std::wstring(tmp) + L"ShellNSE\\" + clean + L"-" + suffix;
        if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        if (SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) == ERROR_SUCCESS)
            return dir;
    }
    return L"";
}

void RemoveTree(const std::wstring& dir)
{
    if (dir.empty()) return;
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    // SHFileOperation needs a double-null terminated path list.
    std::vector<wchar_t> from(dir.begin(), dir.end());
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc  = FO_DELETE;
    op.pFrom  = from.data();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT |
                FOF_NOCONFIRMMKDIR;
    SHFileOperationW(&op);
}

bool ExtractEntry(const EnginePtr& eng, const ArchiveEntry& e,
                  const std::wstring& destDir, std::wstring* produced)
{
    if (!eng || destDir.empty()) return false;

    // The engine lays files out as <destDir>\<path inside the archive>.
    std::wstring disk = destDir;
    if (!disk.empty() && disk.back() != L'\\') disk += L'\\';
    disk += ToWin32(e.fullPath);
    if (produced) *produced = disk;

    if (!eng->ExtractFile(e, destDir, nullptr)) return false;

    // Directories legitimately produce no file.
    if (e.isDirectory) return true;
    return GetFileAttributesW(disk.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ─────────────────────────────────────────────────────────
// Capability gates
// ─────────────────────────────────────────────────────────
static void Explain(HWND hwnd, const std::wstring& head, const std::wstring& why)
{
    std::wstring msg = head;
    if (!why.empty()) msg += L"\n\n" + why;
    MessageBoxW(hwnd, msg.c_str(), L"ShellNSE", MB_ICONWARNING | MB_OK);
}

bool EnsureCanRead(HWND hwnd, const EnginePtr& eng)
{
    if (!eng)
    {
        Explain(hwnd, L"This archive is not open.", L"");
        return false;
    }
    EngineCaps caps = eng->GetCaps();
    if (caps.canExtract) return true;

    Explain(hwnd, L"ShellNSE cannot read the contents of this archive.",
            caps.unavailableReason);
    return false;
}

bool EnsureCanAdd(HWND hwnd, const EnginePtr& eng)
{
    if (!eng)
    {
        Explain(hwnd, L"This archive is not open.", L"");
        return false;
    }
    EngineCaps caps = eng->GetCaps();
    if (caps.canAdd) return true;

    Explain(hwnd,
        L"Files cannot be added to this archive.",
        caps.isStub && !caps.unavailableReason.empty()
            ? caps.unavailableReason
            : std::wstring(
                L"The " + caps.engineName + L" engine ShellNSE uses is "
                L"read-only: it can list, extract and test archives, but it "
                L"has no compressor, so nothing can be written back.\n\n"
                L"Extract the archive, add your files, and repack it with a "
                L"tool that can write this format."));
    return false;
}

// ─────────────────────────────────────────────────────────
// Data object → file system paths
// ─────────────────────────────────────────────────────────
static bool PathsFromHDrop(IDataObject* pdo, std::vector<std::wstring>& paths)
{
    FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM sm{};
    if (FAILED(pdo->GetData(&fe, &sm))) return false;

    bool any = false;
    if (HDROP hDrop = (HDROP)GlobalLock(sm.hGlobal))
    {
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i)
        {
            UINT need = DragQueryFileW(hDrop, i, nullptr, 0);
            if (!need) continue;
            std::wstring p(need, L'\0');
            if (DragQueryFileW(hDrop, i, p.data(), need + 1))
            {
                paths.push_back(p);
                any = true;
            }
        }
        GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
    return any;
}

static bool PathsFromIdList(IDataObject* pdo, std::vector<std::wstring>& paths)
{
    static const CLIPFORMAT cfIdList =
        (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_SHELLIDLIST);

    FORMATETC fe{ cfIdList, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM sm{};
    if (FAILED(pdo->GetData(&fe, &sm))) return false;

    bool any = false;
    if (auto* cida = (CIDA*)GlobalLock(sm.hGlobal))
    {
        auto pidlAt = [&](UINT i) -> LPCITEMIDLIST {
            return (LPCITEMIDLIST)((BYTE*)cida + cida->aoffset[i]);
        };
        LPCITEMIDLIST parent = pidlAt(0);
        for (UINT i = 1; i <= cida->cidl; ++i)
        {
            if (LPITEMIDLIST full = ILCombine(parent, pidlAt(i)))
            {
                wchar_t buf[MAX_PATH * 2] = {};
                if (SHGetPathFromIDListW(full, buf) && buf[0])
                {
                    paths.push_back(buf);
                    any = true;
                }
                ILFree(full);
            }
        }
        GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
    return any;
}

bool PathsFromDataObject(IDataObject* pdo, std::vector<std::wstring>& paths)
{
    if (!pdo) return false;
    if (PathsFromHDrop(pdo, paths)) return true;
    return PathsFromIdList(pdo, paths);   // e.g. dragged out of another NSE
}

// ─────────────────────────────────────────────────────────
// Expand dropped roots into individual files
// ─────────────────────────────────────────────────────────
static void WalkDir(const std::wstring& dir, const std::wstring& relBase,
                    std::vector<AddItem>& out)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        std::wstring child = dir + L"\\" + fd.cFileName;
        std::wstring rel   = relBase.empty() ? std::wstring(fd.cFileName)
                                             : relBase + L"\\" + fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            WalkDir(child, rel, out);
        else
            out.push_back({ child, rel });
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

void ExpandForAdd(const std::vector<std::wstring>& roots,
                  std::vector<AddItem>& out)
{
    for (const auto& root : roots)
    {
        DWORD attr = GetFileAttributesW(root.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;

        const wchar_t* leaf = PathFindFileNameW(root.c_str());
        if (attr & FILE_ATTRIBUTE_DIRECTORY)
            WalkDir(root, leaf, out);
        else
            out.push_back({ root, leaf });
    }
}

std::wstring TargetDirFor(const std::wstring& baseDir, const AddItem& item)
{
    std::wstring dir = baseDir;
    if (!dir.empty() && dir.back() != L'/') dir += L'/';

    size_t slash = item.rel.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
    {
        std::wstring sub = item.rel.substr(0, slash + 1);
        for (auto& ch : sub) if (ch == L'\\') ch = L'/';
        dir += sub;
    }
    return dir;
}

bool ClipboardHasFiles()
{
    static const UINT cfIdList  = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
    static const UINT cfDescrW  = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    return IsClipboardFormatAvailable(CF_HDROP) ||
           IsClipboardFormatAvailable(cfIdList) ||
           IsClipboardFormatAvailable(cfDescrW);
}

} // namespace ArchiveOps

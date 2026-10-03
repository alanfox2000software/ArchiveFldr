// ArchiveOps.cpp — see ArchiveOps.h
#include "stdafx.h"
#include "ArchiveOps.h"
#include "ArchiveWriter.h"
#include "PasswordDialog.h"
#include "Settings.h"

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
    // The configured folder wins, when there is one and it is usable;
    // otherwise the system temp folder, as before. A custom path that
    // cannot be created is not worth failing an extraction over.
    std::wstring root;
    {
        const Settings& cfg = Settings::Get();
        if (cfg.useTempDir && !cfg.tempDirPath.empty())
        {
            if (GetFileAttributesW(cfg.tempDirPath.c_str()) != INVALID_FILE_ATTRIBUTES ||
                SHCreateDirectoryExW(nullptr, cfg.tempDirPath.c_str(), nullptr) == ERROR_SUCCESS)
            {
                root = cfg.tempDirPath;
                if (!root.empty() && root.back() != L'\\') root += L'\\';
            }
        }
    }

    wchar_t tmp[MAX_PATH] = {};
    if (root.empty())
    {
        if (!GetTempPathW(MAX_PATH, tmp)) return L"";
    }
    else
    {
        wcsncpy_s(tmp, root.c_str(), _TRUNCATE);
    }

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

        std::wstring dir = std::wstring(tmp) + L"ArchiveFldr\\" + clean + L"-" + suffix;
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
    MessageBoxW(hwnd, msg.c_str(), L"ArchiveFldr", MB_ICONWARNING | MB_OK);
}

bool EnsureCanRead(HWND hwnd, const EnginePtr& eng)
{
    if (!eng)
    {
        Explain(hwnd, L"This archive is not open.", L"");
        return false;
    }

    EngineCaps caps = eng->GetCaps();
    if (!caps.canExtract)
    {
        Explain(hwnd, L"ArchiveFldr cannot read the contents of this archive.",
                caps.unavailableReason);
        return false;
    }
    if (!eng->IsOpen())
    {
        Explain(hwnd, L"This archive is not open.",
                eng->PasswordNeededToOpen()
                    ? L"A password is required before its contents can be read."
                    : L"The archive may be damaged, incomplete, or unsupported.");
        return false;
    }
    return true;
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
                L"The " + caps.engineName + L" engine ArchiveFldr uses is "
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
            // DragQueryFile reports the length WITHOUT the terminator
            // but writes one, so the buffer has to be a character
            // longer than the answer. Sizing it to `need` and then
            // promising `need + 1` was writing the NUL onto the one
            // element of a std::wstring that is not ours to write.
            UINT need = DragQueryFileW(hDrop, i, nullptr, 0);
            if (!need) continue;
            std::wstring p(need + 1, L'\0');
            if (DragQueryFileW(hDrop, i, p.data(), need + 1))
            {
                p.resize(wcslen(p.c_str()));
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

std::wstring FormatRatio(uint64_t uncompressed, uint64_t packed)
{
    if (uncompressed == 0) return L"";        // folders, empty files
    if (packed == 0)       return L"\u2014";  // not reported for this item

    // Packed size as a percentage of the original, which is what every
    // other archiver means by "ratio": 7-Zip and WinRAR both show 85% for
    // a file that still takes 85% of its original room. This used to
    // report the space saved instead, so that same file was labelled
    // "15%" — a figure that reads like excellent compression and meant
    // very nearly the opposite.
    double r = 100.0 * (double)packed / (double)uncompressed;
    if (r < 0.0 || r > 9999.0) return L"\u2014";   // not a usable figure

    wchar_t buf[32];
    swprintf_s(buf, 32, L"%.0f%%", r);
    return buf;
}

std::wstring FormatSizeKB(uint64_t bytes)
{
    // Explorer rounds up, so a 1-byte file is "1 KB" rather than "0 KB".
    const unsigned long long kb = (bytes + 1023ULL) / 1024ULL;

    wchar_t raw[32];
    swprintf_s(raw, 32, L"%llu", kb);

    // Locale separators, read the XP-compatible way (GetLocaleInfoEx is
    // Vista+). Defaults cover the case where the query fails.
    wchar_t thousand[8] = L",";
    wchar_t decimal[8]  = L".";
    GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_STHOUSAND, thousand, 8);
    GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SDECIMAL,  decimal,  8);

    NUMBERFMTW nf{};
    nf.NumDigits     = 0;          // whole kilobytes, no decimals
    nf.LeadingZero   = 0;
    nf.Grouping      = 3;
    nf.lpDecimalSep  = decimal;
    nf.lpThousandSep = thousand;
    nf.NegativeOrder = 1;

    wchar_t out[48];
    if (GetNumberFormatW(LOCALE_USER_DEFAULT, 0, raw, &nf, out, 48) > 0)
        return std::wstring(out) + L" KB";
    return std::wstring(raw) + L" KB";
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

// ─────────────────────────────────────────────────────────
// Passwords
// ─────────────────────────────────────────────────────────
static std::wstring ArchiveLeafName(const EnginePtr& eng)
{
    const std::wstring path = eng ? eng->GetFilePath() : L"";
    const wchar_t* leaf = PathFindFileNameW(path.c_str());
    return leaf ? leaf : L"";
}

bool EnsureOpenPassword(HWND hwnd, const EnginePtr& eng)
{
    if (!eng) return false;
    if (eng->IsOpen()) return true;
    if (!eng->PasswordNeededToOpen()) return false;

    const std::wstring path = eng->GetFilePath();
    if (path.empty()) return false;

    // Another Explorer shell object may already have verified and cached the
    // password for this archive. Let Open() consume that in-process cache
    // before showing a duplicate prompt in this object.
    if (eng->GetPassword().empty() && eng->Open(path)) return true;

    for (int attempt = 0; eng->PasswordNeededToOpen() && attempt < 3;
         ++attempt)
    {
        std::wstring pw;
        if (!PasswordDialog::Ask(hwnd, ArchiveLeafName(eng),
                attempt == 0
                    ? L"This archive is encrypted.\nIts contents cannot "
                      L"be shown without the password."
                    : L"That password is not correct.\nEnter the password "
                      L"to try again.",
                pw))
            return false;

        eng->SetPassword(pw);
        if (eng->Open(path)) return true;
    }
    return false;
}

bool EnsureReadPassword(HWND hwnd, const EnginePtr& eng)
{
    if (!eng) return false;
    const bool decoderAsked = eng->LastErrorNeedsPassword();
    if ((!eng->HasEncryptedItems() && !decoderAsked) ||
        !eng->GetPassword().empty())
        return true;                        // nothing to ask about

    std::wstring pw;
    if (!PasswordDialog::Ask(hwnd, ArchiveLeafName(eng),
            L"Items in this archive are encrypted.\n"
            L"Enter the password to continue.", pw))
        return false;
    eng->SetPassword(pw);
    return true;
}

bool AskPasswordAgain(HWND hwnd, const EnginePtr& eng)
{
    if (!eng) return false;
    std::wstring pw;
    if (!PasswordDialog::Ask(hwnd, ArchiveLeafName(eng),
            L"That password is not correct.\n"
            L"Enter the password to try again.", pw))
        return false;
    eng->SetPassword(pw);
    return true;
}

bool ExtractEntryPrompting(HWND hwnd, const EnginePtr& eng,
                           const ArchiveEntry& e,
                           const std::wstring& destDir,
                           std::wstring* produced)
{
    // Only encrypted entries are worth a prompt up front; everything
    // else extracts or fails on its own merits.
    if (e.isEncrypted && !EnsureReadPassword(hwnd, eng))
        return false;

    int passwordAttempts = 0;
    for (;;)
    {
        const bool hadPassword = eng && !eng->GetPassword().empty();
        if (hadPassword) ++passwordAttempts;

        if (ExtractEntry(eng, e, destDir, produced)) return true;
        if (!eng || !eng->LastErrorWasWrongPassword()) return false;
        if (hadPassword && passwordAttempts >= 3) return false;

        const bool supplied = eng->LastErrorNeedsPassword()
            ? EnsureReadPassword(hwnd, eng)
            : AskPasswordAgain(hwnd, eng);
        if (!supplied) return false;
    }
}

// ─────────────────────────────────────────────────────────
// Writer items
// ─────────────────────────────────────────────────────────
void BuildWriterItems(const std::vector<AddItem>& in,
                      const std::wstring& baseDir,
                      std::vector<ArchiveWriter::Item>& out)
{
    for (const auto& ai : in)
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(ai.src.c_str(), GetFileExInfoStandard, &fad))
            continue;                       // vanished between expand and now

        ArchiveWriter::Item it;
        it.diskPath = ai.src;

        // Stored name: the target directory inside the archive plus the
        // file's own name, '/'-separated like every internal path here.
        std::wstring name = TargetDirFor(baseDir, ai);
        name += PathFindFileNameW(ai.src.c_str());
        it.nameInArchive = name;

        it.isDir  = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        it.attrib = fad.dwFileAttributes;
        it.mtime  = fad.ftLastWriteTime;
        it.size   = it.isDir ? 0
                  : ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        out.push_back(std::move(it));
    }
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

// WimEngine.cpp — see WimEngine.h
#include "stdafx.h"
#include "WimEngine.h"
#include "ThirdParty.h"
#include "Formats.h"

#include <mutex>

// ═════════════════════════════════════════════════════════════════════════
// wimlib ABI
//
// Transcribed from wimlib.h (v1.14.5). wimlib hands us a pointer to a
// library-allocated struct and we read fields by offset, so a wrong layout
// means reading garbage — inside explorer.exe. Three things decide the
// offsets and all three are pinned below with static_asserts:
//
//   * wimlib_tchar is wchar_t on Windows, and paths use BACKSLASH.
//   * WIMLIBAPI is empty for library consumers, so everything is __cdecl.
//   * struct wimlib_timespec holds a 64-bit tv_sec only on _WIN64; on
//     32-bit Windows it is 32-bit, with the high halves carried separately
//     in the creation_time_high / last_write_time_high fields. That single
//     difference shifts every field after it by 12 bytes between the two
//     builds, which is exactly the kind of mistake that would corrupt
//     Explorer rather than fail cleanly.
//
// The struct also grew `object_id` in wimlib 1.9.1 and the `*_time_high`
// fields later still. An older libwim would therefore write a *different*
// layout into the same memory, so the version is checked at load time and
// anything below 1.13.0 is refused.
// ═════════════════════════════════════════════════════════════════════════
namespace {

constexpr size_t kWimlibGuidLen = 16;

struct wimlib_timespec
{
#ifdef _WIN64
    int64_t tv_sec;
#else
    int32_t tv_sec;
#endif
    int32_t tv_nsec;
};

struct wimlib_object_id
{
    uint8_t object_id[kWimlibGuidLen];
    uint8_t birth_volume_id[kWimlibGuidLen];
    uint8_t birth_object_id[kWimlibGuidLen];
    uint8_t domain_id[kWimlibGuidLen];
};

struct wimlib_resource_entry
{
    uint64_t uncompressed_size;
    uint64_t compressed_size;
    uint64_t offset;
    uint8_t  sha1_hash[20];
    uint32_t part_number;
    uint32_t reference_count;
    uint32_t flags;                    // six 1-bit fields + 26 reserved
    uint64_t raw_resource_offset_in_wim;
    uint64_t raw_resource_compressed_size;
    uint64_t raw_resource_uncompressed_size;
    uint64_t reserved[1];
};

struct wimlib_stream_entry
{
    const wchar_t*        stream_name;
    wimlib_resource_entry resource;
    uint64_t              reserved[4];
};

struct wimlib_dir_entry
{
    const wchar_t* filename;
    const wchar_t* dos_name;
    const wchar_t* full_path;
    size_t         depth;
    const char*    security_descriptor;
    size_t         security_descriptor_size;
    uint32_t       attributes;
    uint32_t       reparse_tag;
    uint32_t       num_links;
    uint32_t       num_named_streams;
    uint64_t       hard_link_group_id;
    wimlib_timespec creation_time;
    wimlib_timespec last_write_time;
    wimlib_timespec last_access_time;
    uint32_t       unix_uid;
    uint32_t       unix_gid;
    uint32_t       unix_mode;
    uint32_t       unix_rdev;
    wimlib_object_id object_id;
    int32_t        creation_time_high;
    int32_t        last_write_time_high;
    int32_t        last_access_time_high;
    int32_t        reserved2;
    uint64_t       reserved[4];
    wimlib_stream_entry streams[1];    // really a flexible array member
};

static_assert(sizeof(wchar_t) == 2, "wimlib_tchar is UTF-16 on Windows");
static_assert(sizeof(wimlib_timespec) == (sizeof(void*) == 8 ? 16 : 8),
              "wimlib ABI: timespec width");
static_assert(sizeof(wimlib_resource_entry) == 88, "wimlib ABI: resource_entry");
static_assert(sizeof(wimlib_stream_entry)   == 128, "wimlib ABI: stream_entry");
static_assert(offsetof(wimlib_dir_entry, attributes) ==
              (sizeof(void*) == 8 ? 48 : 24), "wimlib ABI: attributes");
static_assert(offsetof(wimlib_dir_entry, hard_link_group_id) ==
              (sizeof(void*) == 8 ? 64 : 40), "wimlib ABI: hard_link_group_id");
static_assert(offsetof(wimlib_dir_entry, last_write_time) ==
              (sizeof(void*) == 8 ? 88 : 56), "wimlib ABI: last_write_time");
static_assert(offsetof(wimlib_dir_entry, object_id) ==
              (sizeof(void*) == 8 ? 136 : 88), "wimlib ABI: object_id");
static_assert(offsetof(wimlib_dir_entry, streams) ==
              (sizeof(void*) == 8 ? 248 : 200), "wimlib ABI: streams");

// ── Flags and constants ─────────────────────────────────────────────────
constexpr int kIterateRecursive          = 0x00000001;
constexpr int kExtractNoPreserveDirStruct = 0x00200000;
constexpr int kAllImages                 = -1;

constexpr uint32_t kAttrDirectory    = 0x00000010;
constexpr uint32_t kAttrReparsePoint = 0x00000400;
constexpr uint32_t kAttrEncrypted    = 0x00004000;

typedef int (*PfnIterateCb)(const wimlib_dir_entry* dentry, void* user_ctx);

struct WimApi
{
    int  (__cdecl* open_wim)(const wchar_t* path, int flags, void** wim_ret);
    void (__cdecl* free_wim)(void* wim);
    int  (__cdecl* iterate_dir_tree)(void* wim, int image, const wchar_t* path,
                                     int flags, PfnIterateCb cb, void* ctx);
    int  (__cdecl* extract_paths)(void* wim, int image, const wchar_t* target,
                                  const wchar_t* const* paths, size_t numPaths,
                                  int extractFlags);
    int  (__cdecl* get_wim_info)(void* wim, void* info);
    int  (__cdecl* verify_wim)(void* wim, int flags);
    const wchar_t* (__cdecl* get_image_name)(void* wim, int image);
    const wchar_t* (__cdecl* get_error_string)(int code);
    int  (__cdecl* global_init)(int flags);
    uint32_t (__cdecl* get_version)();
    int  (__cdecl* reference_resource_files)(void* wim,
                                             const wchar_t* const* globs,
                                             unsigned count,
                                             int refFlags, int openFlags);
};

// Only the head of struct wimlib_wim_info is read, and every field used
// sits before any platform-dependent member, so a fixed buffer is safe.
struct WimInfoHead
{
    uint8_t  guid[kWimlibGuidLen];
    uint32_t image_count;
    uint32_t boot_index;
    uint32_t wim_version;
    uint32_t chunk_size;
    uint16_t part_number;
    uint16_t total_parts;
    int32_t  compression_type;
    uint64_t total_bytes;
    uint32_t flags;
    uint32_t reserved[9];
};

struct WimLib
{
    bool         tried  = false;
    HMODULE      module = nullptr;
    std::wstring path;
    std::wstring error;
    WimApi       api{};
    uint32_t     version = 0;
};

std::mutex g_mutex;
WimLib     g_lib;

WimLib& Lib()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_lib.tried) return g_lib;
    g_lib.tried = true;

    g_lib.module = ThirdParty::LoadComponent(L"WimLib", &g_lib.path);
    if (!g_lib.module)
    {
        g_lib.error = g_lib.path.empty()
            ? L"libwim was not found."
            : L"libwim was found but Windows refused to load it (wrong "
              L"architecture, or a dependency is missing beside it).";
        return g_lib;
    }

    auto get = [](const char* n) { return GetProcAddress(g_lib.module, n); };
    #define BIND(field, name) \
        g_lib.api.field = reinterpret_cast<decltype(g_lib.api.field)>(get(name))

    BIND(open_wim,                 "wimlib_open_wim");
    BIND(free_wim,                 "wimlib_free");
    BIND(iterate_dir_tree,         "wimlib_iterate_dir_tree");
    BIND(extract_paths,            "wimlib_extract_paths");
    BIND(get_wim_info,             "wimlib_get_wim_info");
    BIND(verify_wim,               "wimlib_verify_wim");
    BIND(get_image_name,           "wimlib_get_image_name");
    BIND(get_error_string,         "wimlib_get_error_string");
    BIND(global_init,              "wimlib_global_init");
    BIND(get_version,              "wimlib_get_version");
    BIND(reference_resource_files, "wimlib_reference_resource_files");
    #undef BIND

    if (!g_lib.api.open_wim || !g_lib.api.free_wim ||
        !g_lib.api.iterate_dir_tree || !g_lib.api.extract_paths)
    {
        g_lib.error = L"That DLL does not export the wimlib API "
                      L"(wimlib_open_wim / wimlib_iterate_dir_tree). It may "
                      L"not be libwim at all.";
        g_lib.module = nullptr;
        return g_lib;
    }

    // Version gate. struct wimlib_dir_entry gained object_id in 1.9.1 and
    // the *_time_high fields afterwards; against an older libwim the
    // layout compiled in here would not match what the DLL writes, and we
    // would read nonsense. Refuse rather than risk it.
    if (!g_lib.api.get_version)
    {
        g_lib.error = L"That libwim is too old: it does not export "
                      L"wimlib_get_version, so its ABI cannot be verified. "
                      L"wimlib 1.13.0 or newer is required.";
        g_lib.module = nullptr;
        return g_lib;
    }

    g_lib.version = g_lib.api.get_version();
    const uint32_t major = (g_lib.version >> 20) & 0x3FF;
    const uint32_t minor = (g_lib.version >> 10) & 0x3FF;
    if (major < 1 || (major == 1 && minor < 13))
    {
        wchar_t buf[192];
        swprintf_s(buf, 192,
            L"That libwim reports version %u.%u, but ArchiveFldr needs 1.13.0 "
            L"or newer — older builds lay out their directory entries "
            L"differently. Use the libwim-15 build.",
            major, minor);
        g_lib.error = buf;
        g_lib.module = nullptr;
        return g_lib;
    }

    if (g_lib.api.global_init) g_lib.api.global_init(0);
    return g_lib;
}

std::wstring ErrText(int code)
{
    WimLib& lib = g_lib;
    if (lib.api.get_error_string)
        if (const wchar_t* s = lib.api.get_error_string(code)) return s;
    wchar_t buf[64];
    swprintf_s(buf, 64, L"wimlib error %d", code);
    return buf;
}

// wimlib reports UNIX time; on 32-bit builds the high half of the seconds
// arrives in a separate field.
FILETIME ToFileTime(const wimlib_timespec& ts, int32_t high)
{
    int64_t secs;
    if (sizeof(ts.tv_sec) == sizeof(int64_t))
        secs = (int64_t)ts.tv_sec;
    else
        secs = ((int64_t)high << 32) | (uint32_t)ts.tv_sec;

    const int64_t ticks = (secs + 11644473600LL) * 10000000LL +
                          (ts.tv_nsec / 100);
    FILETIME ft{};
    if (ticks > 0)
    {
        ft.dwLowDateTime  = (DWORD)(ticks & 0xFFFFFFFF);
        ft.dwHighDateTime = (DWORD)(ticks >> 32);
    }
    return ft;
}

// WIM paths use backslash; the rest of ArchiveFldr uses forward slash.
std::wstring ToInternal(const wchar_t* wimPath)
{
    std::wstring s = wimPath ? wimPath : L"";
    for (auto& c : s) if (c == L'\\') c = L'/';
    while (!s.empty() && s.front() == L'/') s.erase(s.begin());
    return s;
}

struct IterCtx
{
    std::vector<ArchiveEntry>* out;
    std::wstring               prefix;     // "" or "Image 2/"
    int                        image;
    uint64_t                   bytes;
};

int IterateCallback(const wimlib_dir_entry* d, void* userCtx)
{
    auto* ctx = static_cast<IterCtx*>(userCtx);
    if (!d || !ctx) return 0;

    // The image root arrives with an empty path; it is not an entry.
    std::wstring rel = ToInternal(d->full_path);
    if (rel.empty()) return 0;

    const bool isDir = (d->attributes & kAttrDirectory) != 0;

    ArchiveEntry e;
    e.fullPath    = ctx->prefix + rel + (isDir ? L"/" : L"");
    e.isDirectory = isDir;
    e.name        = d->filename ? d->filename : L"";
    if (e.name.empty())
    {
        size_t slash = rel.find_last_of(L'/');
        e.name = (slash == std::wstring::npos) ? rel : rel.substr(slash + 1);
    }

    if (!isDir)
    {
        // streams[0] is always present and is the unnamed data stream.
        e.uncompressedSize = d->streams[0].resource.uncompressed_size;
        // wimlib fills in compressed_size for blobs held in an ordinary
        // (non-solid) resource. For solid resources — which is what an
        // .esd uses — it stays 0, and the view shows that as "unknown"
        // rather than inventing a figure.
        e.compressedSize   = d->streams[0].resource.compressed_size;

        // WIM identifies blobs by SHA-1, not CRC-32. Leaving hasCrc false
        // keeps the CRC column honest; the digest itself is worth showing
        // in Properties, where there is room for it.
        const uint8_t* h = d->streams[0].resource.sha1_hash;
        bool anySet = false;
        for (int b = 0; b < 20 && !anySet; ++b) anySet = (h[b] != 0);
        if (anySet)
        {
            wchar_t hex[41];
            for (int b = 0; b < 20; ++b)
                swprintf_s(hex + b * 2, 3, L"%02x", h[b]);
            e.sha1.assign(hex, 40);
        }
        ctx->bytes        += e.uncompressedSize;
    }

    e.modifiedTime = ToFileTime(d->last_write_time, d->last_write_time_high);
    e.isEncrypted  = (d->attributes & kAttrEncrypted) != 0;
    e.compressionMethod = (d->attributes & kAttrReparsePoint)
                        ? L"reparse point" : L"WIM";
    e.engineIndex  = ctx->image;

    ctx->out->push_back(std::move(e));
    return 0;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════
// Public helpers
// ═════════════════════════════════════════════════════════════════════════
bool IsWimLibAvailable()     { return Lib().module != nullptr; }
std::wstring GetWimLibPath() { return Lib().path; }

// ═════════════════════════════════════════════════════════════════════════
// CWimEngine
// ═════════════════════════════════════════════════════════════════════════
CWimEngine::CWimEngine()  { m_readOnly = true; }
CWimEngine::~CWimEngine() { Close(); }

bool CWimEngine::Create(const std::wstring&) { return false; }

void CWimEngine::Close()
{
    if (m_wim && Lib().api.free_wim) Lib().api.free_wim(m_wim);
    m_wim = nullptr;
    m_open = false;
    m_allEntries.clear();
    m_comment.clear();
    m_imageCount = 0;
    m_fileBytes  = 0;
}

bool CWimEngine::Open(const std::wstring& path)
{
    Close();
    m_filePath = path;

    WimLib& lib = Lib();
    if (!lib.module) return false;

    LPCWSTR ext  = PathFindExtensionW(path.c_str());
    std::wstring pretty = Formats::NameFor(ext);
    if (!pretty.empty()) m_formatName = pretty;

    void* wim = nullptr;
    int rc = lib.api.open_wim(path.c_str(), 0, &wim);
    if (rc != 0 || !wim)
    {
        m_lastError = L"wimlib could not open this file: " + ErrText(rc);
        return false;
    }
    m_wim = wim;

    WimInfoHead info{};
    if (lib.api.get_wim_info && lib.api.get_wim_info(m_wim, &info) == 0)
    {
        m_imageCount = (int)info.image_count;
        if (info.total_parts > 1) ReferenceSplitParts();
    }
    if (m_imageCount <= 0) m_imageCount = 1;

    if (!BuildEntryList()) { Close(); return false; }

    m_open = true;
    return true;
}

// A split WIM keeps its file data in sibling parts. Rather than guess at
// wimlib's glob flag, enumerate the siblings and hand over explicit paths.
void CWimEngine::ReferenceSplitParts()
{
    WimLib& lib = Lib();
    if (!lib.api.reference_resource_files) return;

    wchar_t dir[MAX_PATH * 2];
    wcsncpy_s(dir, m_filePath.c_str(), _TRUNCATE);
    PathRemoveFileSpecW(dir);

    // "install2.swm" -> stem "install"; match install*.swm beside it.
    std::wstring stem = PathFindFileNameW(m_filePath.c_str());
    size_t dot = stem.find_last_of(L'.');
    std::wstring suffix = (dot == std::wstring::npos) ? L".swm"
                                                      : stem.substr(dot);
    if (dot != std::wstring::npos) stem.erase(dot);
    while (!stem.empty() && iswdigit(stem.back())) stem.pop_back();

    std::wstring pattern = std::wstring(dir) + L"\\" + stem + L"*" + suffix;

    std::vector<std::wstring>  parts;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring full = std::wstring(dir) + L"\\" + fd.cFileName;
            if (_wcsicmp(full.c_str(), m_filePath.c_str()) == 0) continue;
            parts.push_back(std::move(full));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (parts.empty()) return;

    std::vector<const wchar_t*> ptrs;
    ptrs.reserve(parts.size());
    for (const auto& p : parts) ptrs.push_back(p.c_str());

    lib.api.reference_resource_files(m_wim, ptrs.data(),
                                     (unsigned)ptrs.size(), 0, 0);
}

bool CWimEngine::BuildEntryList()
{
    WimLib& lib = Lib();
    m_allEntries.clear();
    m_fileBytes = 0;

    const bool many = m_imageCount > 1;

    for (int image = 1; image <= m_imageCount; ++image)
    {
        IterCtx ctx;
        ctx.out   = &m_allEntries;
        ctx.image = image;
        ctx.bytes = 0;

        if (many)
        {
            // Name the folder after the image, falling back to its index.
            std::wstring label;
            if (lib.api.get_image_name)
                if (const wchar_t* n = lib.api.get_image_name(m_wim, image))
                    label = n;

            wchar_t idx[16];
            swprintf_s(idx, 16, L"%d", image);
            std::wstring folder = label.empty()
                ? (std::wstring(L"Image ") + idx)
                : (std::wstring(idx) + L" - " + label);

            // Path separators in an image name would break the tree.
            for (auto& c : folder)
                if (c == L'/' || c == L'\\' || c == L':') c = L'_';

            ArchiveEntry d;
            d.isDirectory = true;
            d.name        = folder;
            d.fullPath    = folder + L"/";
            d.engineIndex = image;
            m_allEntries.push_back(std::move(d));

            ctx.prefix = folder + L"/";
        }

        int rc = lib.api.iterate_dir_tree(m_wim, image, L"\\",
                                          kIterateRecursive,
                                          IterateCallback, &ctx);
        if (rc != 0 && m_allEntries.empty())
        {
            m_lastError = L"wimlib could not read the image: " + ErrText(rc);
            return false;
        }
        m_fileBytes += ctx.bytes;
    }
    return true;
}

bool CWimEngine::SplitImagePath(const std::wstring& full,
                                int* image, std::wstring* wimPath) const
{
    std::wstring rest = full;
    *image = 1;

    if (m_imageCount > 1)
    {
        size_t slash = full.find(L'/');
        std::wstring folder = (slash == std::wstring::npos)
                            ? full : full.substr(0, slash);
        // Map the folder back to its image by looking it up in the list.
        bool found = false;
        for (const auto& e : m_allEntries)
        {
            if (e.isDirectory && e.fullPath == folder + L"/")
            {
                *image = (int)e.engineIndex;
                found  = true;
                break;
            }
        }
        if (!found) return false;
        rest = (slash == std::wstring::npos) ? L"" : full.substr(slash + 1);
    }

    while (!rest.empty() && rest.back() == L'/') rest.pop_back();
    for (auto& c : rest) if (c == L'/') c = L'\\';

    *wimPath = L"\\" + rest;
    return true;
}

std::vector<ArchiveEntry> CWimEngine::List(const std::wstring& dirPath)
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

        std::wstring rest = e.fullPath.substr(prefix.size());
        if (e.isDirectory && !rest.empty() && rest.back() == L'/')
            rest.pop_back();
        if (rest.find(L'/') != std::wstring::npos) continue;

        result.push_back(e);
    }
    return result;
}

bool CWimEngine::ExtractFile(const ArchiveEntry& e,
                             const std::wstring& destDir, ProgressFn cb)
{
    if (!m_open) return false;
    WimLib& lib = Lib();

    int          image = 1;
    std::wstring wimPath;
    if (!SplitImagePath(e.fullPath, &image, &wimPath)) return false;

    // An image folder itself has no WIM-side path; extract the whole image.
    if (wimPath == L"\\" && m_imageCount > 1)
    {
        SHCreateDirectoryExW(nullptr, destDir.c_str(), nullptr);
        const wchar_t* root = L"\\";
        int rc = lib.api.extract_paths(m_wim, image, destDir.c_str(),
                                       &root, 1, 0);
        if (rc != 0)
        {
            MessageBoxW(nullptr,
                (L"wimlib could not extract the image.\n\n" +
                 ErrText(rc)).c_str(), L"ArchiveFldr", MB_ICONWARNING | MB_OK);
            return false;
        }
        if (cb) cb(100, e.name);
        return true;
    }

    SHCreateDirectoryExW(nullptr, destDir.c_str(), nullptr);
    if (cb) cb(0, e.name);

    const wchar_t* p = wimPath.c_str();
    int rc = lib.api.extract_paths(m_wim, image, destDir.c_str(), &p, 1,
                                   kExtractNoPreserveDirStruct);
    if (rc != 0)
    {
        MessageBoxW(nullptr,
            (L"wimlib could not extract \"" + e.name + L"\".\n\n" +
             ErrText(rc) +
             (m_imageCount > 0 && PathMatchSpecW(m_filePath.c_str(), L"*.swm")
                ? L"\n\nFor a split WIM, keep every .swm part in the same "
                  L"folder."
                : L"")).c_str(),
            L"ArchiveFldr", MB_ICONWARNING | MB_OK);
        return false;
    }
    if (cb) cb(100, e.name);
    return true;
}

bool CWimEngine::ExtractAll(const std::wstring& destDir, ProgressFn cb)
{
    if (!m_open) return false;
    WimLib& lib = Lib();

    SHCreateDirectoryExW(nullptr, destDir.c_str(), nullptr);

    bool allOk = true;
    for (int image = 1; image <= m_imageCount; ++image)
    {
        // Multiple images would collide in one folder, so give each its own.
        std::wstring target = destDir;
        if (m_imageCount > 1)
        {
            for (const auto& e : m_allEntries)
                if (e.isDirectory && (int)e.engineIndex == image &&
                    e.fullPath.find(L'/') == e.fullPath.size() - 1)
                {
                    target = destDir;
                    if (!target.empty() && target.back() != L'\\') target += L'\\';
                    target += e.name;
                    break;
                }
            SHCreateDirectoryExW(nullptr, target.c_str(), nullptr);
        }

        const wchar_t* root = L"\\";
        int rc = lib.api.extract_paths(m_wim, image, target.c_str(),
                                       &root, 1, 0);
        if (rc != 0)
        {
            allOk = false;
            MessageBoxW(nullptr,
                (L"wimlib could not extract image " + std::to_wstring(image) +
                 L".\n\n" + ErrText(rc)).c_str(),
                L"ArchiveFldr", MB_ICONWARNING | MB_OK);
            break;
        }
        if (cb) cb(image * 100 / (m_imageCount ? m_imageCount : 1), L"");
    }
    return allOk;
}

bool CWimEngine::Test(ProgressFn cb)
{
    if (!m_open) return false;
    WimLib& lib = Lib();
    if (!lib.api.verify_wim)
    {
        // Older libwim without verify_wim: listing succeeded, which already
        // proves the metadata parses. Say so rather than claiming a pass.
        MessageBoxW(nullptr,
            L"This build of libwim cannot verify file data.\n\n"
            L"The image metadata was read successfully, but the contents "
            L"were not checksummed.",
            L"ArchiveFldr", MB_ICONINFORMATION | MB_OK);
        return true;
    }
    if (cb) cb(0, m_filePath);
    int rc = lib.api.verify_wim(m_wim, 0);
    if (cb) cb(100, m_filePath);
    if (rc != 0)
    {
        MessageBoxW(nullptr,
            (L"Verification failed.\n\n" + ErrText(rc)).c_str(),
            L"ArchiveFldr", MB_ICONWARNING | MB_OK);
        return false;
    }
    return true;
}

EngineCaps CWimEngine::GetCaps() const
{
    EngineCaps caps;
    caps.engineName = m_formatName;

    WimLib& lib = Lib();
    caps.backendPath = lib.path;

    if (lib.module)
    {
        caps.isStub     = false;
        caps.canExtract = true;
        caps.canTest    = true;
        // wimlib can write WIMs, but capturing an image is a different
        // operation from "add a file to an archive" and is not offered.
        caps.canAdd = caps.canDelete = caps.canRename = false;
    }
    else
    {
        caps.isStub = true;
        caps.unavailableReason =
            (lib.error.empty() ? std::wstring() : lib.error + L"\n\n") +
            ThirdParty::DescribeSearch(L"WimLib");
    }
    return caps;
}

uint64_t CWimEngine::GetFileCount() const
{
    uint64_t n = 0;
    for (const auto& e : m_allEntries) if (!e.isDirectory) ++n;
    return n;
}

uint64_t CWimEngine::GetTotalSize() const { return m_fileBytes; }

uint64_t CWimEngine::GetPackedSize() const
{
    // The container's own size is the only honest "packed" figure: WIM
    // deduplicates across images, so summing per-file values would be
    // badly wrong for a multi-image file.
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(m_filePath.c_str(), GetFileExInfoStandard, &fad))
        return ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    return 0;
}

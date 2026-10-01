// CodecEngine.cpp — see CodecEngine.h
#include "stdafx.h"
#include "CodecEngine.h"
#include "Formats.h"
#include "ThirdParty.h"

#include <mutex>

// ═════════════════════════════════════════════════════════════════════════
// Codec bindings
//
// Each codec is a handful of C functions resolved by name. Nothing here is
// linked at build time and no third-party header is needed: the ABIs below
// are the published, stable C interfaces of each project.
//
// All five use the C calling convention (__cdecl on x86). Getting that
// wrong would corrupt the stack on 32-bit, so it is spelled out on every
// function pointer rather than left to a compiler default.
// ═════════════════════════════════════════════════════════════════════════
namespace {

constexpr size_t kChunk = 256 * 1024;

// ── zstd ────────────────────────────────────────────────────────────────
// Layouts straight from zstd.h; both are three machine words and have been
// stable since the 1.0 ABI freeze.
struct ZSTD_inBuffer  { const void* src; size_t size; size_t pos; };
struct ZSTD_outBuffer { void*       dst; size_t size; size_t pos; };

constexpr unsigned long long kZstdContentSizeUnknown = (unsigned long long)-1;
constexpr unsigned long long kZstdContentSizeError   = (unsigned long long)-2;

struct ZstdApi
{
    void*  (__cdecl* createDStream)();
    size_t (__cdecl* initDStream)(void*);
    size_t (__cdecl* decompressStream)(void*, ZSTD_outBuffer*, ZSTD_inBuffer*);
    size_t (__cdecl* freeDStream)(void*);
    unsigned (__cdecl* isError)(size_t);
    const char* (__cdecl* getErrorName)(size_t);
    unsigned long long (__cdecl* getFrameContentSize)(const void*, size_t);
};

// ── LZ4-family frame API (LZ4, LZ5 and Lizard are the same shape) ───────
// Lizard and LZ5 are forks of LZ4 and kept its frame interface verbatim,
// renaming only the prefix: LZ4F_ -> LZ5F_ -> LizardF_. One binding
// therefore drives all three.
struct Lz4FamilyApi
{
    size_t (__cdecl* createDecompressionContext)(void** , unsigned);
    size_t (__cdecl* freeDecompressionContext)(void*);
    size_t (__cdecl* decompress)(void*, void* dst, size_t* dstSize,
                                 const void* src, size_t* srcSize,
                                 const void* opts);
    unsigned (__cdecl* isError)(size_t);
    const char* (__cdecl* getErrorName)(size_t);
};

// ── brotli ──────────────────────────────────────────────────────────────
enum BrotliResult { kBrotliError = 0, kBrotliSuccess = 1,
                    kBrotliNeedsMoreInput = 2, kBrotliNeedsMoreOutput = 3 };

struct BrotliApi
{
    void* (__cdecl* createInstance)(void* alloc, void* free, void* opaque);
    void  (__cdecl* destroyInstance)(void*);
    int   (__cdecl* decompressStream)(void* state,
                                      size_t* availIn, const uint8_t** nextIn,
                                      size_t* availOut, uint8_t** nextOut,
                                      size_t* totalOut);
    int   (__cdecl* isFinished)(void*);
};

std::wstring Widen(const char* s);   // defined below, used by the binders

// ── Generic loader ──────────────────────────────────────────────────────
// Tries each candidate export name in turn. Projects occasionally ship a
// decorated or versioned alias, and a missing symbol must read as "this
// DLL is not the codec I need" rather than crash later.
FARPROC Pick(HMODULE h, std::initializer_list<const char*> names)
{
    for (const char* n : names)
        if (FARPROC p = GetProcAddress(h, n)) return p;
    return nullptr;
}

template <typename T>
bool Set(T& dst, HMODULE h, std::initializer_list<const char*> names)
{
    dst = reinterpret_cast<T>(Pick(h, names));
    return dst != nullptr;
}

// One resolved codec: the module plus whichever binding applies.
struct Codec
{
    bool         tried   = false;
    HMODULE      module  = nullptr;
    std::wstring path;
    std::wstring error;          // why it is unusable, for the UI

    enum class Kind { None, Zstd, Lz4Family, Brotli } kind = Kind::None;
    ZstdApi      zstd{};
    Lz4FamilyApi lz4{};
    BrotliApi    brotli{};
};

std::mutex g_codecMutex;
std::map<std::wstring, Codec> g_codecs;

void BindZstd(Codec& c)
{
    bool ok =
        Set(c.zstd.createDStream,       c.module, { "ZSTD_createDStream" }) &
        Set(c.zstd.initDStream,         c.module, { "ZSTD_initDStream" }) &
        Set(c.zstd.decompressStream,    c.module, { "ZSTD_decompressStream" }) &
        Set(c.zstd.freeDStream,         c.module, { "ZSTD_freeDStream" }) &
        Set(c.zstd.isError,             c.module, { "ZSTD_isError" });
    // Optional extras — absence costs us a nicer message or a known size.
    Set(c.zstd.getErrorName,        c.module, { "ZSTD_getErrorName" });
    Set(c.zstd.getFrameContentSize, c.module, { "ZSTD_getFrameContentSize",
                                                "ZSTD_getDecompressedSize" });
    if (!ok)
    {
        c.error = L"libzstd was loaded but does not export the "
                  L"streaming decompression API (ZSTD_decompressStream).";
        return;                     // leave kind None: nothing is callable
    }
    c.kind = Codec::Kind::Zstd;     // only now is the binding safe to call
}

// LZ5 and Lizard are the same project: the lz5 repository was renamed to
// lizard at v2.0, and the export prefix went LZ5F_ -> LizardF_ with it.
// A DLL shipped as liblz5.dll may therefore carry either, depending on
// which release it was built from, so try each prefix rather than assume.
// The frame layout is identical across all three (only the magic number
// differs: LZ4 0x184D2204, LZ5 ...2205, Lizard ...2206), so one binding
// drives them all.
void BindLz4Family(Codec& c, std::initializer_list<const char*> prefixes)
{
    std::wstring tried;
    for (const char* prefix : prefixes)
    {
        if (!tried.empty()) tried += L", ";
        tried += Widen(prefix);

        const std::string p(prefix);
        Lz4FamilyApi api{};
        bool ok = true;
        ok &= Set(api.createDecompressionContext, c.module,
                  { (p + "createDecompressionContext").c_str() });
        ok &= Set(api.freeDecompressionContext,   c.module,
                  { (p + "freeDecompressionContext").c_str() });
        ok &= Set(api.decompress,                 c.module,
                  { (p + "decompress").c_str() });
        ok &= Set(api.isError,                    c.module,
                  { (p + "isError").c_str() });
        if (!ok) continue;               // not this naming; try the next

        Set(api.getErrorName, c.module, { (p + "getErrorName").c_str() });
        c.lz4  = api;
        c.kind = Codec::Kind::Lz4Family; // every pointer below is now real
        return;
    }

    c.error = L"The DLL was loaded but exports none of the frame APIs this "
              L"codec is known by (tried the prefixes " + tried +
              L"). A raw-block build, or one built without the frame "
              L"layer, cannot read framed files.";
}

void BindBrotli(Codec& c)
{
    bool ok =
        Set(c.brotli.createInstance,   c.module, { "BrotliDecoderCreateInstance" }) &
        Set(c.brotli.destroyInstance,  c.module, { "BrotliDecoderDestroyInstance" }) &
        Set(c.brotli.decompressStream, c.module, { "BrotliDecoderDecompressStream" });
    Set(c.brotli.isFinished, c.module, { "BrotliDecoderIsFinished" });

    if (!ok)
    {
        c.error = L"libbrotlidec was loaded but does not export "
                  L"BrotliDecoderDecompressStream.";
        return;
    }
    c.kind = Codec::Kind::Brotli;
}

// Resolve + bind once per codec id, then cache (including the failure).
Codec& GetCodec(const std::wstring& id)
{
    std::lock_guard<std::mutex> lock(g_codecMutex);
    Codec& c = g_codecs[id];
    if (c.tried) return c;
    c.tried = true;

    c.module = ThirdParty::LoadComponent(id.c_str(), &c.path);
    if (!c.module)
    {
        c.error = c.path.empty()
            ? L"The codec DLL was not found."
            : L"The codec DLL was found but Windows refused to load it "
              L"(wrong architecture, or a missing dependency beside it).";
        return c;
    }

    if      (id == L"zstd")   BindZstd(c);
    else if (id == L"lz4")    BindLz4Family(c, { "LZ4F_" });
    else if (id == L"lz5")    BindLz4Family(c, { "LZ5F_", "LizardF_" });
    else if (id == L"lizard") BindLz4Family(c, { "LizardF_", "LZ5F_" });
    else if (id == L"brotli") BindBrotli(c);
    else                      c.error = L"Unknown codec id.";

    if (!c.error.empty()) c.kind = Codec::Kind::None;
    return c;
}

std::wstring Widen(const char* s)
{
    if (!s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

// ── Reading the original size out of the frame header ───────────────────
//
// Every one of these formats may record the uncompressed size in its
// frame header. Parsing those few bytes ourselves beats asking the codec
// DLL for two reasons: the size then shows even when the DLL is missing,
// and it does not depend on which helper functions a given build happens
// to export.
//
// Returns false when the format did not record a size — which is normal
// and must be reported as "unknown", never as zero.

bool ReadHead(const std::wstring& path, uint8_t* buf, DWORD want, DWORD* got)
{
    *got = 0;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BOOL ok = ReadFile(h, buf, want, got, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

uint64_t LoadLE(const uint8_t* p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// Zstandard frame header, per RFC 8878 section 3.1.1.1.
bool ZstdFrameSize(const uint8_t* b, DWORD n, uint64_t* out)
{
    if (n < 6) return false;
    if (LoadLE(b, 4) != 0xFD2FB528ULL) return false;      // not a zstd frame

    const uint8_t fhd        = b[4];
    const int  fcsFlag       = (fhd >> 6) & 3;
    const bool singleSegment = ((fhd >> 5) & 1) != 0;
    const int  dictIdFlag    = fhd & 3;

    // Frame_Content_Size is 0 bytes unless the flag says otherwise — but a
    // single-segment frame always carries at least one byte of it.
    static const int kFcsSize[4] = { 0, 2, 4, 8 };
    int fcsSize = kFcsSize[fcsFlag];
    if (fcsFlag == 0 && singleSegment) fcsSize = 1;
    if (fcsSize == 0) return false;                       // size not stored

    static const int kDictSize[4] = { 0, 1, 2, 4 };
    DWORD pos = 5;
    if (!singleSegment) pos += 1;                         // Window_Descriptor
    pos += kDictSize[dictIdFlag];

    if (pos + (DWORD)fcsSize > n) return false;
    uint64_t v = LoadLE(b + pos, fcsSize);
    if (fcsSize == 2) v += 256;                           // per the spec
    *out = v;
    return true;
}

// LZ4 frame header (RFC-documented). LZ5 and Lizard are forks that kept
// the same layout and only bumped the magic number.
bool Lz4FamilyFrameSize(const uint8_t* b, DWORD n, uint64_t* out)
{
    if (n < 15) return false;
    const uint64_t magic = LoadLE(b, 4);
    if (magic < 0x184D2204ULL || magic > 0x184D2208ULL) return false;

    const uint8_t flg = b[4];
    if (((flg >> 6) & 3) != 1) return false;              // version must be 01
    if (((flg >> 3) & 1) == 0) return false;              // no content size

    // Offset 6 = after the 4-byte magic, the FLG byte and the BD byte.
    // Verified against both lz5frame.c and lizard_frame.c, which build the
    // header identically. The field documents 0 as meaning "unknown", and
    // the encoders only set the flag when the size is non-zero, so a zero
    // here is a header we should not trust rather than an empty file.
    const uint64_t v = LoadLE(b + 6, 8);
    if (v == 0) return false;
    *out = v;
    return true;
}

bool ProbeOriginalSize(const std::wstring& codecId,
                       const std::wstring& path, uint64_t* out)
{
    uint8_t head[64] = {};
    DWORD   got = 0;
    if (!ReadHead(path, head, sizeof(head), &got) || got < 6) return false;

    if (codecId == L"zstd")
        return ZstdFrameSize(head, got, out);
    if (codecId == L"lz4" || codecId == L"lz5" || codecId == L"lizard")
        return Lz4FamilyFrameSize(head, got, out);

    // Brotli streams carry no uncompressed length at all.
    return false;
}

// ── Output sink: a real file, or nowhere (for Test) ─────────────────────
class Sink
{
public:
    // `limit` caps how many bytes we are willing to accept (0 = no cap).
    // Only the size-measuring pass sets it, so a deliberately crafted
    // stream cannot expand without bound while Explorer waits.
    explicit Sink(const std::wstring& path, uint64_t limit = 0)
        : m_discard(path.empty()), m_limit(limit)
    {
        if (m_discard) return;
        m_h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    ~Sink() { if (m_h != INVALID_HANDLE_VALUE) CloseHandle(m_h); }

    bool Ok() const { return m_discard || m_h != INVALID_HANDLE_VALUE; }

    bool Write(const void* data, size_t n)
    {
        m_total += n;
        if (m_limit && m_total > m_limit) { m_overflowed = true; return false; }
        if (m_discard || n == 0) return true;
        DWORD wrote = 0;
        return WriteFile(m_h, data, (DWORD)n, &wrote, nullptr) && wrote == n;
    }
    uint64_t Total() const { return m_total; }
    bool Overflowed() const { return m_overflowed; }

private:
    bool     m_discard;
    uint64_t m_limit = 0;
    bool     m_overflowed = false;
    HANDLE   m_h     = INVALID_HANDLE_VALUE;
    uint64_t m_total = 0;
};

} // namespace

// ═════════════════════════════════════════════════════════════════════════
// Public helpers
// ═════════════════════════════════════════════════════════════════════════
bool IsCodecAvailable(const wchar_t* codecId)
{
    if (!codecId) return false;
    Codec& c = GetCodec(codecId);
    return c.module != nullptr && c.kind != Codec::Kind::None;
}

std::wstring GetCodecPath(const wchar_t* codecId)
{
    if (!codecId) return L"";
    return GetCodec(codecId).path;
}

// ═════════════════════════════════════════════════════════════════════════
// CCodecEngine
// ═════════════════════════════════════════════════════════════════════════
CCodecEngine::CCodecEngine(const wchar_t* codecId)
    : m_codecId(codecId ? codecId : L"")
{
    m_readOnly = true;   // these formats hold one file; we never rewrite them
}

CCodecEngine::~CCodecEngine() { Close(); }

bool CCodecEngine::Open(const std::wstring& path)
{
    Close();
    m_filePath = path;

    LPCWSTR ext  = PathFindExtensionW(path.c_str());
    m_formatName = Formats::NameFor(ext);

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return false;

    const uint64_t packed =
        ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;

    m_entry = ArchiveEntry{};
    m_entry.name              = Formats::InnerNameFor(path);
    m_entry.fullPath          = m_entry.name;
    m_entry.isDirectory       = false;
    m_entry.compressedSize    = packed;
    m_entry.uncompressedSize  = 0;       // unknown until the header says so
    m_entry.modifiedTime      = fad.ftLastWriteTime;
    m_entry.compressionMethod = m_formatName;
    m_entry.engineIndex       = 0;

    // Read the original size straight out of the frame header. This works
    // with no codec DLL present at all, and several of these formats
    // simply never record it — in which case the size stays unknown and
    // the view shows that honestly rather than printing "0 KB".
    uint64_t original = 0;
    if (ProbeOriginalSize(m_codecId, path, &original))
    {
        m_entry.uncompressedSize = original;
        m_entry.sizeKnown        = true;
        m_sizeIsExact            = true;
    }
    else
    {
        // The header did not record it. The lz4 and lz5 command line tools
        // only write the content size when asked (--content-size), and a
        // raw Brotli stream has nowhere to put it at all.
        m_entry.uncompressedSize = 0;
        m_entry.sizeKnown        = false;
        m_open = true;                      // Decode() needs us open
        MeasureSize();
    }

    m_open = true;
    return true;
}

// Decompress the stream to nowhere purely to count the bytes. Only worth
// doing for a small file: this runs while Explorer waits for the folder
// listing, so it is capped on both sides and simply gives up rather than
// holding up the view.
void CCodecEngine::MeasureSize()
{
    // 16 MB in: a one-file archive decodes in milliseconds, and anything
    // larger is not worth reading end to end for a column value.
    const uint64_t kMaxInput  = 16ull * 1024 * 1024;
    // 2 GB out: a ceiling no honest single file of this size will reach.
    const uint64_t kMaxOutput =  2ull * 1024 * 1024 * 1024;

    if (m_entry.compressedSize == 0 ||
        m_entry.compressedSize > kMaxInput) return;
    if (!IsCodecAvailable(m_codecId.c_str())) return;   // no DLL, no answer

    const std::wstring savedError = m_lastError;
    // Decode() stores the byte count it produced on success. Treat a run
    // that produced nothing from a non-empty archive as a failed probe:
    // whatever happened, zero is not the answer, and reporting it as one
    // is the exact bug this whole change set exists to stamp out.
    if (Decode(L"", nullptr, kMaxOutput) && m_entry.uncompressedSize > 0)
    {
        m_entry.sizeKnown = true;
    }
    else
    {
        m_entry.uncompressedSize = 0;
        m_entry.sizeKnown        = false;
    }
    m_lastError = savedError;      // a failed probe is not a user-facing error
}

bool CCodecEngine::Create(const std::wstring&) { return false; }

void CCodecEngine::Close()
{
    m_open = false;
    m_entry = ArchiveEntry{};
    m_sizeIsExact = false;
}

std::vector<ArchiveEntry> CCodecEngine::List(const std::wstring& dirPath)
{
    // One member, at the root. Any subdirectory is empty by definition.
    if (!m_open || !dirPath.empty()) return {};
    return { m_entry };
}

EngineCaps CCodecEngine::GetCaps() const
{
    EngineCaps caps;
    caps.engineName = m_formatName;

    Codec& c = GetCodec(m_codecId);
    caps.backendPath = c.path;

    if (c.module && c.kind != Codec::Kind::None)
    {
        caps.isStub     = false;
        caps.canExtract = true;
        caps.canTest    = true;
        // Writing is deliberately not offered: re-compressing would mean
        // choosing a level and silently rewriting the user's file.
        caps.canAdd = caps.canDelete = caps.canRename = false;
        return caps;
    }

    caps.isStub = true;
    caps.unavailableReason = c.error.empty()
        ? ThirdParty::DescribeSearch(m_codecId.c_str())
        : c.error + L"\n\n" + ThirdParty::DescribeSearch(m_codecId.c_str());
    return caps;
}

// ─────────────────────────────────────────────────────────────────────────
// Decode — the one real piece of work in this file
// ─────────────────────────────────────────────────────────────────────────
bool CCodecEngine::Decode(const std::wstring& destFile, ProgressFn cb,
                          uint64_t outputLimit)
{
    m_lastError.clear();

    Codec& c = GetCodec(m_codecId);
    if (!c.module || c.kind == Codec::Kind::None)
    {
        m_lastError = c.error;
        return false;
    }

    HANDLE in = CreateFileW(m_filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
    if (in == INVALID_HANDLE_VALUE)
    {
        m_lastError = L"The archive could not be opened for reading.";
        return false;
    }

    Sink out(destFile, outputLimit);
    if (!out.Ok())
    {
        CloseHandle(in);
        m_lastError = L"The destination file could not be created.";
        return false;
    }

    std::vector<uint8_t> inBuf(kChunk), outBuf(kChunk);
    const uint64_t totalIn = m_entry.compressedSize;
    uint64_t       readSoFar = 0;
    bool           ok = true, finished = false;
    int            lastPct = -1;

    auto report = [&]() {
        if (!cb || totalIn == 0) return;
        int pct = (int)((readSoFar * 100) / totalIn);
        if (pct != lastPct) { lastPct = pct; cb(pct, m_entry.name); }
    };

    // ── zstd ────────────────────────────────────────────────────────────
    if (c.kind == Codec::Kind::Zstd)
    {
        void* ds = c.zstd.createDStream();
        if (!ds) { CloseHandle(in); m_lastError = L"Out of memory."; return false; }
        c.zstd.initDStream(ds);

        while (ok && !finished)
        {
            DWORD got = 0;
            if (!ReadFile(in, inBuf.data(), (DWORD)inBuf.size(), &got, nullptr))
            { ok = false; m_lastError = L"Read error."; break; }
            if (got == 0) break;                    // input exhausted
            readSoFar += got;

            ZSTD_inBuffer zin{ inBuf.data(), got, 0 };
            while (zin.pos < zin.size)
            {
                ZSTD_outBuffer zout{ outBuf.data(), outBuf.size(), 0 };
                size_t rc = c.zstd.decompressStream(ds, &zout, &zin);
                if (c.zstd.isError(rc))
                {
                    ok = false;
                    m_lastError = L"The stream is damaged or is not "
                                  L"Zstandard data";
                    if (c.zstd.getErrorName)
                        m_lastError += L": " + Widen(c.zstd.getErrorName(rc));
                    m_lastError += L".";
                    break;
                }
                if (!out.Write(outBuf.data(), zout.pos))
                { ok = false; m_lastError = L"Write error."; break; }
                if (rc == 0) { finished = true; break; }   // frame complete
            }
            report();
        }
        c.zstd.freeDStream(ds);
    }
    // ── LZ4 / LZ5 / Lizard ──────────────────────────────────────────────
    else if (c.kind == Codec::Kind::Lz4Family)
    {
        void*  ctx = nullptr;
        size_t rc  = c.lz4.createDecompressionContext(&ctx, 100 /* *F_VERSION */);
        if (c.lz4.isError(rc) || !ctx)
        {
            CloseHandle(in);
            m_lastError = L"The codec refused to create a decompression context.";
            return false;
        }

        while (ok && !finished)
        {
            DWORD got = 0;
            if (!ReadFile(in, inBuf.data(), (DWORD)inBuf.size(), &got, nullptr))
            { ok = false; m_lastError = L"Read error."; break; }
            if (got == 0) break;
            readSoFar += got;

            size_t srcPos = 0;
            while (srcPos < got)
            {
                size_t srcSize = got - srcPos;
                size_t dstSize = outBuf.size();
                size_t hint = c.lz4.decompress(ctx, outBuf.data(), &dstSize,
                                               inBuf.data() + srcPos, &srcSize,
                                               nullptr);
                if (c.lz4.isError(hint))
                {
                    ok = false;
                    m_lastError = L"The stream is damaged or is not "
                                  L"a frame of this codec";
                    if (c.lz4.getErrorName)
                        m_lastError += L": " + Widen(c.lz4.getErrorName(hint));
                    m_lastError += L".";
                    break;
                }
                if (!out.Write(outBuf.data(), dstSize))
                { ok = false; m_lastError = L"Write error."; break; }

                // No input consumed and no output produced means the codec
                // cannot make progress; bail rather than spin forever.
                if (srcSize == 0 && dstSize == 0) { ok = false;
                    m_lastError = L"The codec stopped making progress."; break; }

                srcPos += srcSize;
                if (hint == 0) { finished = true; break; }   // frame complete
            }
            report();
        }
        c.lz4.freeDecompressionContext(ctx);
    }
    // ── brotli ──────────────────────────────────────────────────────────
    else if (c.kind == Codec::Kind::Brotli)
    {
        void* st = c.brotli.createInstance(nullptr, nullptr, nullptr);
        if (!st) { CloseHandle(in); m_lastError = L"Out of memory."; return false; }

        size_t totalOut = 0;
        while (ok && !finished)
        {
            DWORD got = 0;
            if (!ReadFile(in, inBuf.data(), (DWORD)inBuf.size(), &got, nullptr))
            { ok = false; m_lastError = L"Read error."; break; }
            if (got == 0) break;
            readSoFar += got;

            size_t         availIn = got;
            const uint8_t* nextIn  = inBuf.data();

            for (;;)
            {
                size_t   availOut = outBuf.size();
                uint8_t* nextOut  = outBuf.data();

                int r = c.brotli.decompressStream(st, &availIn, &nextIn,
                                                  &availOut, &nextOut, &totalOut);

                const size_t produced = outBuf.size() - availOut;
                if (produced && !out.Write(outBuf.data(), produced))
                { ok = false; m_lastError = L"Write error."; break; }

                if (r == kBrotliSuccess)          { finished = true; break; }
                if (r == kBrotliNeedsMoreInput)   { break; }  // refill
                if (r == kBrotliNeedsMoreOutput)  { continue; }
                ok = false;
                m_lastError = L"The stream is damaged or is not Brotli data.";
                break;
            }
            report();
        }
        c.brotli.destroyInstance(st);
    }

    CloseHandle(in);

    if (!ok && out.Overflowed())
        m_lastError = L"The stream expands beyond the size limit.";
    else if (ok && !finished)
    {
        // Ran out of input without the codec declaring the frame complete.
        ok = false;
        if (m_lastError.empty())
            m_lastError = L"The file ends in the middle of the compressed "
                          L"stream — it is incomplete or truncated.";
    }

    if (ok)
    {
        // Now we know the real size, even for the codecs that do not
        // advertise it up front.
        m_entry.uncompressedSize = out.Total();
        m_sizeIsExact = true;
        if (cb) cb(100, m_entry.name);
    }
    else if (!destFile.empty())
    {
        // Qualified: IArchiveEngine::DeleteFile is itself macro-expanded to
        // DeleteFileW by <windows.h>, so the member would hide the API.
        ::DeleteFileW(destFile.c_str());   // never leave a half-written file
    }
    return ok;
}

bool CCodecEngine::ExtractFile(const ArchiveEntry& e,
                               const std::wstring& destDir, ProgressFn cb)
{
    if (!m_open || e.isDirectory) return false;

    std::wstring dest = destDir;
    if (!dest.empty() && dest.back() != L'\\') dest += L'\\';
    SHCreateDirectoryExW(nullptr, dest.c_str(), nullptr);
    dest += m_entry.name;

    if (!Decode(dest, cb))
    {
        if (!m_lastError.empty())
            MessageBoxW(nullptr, m_lastError.c_str(), L"ArchiveFldr",
                        MB_ICONWARNING | MB_OK);
        return false;
    }
    return true;
}

bool CCodecEngine::ExtractAll(const std::wstring& destDir, ProgressFn cb)
{
    return ExtractFile(m_entry, destDir, cb);
}

bool CCodecEngine::Test(ProgressFn cb)
{
    return m_open && Decode(L"", cb);
}

bool CCodecEngine::AddFile(const std::wstring&, const std::wstring&, ProgressFn)
{ return false; }
bool CCodecEngine::DeleteFile(const ArchiveEntry&) { return false; }
bool CCodecEngine::Rename(const ArchiveEntry&, const std::wstring&) { return false; }

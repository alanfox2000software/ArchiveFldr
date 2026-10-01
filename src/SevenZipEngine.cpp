// SevenZipEngine.cpp
// Real .7z support via an external, user-supplied 7-Zip engine DLL.
//
// We never ship or statically link any 7-Zip decoder code. At runtime we
// LoadLibrary() a bitness-matched engine DLL (thirdparty\7z\7z.64.dll or
// thirdparty\7z\7z.32.dll, placed next to ArchiveFldr.64.dll / ArchiveFldr.32.dll)
// and talk to it purely through its public "COM-lite" ABI — the same
// contract 7-Zip's own CPP/7zip/UI/Client7z sample uses. See Sdk7z.h for
// the interface/GUID declarations and provenance notes.
//
// KNOWN LIMITATION (v1): no password-prompt UI. CryptoGetTextPassword()
// always reports "no password" — unencrypted archives are unaffected;
// opening an archive with encrypted headers, or extracting encrypted
// items, will fail (or fail per-item) until a password UI is wired in.
#include "stdafx.h"
#include "SevenZipEngine.h"
#include "Formats.h"
#include "Sdk7z.h"
#include "ThirdParty.h"
#include "ArchiveWriter.h"

// ═════════════════════════════════════════════════════════
// Engine DLL discovery / loading
// ═════════════════════════════════════════════════════════
namespace {

HMODULE                    g_hLib          = nullptr;
Func7z_CreateObject        g_pCreateObject = nullptr;
Func7z_GetNumberOfFormats  g_pNumFormats   = nullptr;
Func7z_GetHandlerProperty2 g_pHandlerProp  = nullptr;
std::wstring               g_enginePath;
std::once_flag             g_initOnce;

// Engine discovery is delegated to the universal third-party DLL layout
// (see ThirdParty.h): thirdparty\7z\7z.64.dll, thirdparty\7z\7z.dll,
// <ArchiveFldr dir>\7z.64.dll, an installed 7-Zip, ... — one shared search
// order that every future engine DLL inherits for free.
std::wstring Resolve7zDllPath()
{
    return ThirdParty::Resolve(L"7z");
}

void InitEngineOnce()
{
    g_enginePath = Resolve7zDllPath();
    if (g_enginePath.empty()) return;

    // Loaded with LOAD_WITH_ALTERED_SEARCH_PATH so the engine resolves its
    // own dependencies from the folder it lives in, not from ours.
    g_hLib = ThirdParty::Load(g_enginePath);
    if (!g_hLib) { g_enginePath.clear(); return; }

    g_pCreateObject = reinterpret_cast<Func7z_CreateObject>(
        GetProcAddress(g_hLib, "CreateObject"));

    // Optional: present in every real 7z.dll, absent from some cut-down
    // builds. Without them we fall back to the 7z format class alone.
    g_pNumFormats = reinterpret_cast<Func7z_GetNumberOfFormats>(
        GetProcAddress(g_hLib, "GetNumberOfFormats"));
    g_pHandlerProp = reinterpret_cast<Func7z_GetHandlerProperty2>(
        GetProcAddress(g_hLib, "GetHandlerProperty2"));

    if (!g_pCreateObject)
    {
        FreeLibrary(g_hLib);
        g_hLib = nullptr;
        g_enginePath.clear();
    }
}

Func7z_CreateObject Get7zCreateObjectFunc()
{
    std::call_once(g_initOnce, InitEngineOnce);
    return g_pCreateObject;
}

// ═════════════════════════════════════════════════════════
// Format handlers published by 7z.dll
//
// 7z.dll knows its own format list, so ask it rather than carrying a
// table of class GUIDs that would silently rot every time 7-Zip adds a
// format. Enumerated once, on first use.
// ═════════════════════════════════════════════════════════
struct Handler7z
{
    GUID                      clsid{};
    std::wstring              name;      // "tar", "wim", "zip", ...
    std::vector<std::wstring> exts;      // without the leading dot
};

std::vector<Handler7z> g_handlers;
std::once_flag         g_handlersOnce;

std::wstring HandlerPropStr(UINT32 i, PROPID pid)
{
    PROPVARIANT v; PropVariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(g_pHandlerProp(i, pid, &v)) && v.vt == VT_BSTR && v.bstrVal)
        out = v.bstrVal;
    PropVariantClear(&v);
    return out;
}

void EnumerateHandlersOnce()
{
    if (!g_pNumFormats || !g_pHandlerProp) return;

    UINT32 count = 0;
    if (FAILED(g_pNumFormats(&count)) || count == 0 || count > 512) return;

    g_handlers.reserve(count);
    for (UINT32 i = 0; i < count; ++i)
    {
        Handler7z h;

        // The class id arrives as a BSTR carrying the raw 16 GUID bytes,
        // not as text — so check the byte length before copying.
        PROPVARIANT v; PropVariantInit(&v);
        bool haveClsid = false;
        if (SUCCEEDED(g_pHandlerProp(i, kHandlerClassID, &v)) &&
            v.vt == VT_BSTR && v.bstrVal &&
            SysStringByteLen(v.bstrVal) == sizeof(GUID))
        {
            memcpy(&h.clsid, v.bstrVal, sizeof(GUID));
            haveClsid = true;
        }
        PropVariantClear(&v);
        if (!haveClsid) continue;

        h.name = HandlerPropStr(i, kHandlerName);

        // Extensions come space separated: "tar ova".
        std::wstring ext = HandlerPropStr(i, kHandlerExtension);
        size_t start = 0;
        while (start <= ext.size())
        {
            size_t sp = ext.find(L' ', start);
            if (sp == std::wstring::npos) sp = ext.size();
            if (sp > start) h.exts.push_back(ext.substr(start, sp - start));
            if (sp == ext.size()) break;
            start = sp + 1;
        }

        g_handlers.push_back(std::move(h));
    }
}

const std::vector<Handler7z>& Handlers()
{
    Get7zCreateObjectFunc();                       // make sure the DLL is in
    std::call_once(g_handlersOnce, EnumerateHandlersOnce);
    return g_handlers;
}

// Handlers whose declared extension list contains `ext` (".tar" -> "tar").
std::vector<const Handler7z*> HandlersForExt(const wchar_t* ext)
{
    std::vector<const Handler7z*> out;
    if (!ext || !*ext) return out;
    const wchar_t* bare = (*ext == L'.') ? ext + 1 : ext;

    for (const auto& h : Handlers())
        for (const auto& e : h.exts)
            if (_wcsicmp(e.c_str(), bare) == 0) { out.push_back(&h); break; }
    return out;
}

// ═════════════════════════════════════════════════════════
// PROPVARIANT helpers
// ═════════════════════════════════════════════════════════
bool PropGetBool(IInArchive7z* arc, UINT32 idx, PROPID pid, bool defVal = false)
{
    PROPVARIANT v; PropVariantInit(&v);
    bool res = defVal;
    if (SUCCEEDED(arc->GetProperty(idx, pid, &v)) && v.vt == VT_BOOL)
        res = (v.boolVal != VARIANT_FALSE);
    PropVariantClear(&v);
    return res;
}

// Like PropGetUInt64, but says whether the archive actually carried the
// property. GetProperty succeeds with VT_EMPTY for anything a format does
// not record, so a plain "returned 0" cannot be trusted.
// As above, for 64-bit values. The distinction matters most for the
// single-stream formats: bzip2 records no uncompressed size anywhere, so
// 7-Zip reports VT_EMPTY and a plain default of 0 would be shown to the
// user as a confident "0 KB".
bool PropGetUInt64If(IInArchive7z* arc, UINT32 idx, PROPID pid, uint64_t* out)
{
    PROPVARIANT v; PropVariantInit(&v);
    bool got = false;
    if (SUCCEEDED(arc->GetProperty(idx, pid, &v)))
    {
        switch (v.vt)
        {
        case VT_UI1: *out = v.bVal;  got = true; break;
        case VT_UI2: *out = v.uiVal; got = true; break;
        case VT_UI4: *out = v.ulVal; got = true; break;
        case VT_UI8: *out = v.uhVal.QuadPart; got = true; break;
        case VT_I4:  *out = (uint64_t)(int64_t)v.lVal; got = true; break;
        case VT_I8:  *out = (uint64_t)v.hVal.QuadPart; got = true; break;
        default: break;        // VT_EMPTY / VT_NULL: not stored
        }
    }
    PropVariantClear(&v);
    return got;
}

bool PropGetUInt32If(IInArchive7z* arc, UINT32 idx, PROPID pid, uint32_t* out)
{
    PROPVARIANT v; PropVariantInit(&v);
    bool got = false;
    if (SUCCEEDED(arc->GetProperty(idx, pid, &v)))
    {
        switch (v.vt)
        {
        case VT_UI1: *out = v.bVal;  got = true; break;
        case VT_UI2: *out = v.uiVal; got = true; break;
        case VT_UI4: *out = v.ulVal; got = true; break;
        case VT_UI8: *out = (uint32_t)v.uhVal.QuadPart; got = true; break;
        case VT_I4:  *out = (uint32_t)v.lVal; got = true; break;
        default: break;        // VT_EMPTY / VT_NULL: not stored
        }
    }
    PropVariantClear(&v);
    return got;
}

uint64_t PropGetUInt64(IInArchive7z* arc, UINT32 idx, PROPID pid, uint64_t defVal = 0)
{
    PROPVARIANT v; PropVariantInit(&v);
    uint64_t res = defVal;
    if (SUCCEEDED(arc->GetProperty(idx, pid, &v)))
    {
        switch (v.vt)
        {
        case VT_UI1: res = v.bVal; break;
        case VT_UI2: res = v.uiVal; break;
        case VT_UI4: res = v.ulVal; break;
        case VT_UI8: res = v.uhVal.QuadPart; break;
        case VT_I4:  res = (uint64_t)(int64_t)v.lVal; break;
        case VT_I8:  res = (uint64_t)v.hVal.QuadPart; break;
        default: break;
        }
    }
    PropVariantClear(&v);
    return res;
}

std::wstring PropGetString(IInArchive7z* arc, UINT32 idx, PROPID pid)
{
    PROPVARIANT v; PropVariantInit(&v);
    std::wstring res;
    if (SUCCEEDED(arc->GetProperty(idx, pid, &v)) && v.vt == VT_BSTR && v.bstrVal)
        res.assign(v.bstrVal, SysStringLen(v.bstrVal));
    PropVariantClear(&v);
    return res;
}

FILETIME PropGetFileTime(IInArchive7z* arc, UINT32 idx, PROPID pid)
{
    PROPVARIANT v; PropVariantInit(&v);
    FILETIME ft{};
    if (SUCCEEDED(arc->GetProperty(idx, pid, &v)) && v.vt == VT_FILETIME)
        ft = v.filetime;
    PropVariantClear(&v);
    return ft;
}

// ═════════════════════════════════════════════════════════
// CInFileStream — IInStream over a real file, for Open()
// ═════════════════════════════════════════════════════════
class CInFileStream final : public IInStream7z
{
public:
    // NOTE: no `override` here — COM interfaces (IUnknown and therefore
    // every 7-Zip "COM-lite" interface) deliberately have no virtual
    // destructor, so there is nothing to override (MSVC: error C3668).
    // The class is `final` and only ever destroyed through `delete this`
    // in Release() below, where the static type is exact, so a plain
    // non-virtual destructor is correct and keeps the vtable layout
    // byte-for-byte identical to the interface the engine DLL expects.
    ~CInFileStream() { CloseFile(); }

    bool OpenFile(const std::wstring& path)
    {
        m_handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return m_handle != INVALID_HANDLE_VALUE;
    }
    void CloseFile()
    {
        if (m_handle != INVALID_HANDLE_VALUE) { CloseHandle(m_handle); m_handle = INVALID_HANDLE_VALUE; }
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ISequentialInStream7z) ||
            IsEqualIID(riid, IID_IInStream7z))
            *ppv = static_cast<IInStream7z*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG n = (ULONG)InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP Read(void* data, UINT32 size, UINT32* processedSize) override
    {
        DWORD read = 0;
        BOOL ok = ReadFile(m_handle, data, size, &read, nullptr);
        if (processedSize) *processedSize = read;
        return ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    STDMETHODIMP Seek(INT64 offset, UINT32 seekOrigin, UINT64* newPosition) override
    {
        LARGE_INTEGER li; li.QuadPart = offset;
        LARGE_INTEGER res{};
        DWORD method = (seekOrigin == 0) ? FILE_BEGIN : (seekOrigin == 1) ? FILE_CURRENT : FILE_END;
        BOOL ok = SetFilePointerEx(m_handle, li, &res, method);
        if (newPosition) *newPosition = (UINT64)res.QuadPart;
        return ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

private:
    LONG   m_ref   = 1;
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

// ═════════════════════════════════════════════════════════
// COutFileStream — ISequentialOutStream writing to a real file
// ═════════════════════════════════════════════════════════
class COutFileStream final : public ISequentialOutStream7z
{
public:
    // No `override` — see the note on CInFileStream above: COM interfaces
    // have no virtual destructor to override (MSVC: error C3668), and this
    // object is only ever deleted through its exact type in Release().
    ~COutFileStream() { CloseFile(); }

    bool CreateOutputFile(const std::wstring& path)
    {
        DWORD existing = GetFileAttributesW(path.c_str());
        if (existing != INVALID_FILE_ATTRIBUTES && (existing & FILE_ATTRIBUTE_READONLY))
            SetFileAttributesW(path.c_str(), existing & ~FILE_ATTRIBUTE_READONLY);

        m_handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return m_handle != INVALID_HANDLE_VALUE;
    }
    void CloseFile()
    {
        if (m_handle != INVALID_HANDLE_VALUE) { CloseHandle(m_handle); m_handle = INVALID_HANDLE_VALUE; }
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ISequentialOutStream7z))
            *ppv = static_cast<ISequentialOutStream7z*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG n = (ULONG)InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP Write(const void* data, UINT32 size, UINT32* processedSize) override
    {
        DWORD written = 0;
        BOOL ok = WriteFile(m_handle, data, size, &written, nullptr);
        if (processedSize) *processedSize = written;
        return ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

private:
    LONG   m_ref   = 1;
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

// ═════════════════════════════════════════════════════════
// CArchiveOpenCallback
// ═════════════════════════════════════════════════════════
class CArchiveOpenCallback final : public IArchiveOpenCallback7z,
                                    public ICryptoGetTextPassword7z
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IArchiveOpenCallback7z))
            *ppv = static_cast<IArchiveOpenCallback7z*>(this);
        else if (IsEqualIID(riid, IID_ICryptoGetTextPassword7z))
            *ppv = static_cast<ICryptoGetTextPassword7z*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG n = (ULONG)InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP SetTotal(const UINT64*, const UINT64*) override { return S_OK; }
    STDMETHODIMP SetCompleted(const UINT64*, const UINT64*) override { return S_OK; }

    // v1 limitation: no password-prompt UI (see file header comment).
    STDMETHODIMP CryptoGetTextPassword(BSTR* password) override
    {
        if (password) *password = nullptr;
        return S_OK;
    }

private:
    LONG m_ref = 1;
};

// ═════════════════════════════════════════════════════════
// CArchiveExtractCallback
// ═════════════════════════════════════════════════════════
class CArchiveExtractCallback final : public IArchiveExtractCallback7z,
                                       public ICryptoGetTextPassword7z
{
public:
    CArchiveExtractCallback(IInArchive7z* archive, std::wstring destDir,
                             UINT32 totalCount, ProgressFn cb)
        : m_archive(archive), m_destDir(std::move(destDir)),
          m_total(totalCount ? totalCount : 1), m_cb(std::move(cb)) {}

    bool HadError() const { return m_hadError; }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IArchiveExtractCallback7z))
            *ppv = static_cast<IArchiveExtractCallback7z*>(this);
        else if (IsEqualIID(riid, IID_ICryptoGetTextPassword7z))
            *ppv = static_cast<ICryptoGetTextPassword7z*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG n = (ULONG)InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return n;
    }

    // IProgress7z
    STDMETHODIMP SetTotal(UINT64) override { return S_OK; }
    STDMETHODIMP SetCompleted(const UINT64*) override { return S_OK; }

    // IArchiveExtractCallback7z
    STDMETHODIMP GetStream(UINT32 index, ISequentialOutStream7z** outStream,
                            INT32 askExtractMode) override
    {
        if (outStream) *outStream = nullptr;
        m_curOut.Reset();
        m_curOutSpec  = nullptr;
        m_curIsDir    = false;
        m_curDiskPath.clear();

        std::wstring path = PropGetString(m_archive, index, k7zPidPath);
        for (auto& ch : path) if (ch == L'\\') ch = L'/';
        bool isDir = PropGetBool(m_archive, index, k7zPidIsDir, false);
        m_curMTime  = PropGetFileTime(m_archive, index, k7zPidMTime);
        m_curAttrib = (DWORD)PropGetUInt64(m_archive, index, k7zPidAttrib, 0);
        m_curIsDir  = isDir;
        m_curPath   = path;

        if (askExtractMode != N7zExtract::kExtract)
            return S_OK; // test / skip / read-external — no output stream needed

        std::wstring diskPath = m_destDir;
        if (!diskPath.empty() && diskPath.back() != L'\\') diskPath += L'\\';
        std::wstring rel = path;
        for (auto& ch : rel) if (ch == L'/') ch = L'\\';
        diskPath += rel;
        m_curDiskPath = diskPath;

        if (isDir)
        {
            SHCreateDirectoryExW(nullptr, diskPath.c_str(), nullptr);
            return S_OK;
        }

        std::wstring parent = diskPath;
        size_t slash = parent.find_last_of(L'\\');
        if (slash != std::wstring::npos)
        {
            parent.resize(slash);
            SHCreateDirectoryExW(nullptr, parent.c_str(), nullptr);
        }

        auto* outRaw = new COutFileStream();
        if (!outRaw->CreateOutputFile(diskPath))
        {
            delete outRaw;
            return S_FALSE; // per-item data error; extraction of other items continues
        }

        ComPtr<ISequentialOutStream7z> local;
        local.Attach(outRaw);   // local now owns the one ref from `new`
        m_curOutSpec = outRaw;  // non-owning observer, for explicit CloseFile()
        m_curOut     = local;   // copy -> AddRef (now 2 refs total)
        *outStream   = local.Detach(); // hand our (still-live) ref to the caller
        return S_OK;
    }

    STDMETHODIMP PrepareOperation(INT32) override { return S_OK; }

    STDMETHODIMP SetOperationResult(INT32 opRes) override
    {
        // Deterministically flush/close our file handle regardless of
        // whether the engine has released its own stream reference yet.
        if (m_curOutSpec) m_curOutSpec->CloseFile();

        if (opRes == N7zExtract::kOK)
        {
            if (!m_curIsDir && !m_curDiskPath.empty())
            {
                HANDLE h = CreateFileW(m_curDiskPath.c_str(), FILE_WRITE_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                if (h != INVALID_HANDLE_VALUE)
                {
                    if (m_curMTime.dwLowDateTime || m_curMTime.dwHighDateTime)
                        SetFileTime(h, nullptr, nullptr, &m_curMTime);
                    CloseHandle(h);
                }
                // Low word of 7z's attrib property holds real Win32 file attributes.
                DWORD attr = m_curAttrib & 0xFFFF;
                if (attr != 0)
                    SetFileAttributesW(m_curDiskPath.c_str(), attr);
            }
        }
        else
        {
            m_hadError = true;
        }

        m_curOut.Reset();
        m_curOutSpec = nullptr;

        m_done++;
        if (m_cb)
        {
            int pct = (int)((UINT64)m_done * 100 / m_total);
            if (pct > 100) pct = 100;
            m_cb(pct, m_curPath);
        }
        return S_OK;
    }

    // v1 limitation: no password-prompt UI (see file header comment).
    STDMETHODIMP CryptoGetTextPassword(BSTR* password) override
    {
        if (password) *password = nullptr;
        return S_OK;
    }

private:
    LONG           m_ref = 1;
    IInArchive7z*  m_archive; // not owned
    std::wstring   m_destDir;
    UINT32         m_total;
    UINT32         m_done = 0;
    ProgressFn     m_cb;
    bool           m_hadError = false;

    bool                           m_curIsDir = false;
    std::wstring                   m_curPath;
    std::wstring                   m_curDiskPath;
    FILETIME                       m_curMTime{};
    DWORD                          m_curAttrib = 0;
    ComPtr<ISequentialOutStream7z> m_curOut;
    COutFileStream*                m_curOutSpec = nullptr;
};

void EnsureSyntheticDir(std::vector<ArchiveEntry>& entries,
                         std::unordered_map<std::wstring, bool>& known,
                         const std::wstring& dirPathWithSlash)
{
    if (dirPathWithSlash.empty() || known.count(dirPathWithSlash)) return;

    std::wstring trimmed = dirPathWithSlash.substr(0, dirPathWithSlash.size() - 1);
    size_t slash = trimmed.rfind(L'/');
    std::wstring parent = (slash == std::wstring::npos) ? L"" : trimmed.substr(0, slash + 1);
    if (!parent.empty()) EnsureSyntheticDir(entries, known, parent);

    known[dirPathWithSlash] = true;
    ArchiveEntry d;
    d.fullPath     = dirPathWithSlash;
    d.isDirectory  = true;
    d.engineIndex  = -1; // synthesized — no backing archive item
    d.name         = (slash == std::wstring::npos) ? trimmed : trimmed.substr(slash + 1);
    entries.push_back(std::move(d));
}

} // anonymous namespace

bool Is7zEngineAvailable() { return Get7zCreateObjectFunc() != nullptr; }
std::wstring Get7zEnginePath()
{
    std::call_once(g_initOnce, InitEngineOnce);
    return g_enginePath;
}

// ═════════════════════════════════════════════════════════
// C7zArchiveEngine
// ═════════════════════════════════════════════════════════
C7zArchiveEngine::C7zArchiveEngine()  = default;
C7zArchiveEngine::~C7zArchiveEngine() { Close(); }

bool C7zArchiveEngine::Open(const std::wstring& path)
{
    Close();
    m_lastError.clear();
    m_filePath = path;
    m_readOnly = true;

    Func7z_CreateObject createObj = Get7zCreateObjectFunc();
    if (!createObj)
    {
        m_lastError = L"7-Zip engine not found. Place 7z.64.dll / 7z.32.dll under "
                      L"thirdparty\\7z\\ next to ArchiveFldr.";
        return false;
    }

    // Which handler? Start with the ones that claim this extension, then
    // fall back to every other handler, so a .tar that is really a .gz —
    // or a file with no extension at all — still opens. Each attempt gets
    // a fresh stream, because a failed Open leaves the position anywhere.
    LPCWSTR ext = PathFindExtensionW(path.c_str());

    std::vector<const Handler7z*> candidates = HandlersForExt(ext);
    const size_t preferred = candidates.size();
    for (const auto& h : Handlers())
    {
        bool already = false;
        for (size_t i = 0; i < preferred; ++i)
            if (candidates[i] == &h) { already = true; break; }
        if (!already) candidates.push_back(&h);
    }

    ComPtr<IInArchive7z> archive;
    std::wstring         chosenName;

    auto tryHandler = [&](const GUID& clsid, const std::wstring& name) -> bool
    {
        auto* fsRaw = new CInFileStream();
        if (!fsRaw->OpenFile(path)) { delete fsRaw; return false; }
        ComPtr<IInStream7z> inStream;
        inStream.Attach(fsRaw);

        void* rawArchive = nullptr;
        if (FAILED(createObj(&clsid, &IID_IInArchive7z, &rawArchive)) ||
            !rawArchive)
            return false;

        ComPtr<IInArchive7z> candidate;
        candidate.Attach(static_cast<IInArchive7z*>(rawArchive));

        ComPtr<IArchiveOpenCallback7z> openCb;
        openCb.Attach(new CArchiveOpenCallback());

        UINT64 maxCheckStartPosition = 1 << 20;   // tolerate SFX stubs etc.
        if (FAILED(candidate->Open(inStream.Get(), &maxCheckStartPosition,
                                   openCb.Get())))
            return false;

        archive    = candidate;
        chosenName = name;
        return true;
    };

    if (candidates.empty())
    {
        // No handler list available (a cut-down 7z.dll without
        // GetNumberOfFormats). The 7z format class is always there.
        tryHandler(CLSID_CFormat7z, L"7-Zip");
    }
    else
    {
        for (const Handler7z* h : candidates)
            if (tryHandler(h->clsid, h->name)) break;
    }

    if (!archive)
    {
        if (!PathFileExistsW(path.c_str()))
        {
            m_lastError = L"The archive file no longer exists.";
            return false;
        }

        // Say what was actually tried. "No handler could read this" on its
        // own gives nobody anything to act on; the engine path, the handler
        // count and the leading bytes together usually identify the problem
        // on sight (wrong-bitness DLL, cut-down build, truncated file).
        m_lastError = L"No 7-Zip handler could read this file.\n\n";

        const std::wstring enginePath = Get7zEnginePath();
        m_lastError += L"Engine: " +
            (enginePath.empty() ? std::wstring(L"<none>") : enginePath) + L"\n";

        wchar_t buf[128] = {};
        const auto claimed = HandlersForExt(ext);
        swprintf_s(buf, 128, L"Handlers published: %u, claiming %s: %u\n",
                   (unsigned)Handlers().size(),
                   (ext && *ext) ? ext : L"this extension",
                   (unsigned)claimed.size());
        m_lastError += buf;

        if (!claimed.empty())
        {
            m_lastError += L"Tried first: ";
            for (size_t i = 0; i < claimed.size(); ++i)
            {
                if (i) m_lastError += L", ";
                m_lastError += claimed[i]->name;
            }
            m_lastError += L"\n";
        }

        // The first bytes identify the real format regardless of the name.
        if (HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            h != INVALID_HANDLE_VALUE)
        {
            uint8_t sig[8] = {};
            DWORD got = 0;
            ReadFile(h, sig, sizeof(sig), &got, nullptr);
            CloseHandle(h);
            if (got)
            {
                m_lastError += L"First bytes:";
                for (DWORD i = 0; i < got; ++i)
                {
                    swprintf_s(buf, 128, L" %02X", sig[i]);
                    m_lastError += buf;
                }
                m_lastError += L"\n";
            }
        }

        m_lastError += L"\nThe file may be damaged or incomplete, or its "
                       L"headers may be encrypted — password-protected "
                       L"headers are not yet supported.";
        return false;
    }

    // Prefer the friendly name from the format table over 7-Zip's short
    // handler id, so the UI says "Windows image" rather than "wim".
    {
        std::wstring pretty = Formats::NameFor(ext);
        m_formatName = pretty.empty() ? chosenName : pretty;
    }

    m_archive = archive;
    m_open    = true;
    BuildEntryList();
    return true;
}

bool C7zArchiveEngine::Create(const std::wstring&)
{
    m_lastError = L"Creating/modifying .7z archives is not supported by this engine "
                  L"(extraction-only).";
    return false;
}

void C7zArchiveEngine::Close()
{
    if (m_archive) { m_archive->Close(); m_archive.Reset(); }
    m_allEntries.clear();
    m_open = false;
}

void C7zArchiveEngine::BuildEntryList()
{
    m_allEntries.clear();
    if (!m_archive) return;

    UINT32 numItems = 0;
    m_archive->GetNumberOfItems(&numItems);

    std::unordered_map<std::wstring, bool> known;
    // index into m_allEntries -> solid block it belongs to
    std::vector<std::pair<size_t, uint64_t>> blockOf;

    for (UINT32 i = 0; i < numItems; i++)
    {
        std::wstring path = PropGetString(m_archive.Get(), i, k7zPidPath);
        for (auto& ch : path) if (ch == L'\\') ch = L'/';
        bool isDir = PropGetBool(m_archive.Get(), i, k7zPidIsDir, false);

        if (isDir)
        {
            std::wstring p = path;
            if (!p.empty() && p.back() != L'/') p += L'/';
            if (p.empty()) continue;

            std::wstring trimmed = p.substr(0, p.size() - 1);
            size_t slash = trimmed.rfind(L'/');
            std::wstring parent = (slash == std::wstring::npos) ? L"" : trimmed.substr(0, slash + 1);
            if (!parent.empty()) EnsureSyntheticDir(m_allEntries, known, parent);

            if (known.count(p)) continue; // already represented by a synthesized placeholder

            ArchiveEntry e;
            e.isDirectory = true;
            e.engineIndex = (int64_t)i;
            e.fullPath    = p;
            e.name        = (slash == std::wstring::npos) ? trimmed : trimmed.substr(slash + 1);
            known[p] = true;
            m_allEntries.push_back(std::move(e));
        }
        else
        {
            // Single-stream containers — .xz, .gz, .bz2, .lzma — hold one
            // nameless payload, so 7-Zip reports an empty kpidPath and we
            // used to skip the only item there was, leaving Explorer to
            // say "This folder is empty". Name it after the archive with
            // the suffix removed, which is what 7-Zip's own UI does.
            if (path.empty())
            {
                if (numItems != 1) continue;     // genuinely unnamed, skip
                path = Formats::InnerNameFor(m_filePath);
                if (path.empty()) continue;
            }
            size_t slash = path.rfind(L'/');
            std::wstring parent = (slash == std::wstring::npos) ? L"" : path.substr(0, slash + 1);
            if (!parent.empty()) EnsureSyntheticDir(m_allEntries, known, parent);

            ArchiveEntry e;
            e.isDirectory        = false;
            e.engineIndex        = (int64_t)i;
            e.fullPath           = path;
            e.name               = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
            e.sizeKnown          = PropGetUInt64If(m_archive.Get(), i,
                                        k7zPidSize, &e.uncompressedSize);
            e.compressedSize     = PropGetUInt64(m_archive.Get(), i, k7zPidPackSize, 0);
            e.hasCrc             = PropGetUInt32If(m_archive.Get(), i,
                                                   k7zPidCRC, &e.crc32);
            e.modifiedTime       = PropGetFileTime(m_archive.Get(), i, k7zPidMTime);
            e.isEncrypted        = PropGetBool(m_archive.Get(), i, k7zPidEncrypted, false);
            // kpidMethod is the per-item coder chain ("LZMA2:24", "Copy", …).
            // Older engines leave it empty for some archives, hence the
            // fallback — the details view shows this verbatim.
            e.compressionMethod  = PropGetString(m_archive.Get(), i, k7zPidMethod);
            // Only .7z used to reach this engine, so an unreported method
            // was labelled "7z". Now that tar, zip, iso and the rest come
            // through here that would be a plain lie — a tar member is
            // stored, not 7z-compressed. Leave it empty and let the view
            // show "Store", which is what an unreported method means.
            if (e.isEncrypted)
            {
                e.compressionMethod = e.compressionMethod.empty()
                    ? std::wstring(L"Encrypted")
                    : e.compressionMethod + L" (encrypted)";
            }
            const uint64_t block = PropGetUInt64(m_archive.Get(), i,
                                                 k7zPidBlock, UINT64_MAX);
            m_allEntries.push_back(std::move(e));
            blockOf.emplace_back(m_allEntries.size() - 1, block);
        }
    }

    SpreadSolidBlockPackSizes(blockOf);
}

// Share each solid block's packed size out among the files inside it.
//
// In a solid archive 7-Zip reports kpidPackSize for a whole block against a
// single member and zero against all the others. Taken literally that says a
// small file at the head of a block compressed to many times its own size:
// bin\7zdex.exe came out at -1402% and bin\installer\config.exe at -21235%,
// while every file behind them claimed to pack down to nothing.
//
// The archive does not record how much of the block each file really costs,
// so split it in proportion to the unpacked sizes. Every row then shows a
// believable figure, the archive total stays correct to the byte (the last
// member absorbs the rounding), and entries touched here are flagged so the
// properties text can say the number is a share rather than a measurement.
void C7zArchiveEngine::SpreadSolidBlockPackSizes(
    const std::vector<std::pair<size_t, uint64_t>>& blockOf)
{
    std::unordered_map<uint64_t, std::vector<size_t>> blocks;
    for (const auto& [idx, blk] : blockOf)
    {
        if (blk == UINT64_MAX) continue;      // archive reports no blocks
        blocks[blk].push_back(idx);
    }

    for (auto& [blk, members] : blocks)
    {
        (void)blk;
        if (members.size() < 2) continue;     // nothing solid about it

        uint64_t packed = 0, total = 0;
        for (size_t i : members)
        {
            packed += m_allEntries[i].compressedSize;
            total  += m_allEntries[i].uncompressedSize;
        }
        if (packed == 0 || total == 0) continue;

        uint64_t handed = 0;
        for (size_t n = 0; n < members.size(); ++n)
        {
            ArchiveEntry& e = m_allEntries[members[n]];
            e.packedIsShared = true;

            if (n + 1 == members.size())      // last member takes the rest
            {
                e.compressedSize = packed - handed;
                break;
            }
            const long double share = (long double)packed *
                (long double)e.uncompressedSize / (long double)total;
            uint64_t v = (uint64_t)(share + 0.5L);
            if (handed + v > packed) v = packed - handed;
            e.compressedSize = v;
            handed += v;
        }
    }
}

std::vector<ArchiveEntry> C7zArchiveEngine::List(const std::wstring& dirPath)
{
    std::vector<ArchiveEntry> result;
    if (!m_open) return result;

    std::wstring prefix = dirPath;
    if (!prefix.empty() && prefix.back() != L'/') prefix += L'/';

    std::unordered_set<std::wstring> seen;
    for (auto& e : m_allEntries)
    {
        const std::wstring& fp = e.fullPath;
        if (prefix.empty())
        {
            std::wstring noTrail = fp;
            if (!noTrail.empty() && noTrail.back() == L'/') noTrail.pop_back();
            if (noTrail.find(L'/') == std::wstring::npos && !seen.count(fp))
            {
                seen.insert(fp);
                result.push_back(e);
            }
        }
        else
        {
            if (fp.size() <= prefix.size()) continue;
            if (fp.compare(0, prefix.size(), prefix) != 0) continue;
            std::wstring rest = fp.substr(prefix.size());
            if (rest.empty()) continue;
            size_t sl = rest.find(L'/');
            if (sl == std::wstring::npos)
            {
                if (!seen.count(fp)) { seen.insert(fp); result.push_back(e); }
            }
            else
            {
                std::wstring sub = prefix + rest.substr(0, sl + 1);
                if (!seen.count(sub))
                {
                    seen.insert(sub);
                    for (auto& de : m_allEntries)
                        if (de.fullPath == sub) { result.push_back(de); break; }
                }
            }
        }
    }

    std::sort(result.begin(), result.end(), [](const ArchiveEntry& a, const ArchiveEntry& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return result;
}

bool C7zArchiveEngine::ExtractIndices(const std::vector<UINT32>& indices,
                                       const std::wstring& destDir, ProgressFn cb)
{
    if (!m_open || !m_archive || indices.empty()) return false;
    SHCreateDirectoryExW(nullptr, destDir.c_str(), nullptr);

    auto* cbRaw = new CArchiveExtractCallback(m_archive.Get(), destDir,
                                               (UINT32)indices.size(), cb);
    ComPtr<IArchiveExtractCallback7z> extractCb;
    extractCb.Attach(cbRaw);

    HRESULT hr = m_archive->Extract(indices.data(), (UINT32)indices.size(), 0, extractCb.Get());
    bool ok = SUCCEEDED(hr) && !cbRaw->HadError();
    if (!ok)
        m_lastError = L"Extraction failed for one or more files "
                      L"(corrupt data, or an encrypted item with no supplied password).";
    return ok;
}

bool C7zArchiveEngine::ExtractAll(const std::wstring& destDir, ProgressFn cb)
{
    if (!m_open || !m_archive) return false;
    UINT32 numItems = 0;
    m_archive->GetNumberOfItems(&numItems);
    std::vector<UINT32> indices(numItems);
    for (UINT32 i = 0; i < numItems; i++) indices[i] = i;
    return ExtractIndices(indices, destDir, cb);
}

bool C7zArchiveEngine::ExtractFile(const ArchiveEntry& e, const std::wstring& destDir, ProgressFn cb)
{
    if (!m_open || !m_archive) return false;

    std::vector<UINT32> indices;
    if (!e.isDirectory)
    {
        if (e.engineIndex >= 0) indices.push_back((UINT32)e.engineIndex);
    }
    else
    {
        const std::wstring& prefix = e.fullPath; // ends with '/'
        for (auto& x : m_allEntries)
        {
            if (x.engineIndex < 0) continue; // synthesized placeholder, no real item
            if (x.fullPath == prefix ||
                (x.fullPath.size() > prefix.size() &&
                 x.fullPath.compare(0, prefix.size(), prefix) == 0))
                indices.push_back((UINT32)x.engineIndex);
        }
    }
    if (indices.empty())
    {
        m_lastError = L"Nothing to extract for this selection.";
        return false;
    }
    std::sort(indices.begin(), indices.end());
    return ExtractIndices(indices, destDir, cb);
}

EngineCaps C7zArchiveEngine::GetCaps() const
{
    EngineCaps c;
    c.engineName  = L"7-Zip";
    c.backendPath = Get7zEnginePath();
    c.canExtract  = Is7zEngineAvailable();
    c.canTest     = c.canExtract;
    c.isStub      = !c.canExtract;
    if (!c.canExtract)
        c.unavailableReason =
            L"No usable 7-Zip engine DLL was found, so .7z archives cannot "
            L"be read.\n\n" + ThirdParty::DescribeSearch(L"7z");
    return c;
}

bool C7zArchiveEngine::Test(ProgressFn cb)
{
    if (!m_open || !m_archive) return false;
    UINT32 numItems = 0;
    m_archive->GetNumberOfItems(&numItems);

    auto* cbRaw = new CArchiveExtractCallback(m_archive.Get(), L"", numItems, cb);
    ComPtr<IArchiveExtractCallback7z> extractCb;
    extractCb.Attach(cbRaw);

    HRESULT hr = m_archive->Extract(nullptr, (UINT32)-1, 1 /*testMode*/, extractCb.Get());
    bool ok = SUCCEEDED(hr) && !cbRaw->HadError();
    if (!ok) m_lastError = L"Archive integrity test reported errors.";
    return ok;
}

uint64_t C7zArchiveEngine::GetFileCount() const
{
    uint64_t n = 0;
    for (auto& e : m_allEntries) if (!e.isDirectory) n++;
    return n;
}

uint64_t C7zArchiveEngine::GetTotalSize() const
{
    uint64_t s = 0;
    for (auto& e : m_allEntries) s += e.uncompressedSize;
    return s;
}

uint64_t C7zArchiveEngine::GetPackedSize() const
{
    uint64_t s = 0;
    for (auto& e : m_allEntries) s += e.compressedSize;
    return s;
}

// ═════════════════════════════════════════════════════════════════════
// ArchiveWriter — creating archives through 7z.dll's IOutArchive
//
// Lives in this translation unit because it needs the same machinery
// the reader does: the handler list 7z.dll publishes about itself, the
// file stream wrappers, and the CreateObject entry point. Keeping it
// here is what lets compression support exactly the formats the user's
// copy of 7z.dll can write, with no second table of class GUIDs.
// ═════════════════════════════════════════════════════════════════════
namespace {

// ── Can this handler write, not just read? ───────────────
// NHandlerPropID::kUpdate. Handlers that only read (rar, for one)
// report false, and asking them for IOutArchive would fail later and
// less clearly.
bool HandlerCanUpdate(UINT32 index)
{
    if (!g_pHandlerProp) return false;
    PROPVARIANT v; PropVariantInit(&v);
    bool ok = false;
    if (SUCCEEDED(g_pHandlerProp(index, kHandlerUpdate, &v)) && v.vt == VT_BOOL)
        ok = (v.boolVal != VARIANT_FALSE);
    PropVariantClear(&v);
    return ok;
}

// Handler list indices stay parallel to g_handlers, so the writable
// flag is collected during the same enumeration pass the reader uses.
const std::vector<Handler7z>& WritableHandlers()
{
    static std::vector<Handler7z> s_writable;
    static std::once_flag         s_once;
    std::call_once(s_once, []
    {
        const auto& all = Handlers();           // forces enumeration
        UINT32 count = 0;
        if (g_pNumFormats && FAILED(g_pNumFormats(&count))) count = 0;

        // Re-walk by index: Handlers() skipped entries with no class id,
        // so positions are not interchangeable. Match on the class id.
        for (UINT32 i = 0; i < count; ++i)
        {
            if (!HandlerCanUpdate(i)) continue;
            PROPVARIANT v; PropVariantInit(&v);
            GUID clsid{};
            bool have = false;
            if (SUCCEEDED(g_pHandlerProp(i, kHandlerClassID, &v)) &&
                v.vt == VT_BSTR && v.bstrVal &&
                SysStringByteLen(v.bstrVal) == sizeof(GUID))
            {
                memcpy(&clsid, v.bstrVal, sizeof(GUID));
                have = true;
            }
            PropVariantClear(&v);
            if (!have) continue;

            for (const auto& h : all)
                if (IsEqualGUID(h.clsid, clsid)) { s_writable.push_back(h); break; }
        }
    });
    return s_writable;
}

// ── A seekable output file ───────────────────────────────
// The zip handler writes local headers, then goes back and patches
// them once sizes are known, so a write-only stream is not enough:
// 7-Zip queries for IOutStream and fails the update without it.
class COutSeekFileStream final : public IOutStream7z
{
public:
    ~COutSeekFileStream() { CloseFile(); }

    bool CreateOutputFile(const std::wstring& path)
    {
        m_handle = CreateFileW(path.c_str(), GENERIC_WRITE | GENERIC_READ,
                               FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        return m_handle != INVALID_HANDLE_VALUE;
    }
    void CloseFile()
    {
        if (m_handle != INVALID_HANDLE_VALUE)
        { CloseHandle(m_handle); m_handle = INVALID_HANDLE_VALUE; }
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ISequentialOutStream7z))
            *ppv = static_cast<ISequentialOutStream7z*>(this);
        else if (IsEqualIID(riid, IID_IOutStream7z))
            *ppv = static_cast<IOutStream7z*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG n = (ULONG)InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP Write(const void* data, UINT32 size, UINT32* processedSize) override
    {
        DWORD written = 0;
        BOOL ok = WriteFile(m_handle, data, size, &written, nullptr);
        if (processedSize) *processedSize = written;
        return ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    STDMETHODIMP Seek(INT64 offset, UINT32 origin, UINT64* newPos) override
    {
        LARGE_INTEGER li; li.QuadPart = offset;
        LARGE_INTEGER out{};
        if (!SetFilePointerEx(m_handle, li, &out, origin))
            return HRESULT_FROM_WIN32(GetLastError());
        if (newPos) *newPos = (UINT64)out.QuadPart;
        return S_OK;
    }
    STDMETHODIMP SetSize(UINT64 newSize) override
    {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)newSize;
        if (!SetFilePointerEx(m_handle, li, nullptr, FILE_BEGIN))
            return HRESULT_FROM_WIN32(GetLastError());
        if (!SetEndOfFile(m_handle))
            return HRESULT_FROM_WIN32(GetLastError());
        return S_OK;
    }

private:
    LONG   m_ref    = 1;
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

// ── The update callback ──────────────────────────────────
class CUpdateCallback final : public IArchiveUpdateCallback7z,
                              public ICryptoGetTextPassword2_7z
{
public:
    CUpdateCallback(const std::vector<ArchiveWriter::Item>& items,
                    std::wstring password, ProgressFn progress)
        : m_items(items), m_password(std::move(password)),
          m_progress(std::move(progress)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_IArchiveUpdateCallback7z))
            *ppv = static_cast<IArchiveUpdateCallback7z*>(this);
        else if (IsEqualIID(riid, IID_ICryptoGetTextPassword2_7z))
            *ppv = static_cast<ICryptoGetTextPassword2_7z*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG n = (ULONG)InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return n;
    }

    // IProgress
    STDMETHODIMP SetTotal(UINT64 total) override
    { m_total = total; return S_OK; }

    STDMETHODIMP SetCompleted(const UINT64* value) override
    {
        if (value && m_total && m_progress)
        {
            int pct = (int)((*value * 100) / m_total);
            if (pct > 100) pct = 100;
            m_progress(pct, m_currentName);
        }
        return m_cancelled ? E_ABORT : S_OK;
    }

    // IArchiveUpdateCallback
    STDMETHODIMP GetUpdateItemInfo(UINT32 /*index*/, INT32* newData,
                                   INT32* newProps, UINT32* indexInArchive) override
    {
        // Creating from nothing: every item is new, and none of them
        // corresponds to an entry in an existing archive.
        if (newData)        *newData  = 1;
        if (newProps)       *newProps = 1;
        if (indexInArchive) *indexInArchive = (UINT32)(INT32)-1;
        return S_OK;
    }

    STDMETHODIMP GetProperty(UINT32 index, PROPID propID, PROPVARIANT* value) override
    {
        if (!value) return E_POINTER;
        PropVariantInit(value);
        if (index >= m_items.size()) return E_INVALIDARG;
        const ArchiveWriter::Item& it = m_items[index];

        switch (propID)
        {
        case k7zPidPath:
            value->vt = VT_BSTR;
            value->bstrVal = SysAllocString(it.nameInArchive.c_str());
            if (!value->bstrVal) { value->vt = VT_EMPTY; return E_OUTOFMEMORY; }
            break;
        case k7zPidIsDir:
            value->vt = VT_BOOL;
            value->boolVal = it.isDir ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        case k7zPidSize:
            value->vt = VT_UI8;
            value->uhVal.QuadPart = it.size;
            break;
        case k7zPidAttrib:
            value->vt  = VT_UI4;
            value->ulVal = it.attrib;
            break;
        case k7zPidMTime:
            value->vt = VT_FILETIME;
            value->filetime = it.mtime;
            break;
        default:
            value->vt = VT_EMPTY;   // "not supplied" — the handler decides
            break;
        }
        return S_OK;
    }

    STDMETHODIMP GetStream(UINT32 index, ISequentialInStream7z** inStream) override
    {
        if (!inStream) return E_POINTER;
        *inStream = nullptr;
        if (index >= m_items.size()) return E_INVALIDARG;
        const ArchiveWriter::Item& it = m_items[index];

        m_currentName = it.nameInArchive;
        if (m_progress) m_progress(-1, m_currentName);

        if (it.isDir) return S_OK;              // directories have no stream

        auto* s = new (std::nothrow) CInFileStream();
        if (!s) return E_OUTOFMEMORY;
        if (!s->OpenFile(it.diskPath))
        {
            s->Release();
            // S_FALSE means "skip this one" — a file we cannot read
            // (locked, vanished, denied) must not abort the whole archive.
            m_skipped.push_back(it.diskPath);
            return S_FALSE;
        }
        *inStream = static_cast<IInStream7z*>(s);
        return S_OK;
    }

    STDMETHODIMP SetOperationResult(INT32 /*operationResult*/) override
    { return S_OK; }

    // ICryptoGetTextPassword2
    STDMETHODIMP CryptoGetTextPassword2(INT32* passwordIsDefined, BSTR* password) override
    {
        const bool have = !m_password.empty();
        if (passwordIsDefined) *passwordIsDefined = have ? 1 : 0;
        if (password)
        {
            *password = SysAllocString(have ? m_password.c_str() : L"");
            if (!*password) return E_OUTOFMEMORY;
        }
        return S_OK;
    }

    const std::vector<std::wstring>& Skipped() const { return m_skipped; }
    void Cancel() { m_cancelled = true; }

private:
    LONG         m_ref = 1;
    const std::vector<ArchiveWriter::Item>& m_items;
    std::wstring m_password;
    ProgressFn   m_progress;
    std::wstring m_currentName;
    UINT64       m_total = 0;
    bool         m_cancelled = false;
    std::vector<std::wstring> m_skipped;
};

} // anonymous namespace

namespace ArchiveWriter {

bool IsAvailable()
{
    return Get7zCreateObjectFunc() != nullptr && !WritableHandlers().empty();
}

std::vector<std::wstring> WritableFormats()
{
    std::vector<std::wstring> out;
    for (const auto& h : WritableHandlers())
        if (!h.name.empty()) out.push_back(h.name);
    return out;
}

bool FormatIsWritable(const std::wstring& format)
{
    for (const auto& h : WritableHandlers())
        if (_wcsicmp(h.name.c_str(), format.c_str()) == 0) return true;
    return false;
}

std::wstring FormatForTargetName(const std::wstring& fileName)
{
    const wchar_t* dot = PathFindExtensionW(fileName.c_str());
    if (!dot || !*dot) return L"";
    const std::wstring bare = dot + 1;

    for (const auto& h : WritableHandlers())
        for (const auto& e : h.exts)
            if (_wcsicmp(e.c_str(), bare.c_str()) == 0) return h.name;
    return L"";
}

std::wstring DefaultExtensionFor(const std::wstring& format)
{
    for (const auto& h : WritableHandlers())
        if (_wcsicmp(h.name.c_str(), format.c_str()) == 0 && !h.exts.empty())
            return L"." + h.exts.front();
    return L"." + format;
}

// ── Gathering ────────────────────────────────────────────
namespace {

void WalkInto(const std::wstring& dirPath, const std::wstring& prefix,
              std::vector<Item>& out, uint64_t* totalBytes)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dirPath + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do
    {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        // Reparse points are not followed: a junction would otherwise
        // pull an unrelated tree in, or loop.
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        const std::wstring child  = dirPath + L"\\" + fd.cFileName;
        const std::wstring stored = prefix + L"\\" + fd.cFileName;

        Item it;
        it.diskPath      = child;
        it.nameInArchive = stored;
        it.isDir         = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        it.attrib        = fd.dwFileAttributes;
        it.mtime         = fd.ftLastWriteTime;
        if (!it.isDir)
        {
            it.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (totalBytes) *totalBytes += it.size;
        }
        out.push_back(std::move(it));

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            WalkInto(child, stored, out, totalBytes);
    }
    while (FindNextFileW(h, &fd));

    FindClose(h);
}

} // anonymous namespace

std::vector<Item> CollectItems(const std::vector<std::wstring>& paths,
                               uint64_t* totalBytes)
{
    std::vector<Item> out;
    if (totalBytes) *totalBytes = 0;

    for (const auto& p : paths)
    {
        if (p.empty()) continue;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) continue;

        std::wstring leaf = PathFindFileNameW(p.c_str());
        if (leaf.empty()) continue;

        Item it;
        it.diskPath      = p;
        it.nameInArchive = leaf;
        it.isDir         = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        it.attrib        = fad.dwFileAttributes;
        it.mtime         = fad.ftLastWriteTime;
        if (!it.isDir)
        {
            it.size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            if (totalBytes) *totalBytes += it.size;
        }
        out.push_back(std::move(it));

        if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            WalkInto(p, leaf, out, totalBytes);
    }
    return out;
}

std::wstring SuggestOutputPath(const std::vector<std::wstring>& paths,
                               const std::wstring& extension)
{
    if (paths.empty()) return L"";

    std::wstring dir = paths.front();
    PathRemoveFileSpecW(&dir[0]);
    dir.resize(wcslen(dir.c_str()));

    std::wstring base;
    if (paths.size() == 1)
    {
        base = PathFindFileNameW(paths.front().c_str());
        // Strip the extension of a file, but keep a folder's name whole:
        // "notes.txt" -> "notes", "my.folder" -> "my.folder".
        const DWORD attr = GetFileAttributesW(paths.front().c_str());
        const bool isDir = attr != INVALID_FILE_ATTRIBUTES &&
                           (attr & FILE_ATTRIBUTE_DIRECTORY);
        if (!isDir)
        {
            size_t dot = base.rfind(L'.');
            if (dot != std::wstring::npos && dot > 0) base.resize(dot);
        }
    }
    else
    {
        // Several items: name the archive after the folder holding them.
        base = PathFindFileNameW(dir.c_str());
        if (base.empty()) base = L"Archive";
    }

    std::wstring candidate = dir + L"\\" + base + extension;
    for (int n = 2; PathFileExistsW(candidate.c_str()) && n < 1000; ++n)
        candidate = dir + L"\\" + base + L" (" + std::to_wstring(n) + L")" + extension;
    return candidate;
}

// ── Doing it ─────────────────────────────────────────────
bool Compress(const std::wstring& outPath,
              const std::vector<Item>& items,
              const Options& opt,
              ProgressFn progress,
              std::wstring* error)
{
    auto fail = [&](const std::wstring& msg) { if (error) *error = msg; return false; };

    if (items.empty()) return fail(L"Nothing to compress.");

    Func7z_CreateObject createObj = Get7zCreateObjectFunc();
    if (!createObj)
        return fail(L"7z.dll could not be loaded, so ArchiveFldr cannot create "
                    L"archives.\n\n" + ThirdParty::DescribeSearch(L"7z"));

    const Handler7z* handler = nullptr;
    for (const auto& h : WritableHandlers())
        if (_wcsicmp(h.name.c_str(), opt.format.c_str()) == 0) { handler = &h; break; }

    if (!handler)
    {
        std::wstring msg = L"This copy of 7z.dll cannot write \"" + opt.format +
                           L"\" archives.\n\nFormats it can write: ";
        const auto fmts = WritableFormats();
        for (size_t i = 0; i < fmts.size(); ++i)
            msg += (i ? L", " : L"") + fmts[i];
        if (fmts.empty()) msg += L"(none)";
        return fail(msg);
    }

    Microsoft::WRL::ComPtr<IOutArchive7z> outArc;
    HRESULT hr = createObj(&handler->clsid, &IID_IOutArchive7z, (void**)outArc.GetAddressOf());
    if (FAILED(hr) || !outArc)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"0x%08X", (unsigned)hr);
        return fail(L"7z.dll refused to create a writer for \"" + opt.format +
                    L"\" (" + buf + L").");
    }

    // ── Compression settings ─────────────────────────────
    // Only names the target format accepts: one unknown name fails the
    // whole SetProperties call, and then nothing would be configurable.
    {
        std::vector<std::wstring> names;
        std::vector<PROPVARIANT>  values;
        auto addUInt = [&](const wchar_t* n, UINT32 v)
        {
            names.emplace_back(n);
            PROPVARIANT pv; PropVariantInit(&pv);
            pv.vt = VT_UI4; pv.ulVal = v;
            values.push_back(pv);
        };
        auto addBool = [&](const wchar_t* n, bool v)
        {
            names.emplace_back(n);
            PROPVARIANT pv; PropVariantInit(&pv);
            pv.vt = VT_BOOL; pv.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
            values.push_back(pv);
        };

        const bool is7z = _wcsicmp(handler->name.c_str(), L"7z") == 0;
        const bool isZip = _wcsicmp(handler->name.c_str(), L"zip") == 0;

        int level = opt.level;
        if (level < 0) level = 0;
        if (level > 9) level = 9;
        addUInt(L"x", (UINT32)level);

        if (is7z)
        {
            addBool(L"s", opt.solid);
            if (!opt.password.empty() && opt.encryptNames)
                addBool(L"he", true);
        }
        if (opt.threads > 0 && (is7z || isZip ||
            _wcsicmp(handler->name.c_str(), L"xz") == 0 ||
            _wcsicmp(handler->name.c_str(), L"bzip2") == 0))
        {
            addUInt(L"mt", (UINT32)opt.threads);
        }

        Microsoft::WRL::ComPtr<ISetProperties7z> setProps;
        if (SUCCEEDED(outArc->QueryInterface(IID_ISetProperties7z,
                                             (void**)setProps.GetAddressOf())) && setProps)
        {
            std::vector<const wchar_t*> namePtrs;
            namePtrs.reserve(names.size());
            for (const auto& n : names) namePtrs.push_back(n.c_str());
            // A rejected setting is not worth failing the archive over:
            // the default would still produce a valid file.
            setProps->SetProperties(namePtrs.data(), values.data(),
                                    (UINT32)namePtrs.size());
        }
        for (auto& pv : values) PropVariantClear(&pv);
    }

    // ── Write to a temporary, then move into place ───────
    std::wstring tempPath = outPath + L".part";
    for (int n = 2; PathFileExistsW(tempPath.c_str()) && n < 100; ++n)
        tempPath = outPath + L".part" + std::to_wstring(n);

    auto* outStream = new (std::nothrow) COutSeekFileStream();
    if (!outStream) return fail(L"Out of memory.");
    if (!outStream->CreateOutputFile(tempPath))
    {
        const DWORD err = GetLastError();
        outStream->Release();
        wchar_t buf[64]; swprintf_s(buf, L"%u", err);
        return fail(L"Could not create \"" + tempPath + L"\" (error " + buf + L").");
    }

    auto* callback = new (std::nothrow) CUpdateCallback(items, opt.password, progress);
    if (!callback) { outStream->Release(); return fail(L"Out of memory."); }

    hr = outArc->UpdateItems(static_cast<ISequentialOutStream7z*>(outStream),
                             (UINT32)items.size(), callback);

    const std::vector<std::wstring> skipped = callback->Skipped();
    callback->Release();
    outStream->CloseFile();
    outStream->Release();

    if (FAILED(hr))
    {
        DeleteFileW(tempPath.c_str());
        if (hr == E_ABORT) return fail(L"Cancelled.");
        wchar_t buf[64]; swprintf_s(buf, L"0x%08X", (unsigned)hr);
        return fail(L"Compression failed (" + std::wstring(buf) + L").");
    }

    DeleteFileW(outPath.c_str());
    if (!MoveFileW(tempPath.c_str(), outPath.c_str()))
    {
        const DWORD err = GetLastError();
        DeleteFileW(tempPath.c_str());
        wchar_t buf[64]; swprintf_s(buf, L"%u", err);
        return fail(L"The archive was built but could not be moved into place "
                    L"(error " + std::wstring(buf) + L").");
    }

    if (!skipped.empty() && error)
    {
        *error = L"Finished, but " + std::to_wstring(skipped.size()) +
                 L" file(s) could not be read and were left out:\n\n";
        for (size_t i = 0; i < skipped.size() && i < 10; ++i)
            *error += L"  " + skipped[i] + L"\n";
        if (skipped.size() > 10) *error += L"  ...\n";
    }
    return true;
}

} // namespace ArchiveWriter

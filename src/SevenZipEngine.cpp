// SevenZipEngine.cpp
// Real .7z support via an external, user-supplied 7-Zip engine DLL.
//
// We never ship or statically link any 7-Zip decoder code. At runtime we
// LoadLibrary() a bitness-matched engine DLL (thirdparty\7z\7z.64.dll or
// thirdparty\7z\7z.32.dll, placed next to ShellNSE.64.dll / ShellNSE.32.dll)
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
#include "Sdk7z.h"

// ═════════════════════════════════════════════════════════
// Engine DLL discovery / loading
// ═════════════════════════════════════════════════════════
namespace {

HMODULE             g_hLib          = nullptr;
Func7z_CreateObject g_pCreateObject = nullptr;
std::wstring        g_enginePath;
std::once_flag      g_initOnce;

std::wstring GetModuleDir()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_hDllInstance, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring path(buf, n);
    size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"" : path.substr(0, slash);
}

const wchar_t* PickBitnessDllName()
{
#if defined(_WIN64)
    return L"7z.64.dll";
#else
    return L"7z.32.dll";
#endif
}

bool FileExistsW(const std::wstring& p)
{
    DWORD attr = GetFileAttributesW(p.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// A 7-Zip install registers its folder under SOFTWARE\7-Zip. Check both
// registry views and both hives: a 32-bit ShellNSE must not be redirected to
// the WOW6432Node copy when only a 64-bit 7-Zip is present, and per-user
// installs land in HKCU.
std::wstring Try7zInstallPath(HKEY root, DWORD viewFlag)
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(root, L"SOFTWARE\\7-Zip", 0,
                      KEY_READ | viewFlag, &hKey) != ERROR_SUCCESS)
        return L"";

    wchar_t val[MAX_PATH] = {};
    DWORD sz = sizeof(val);
    DWORD type = 0;
    LSTATUS st = RegQueryValueExW(hKey, L"Path", nullptr, &type, (LPBYTE)val, &sz);
    RegCloseKey(hKey);

    if (st != ERROR_SUCCESS || type != REG_SZ || !val[0]) return L"";

    std::wstring p = val;
    if (p.back() != L'\\') p += L'\\';
    p += L"7z.dll";
    return FileExistsW(p) ? p : std::wstring();
}

std::wstring Resolve7zDllPath()
{
    std::wstring dir = GetModuleDir();
    if (!dir.empty())
    {
        // Look in every place a user plausibly drops the engine DLL, not
        // just thirdparty\7z\ — "I put 7z.64.dll next to the DLL and nothing
        // happened" was by far the most common way this silently failed.
        const wchar_t* const kSubDirs[] = {
            L"\\thirdparty\\7z\\",
            L"\\7z\\",
            L"\\",
        };
        // Bitness-suffixed name first, then a plain 7z.dll (what you get by
        // copying it straight out of an existing 7-Zip installation).
        const wchar_t* const kNames[] = { PickBitnessDllName(), L"7z.dll" };

        for (const wchar_t* sub : kSubDirs)
            for (const wchar_t* name : kNames)
            {
                std::wstring candidate = dir + sub + name;
                if (FileExistsW(candidate)) return candidate;
            }
    }

    // Last resort: a system-wide (or per-user) 7-Zip installation.
    const struct { HKEY root; DWORD view; } kHives[] = {
        { HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY },
        { HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY },
        { HKEY_CURRENT_USER,  KEY_WOW64_64KEY },
        { HKEY_CURRENT_USER,  KEY_WOW64_32KEY },
    };
    for (const auto& h : kHives)
    {
        std::wstring p = Try7zInstallPath(h.root, h.view);
        if (!p.empty()) return p;
    }
    return L"";
}

void InitEngineOnce()
{
    g_enginePath = Resolve7zDllPath();
    if (g_enginePath.empty()) return;

    // LOAD_WITH_ALTERED_SEARCH_PATH so the engine resolves its own
    // dependencies from the folder it lives in, not from ours.
    g_hLib = LoadLibraryExW(g_enginePath.c_str(), nullptr,
                            LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_hLib) { g_enginePath.clear(); return; }

    g_pCreateObject = reinterpret_cast<Func7z_CreateObject>(
        GetProcAddress(g_hLib, "CreateObject"));
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
                      L"thirdparty\\7z\\ next to ShellNSE.";
        return false;
    }

    auto* fsRaw = new CInFileStream();
    if (!fsRaw->OpenFile(path))
    {
        delete fsRaw;
        m_lastError = L"Cannot open archive file for reading.";
        return false;
    }
    ComPtr<IInStream7z> inStream;
    inStream.Attach(fsRaw);

    void* rawArchive = nullptr;
    HRESULT hr = createObj(&CLSID_CFormat7z, &IID_IInArchive7z, &rawArchive);
    if (FAILED(hr) || !rawArchive)
    {
        m_lastError = L"7-Zip engine failed to create a 7z archive handler.";
        return false;
    }
    ComPtr<IInArchive7z> archive;
    archive.Attach(static_cast<IInArchive7z*>(rawArchive));

    ComPtr<IArchiveOpenCallback7z> openCb;
    openCb.Attach(new CArchiveOpenCallback());

    UINT64 maxCheckStartPosition = 1 << 20; // tolerate SFX stubs etc.
    hr = archive->Open(inStream.Get(), &maxCheckStartPosition, openCb.Get());
    if (FAILED(hr))
    {
        m_lastError = L"Not a valid 7z archive (or its headers are encrypted — "
                      L"password-protected headers are not yet supported).";
        return false;
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
            if (path.empty()) continue;
            size_t slash = path.rfind(L'/');
            std::wstring parent = (slash == std::wstring::npos) ? L"" : path.substr(0, slash + 1);
            if (!parent.empty()) EnsureSyntheticDir(m_allEntries, known, parent);

            ArchiveEntry e;
            e.isDirectory        = false;
            e.engineIndex        = (int64_t)i;
            e.fullPath           = path;
            e.name               = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
            e.uncompressedSize   = PropGetUInt64(m_archive.Get(), i, k7zPidSize, 0);
            e.compressedSize     = PropGetUInt64(m_archive.Get(), i, k7zPidPackSize, 0);
            e.crc32              = (uint32_t)PropGetUInt64(m_archive.Get(), i, k7zPidCRC, 0);
            e.modifiedTime       = PropGetFileTime(m_archive.Get(), i, k7zPidMTime);
            e.isEncrypted        = PropGetBool(m_archive.Get(), i, k7zPidEncrypted, false);
            e.compressionMethod  = L"7z";
            m_allEntries.push_back(std::move(e));
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

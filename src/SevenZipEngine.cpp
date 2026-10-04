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
// Passwords: the engine holds one (SetPassword) and hands it to 7z.dll
// through ICryptoGetTextPassword whenever the handler asks — opening an
// archive with encrypted headers, extracting or testing encrypted
// items, and reading old items back during an update. The UI layers
// (shell view, context menu, data object) detect PasswordNeededToOpen /
// LastErrorWasWrongPassword and prompt, then retry.
#include "stdafx.h"
#include "SevenZipEngine.h"
#include "ArchiveSecurity.h"
#include "Formats.h"
#include "Sdk7z.h"
#include "ThirdParty.h"
#include "LizardFrame.h"
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

// Explorer can create more than one shell-folder/engine object for the same
// open view (enumeration, context menu and IDataObject are not guaranteed to
// use one COM object). Keep a verified password for that archive in process
// memory so those sibling objects do not immediately forget it. The key also
// contains the file stamp/size, so replacing an archive at the same path does
// not inherit the old file's password.
std::mutex                              g_passwordMutex;
std::unordered_map<std::wstring, std::wstring> g_passwords;

std::wstring PasswordCacheKey(const std::wstring& path)
{
    std::wstring key = path;
    wchar_t full[32768] = {};
    const DWORD n = GetFullPathNameW(path.c_str(), ARRAYSIZE(full), full, nullptr);
    if (n && n < ARRAYSIZE(full)) key.assign(full, n);
    for (auto& ch : key) ch = (wchar_t)towlower(ch);

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
    {
        wchar_t stamp[80] = {};
        swprintf_s(stamp, ARRAYSIZE(stamp), L"|%08X%08X|%08X%08X",
                   fad.ftLastWriteTime.dwHighDateTime,
                   fad.ftLastWriteTime.dwLowDateTime,
                   fad.nFileSizeHigh, fad.nFileSizeLow);
        key += stamp;
    }
    return key;
}

std::wstring RecallPassword(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(g_passwordMutex);
    const auto it = g_passwords.find(PasswordCacheKey(path));
    return it == g_passwords.end() ? std::wstring() : it->second;
}

void RememberPassword(const std::wstring& path, const std::wstring& password)
{
    if (password.empty()) return;
    std::lock_guard<std::mutex> lock(g_passwordMutex);
    g_passwords[PasswordCacheKey(path)] = password;
}

void ForgetPassword(const std::wstring& path, const std::wstring& password)
{
    if (password.empty()) return;
    std::lock_guard<std::mutex> lock(g_passwordMutex);
    const auto it = g_passwords.find(PasswordCacheKey(path));
    if (it != g_passwords.end() && it->second == password)
        g_passwords.erase(it);
}

// ── External 7-Zip codec catalogue ──────────────────────────────────────
//
// 7z.dll contains archive handlers, but outside methods must be supplied by
// the host through ICompressCodecsInfo. ArchiveFldr publishes both adapters
// for its existing raw codec runtimes and genuine plug-ins discovered in the
// engine's adjacent Codecs folder. Without that catalogue a valid ZSTD+7zAES
// archive lists normally and extraction fails with kUnsupportedMethod (1).
struct ExternalCodecModule
{
    HMODULE                     module = nullptr;
    Func7z_GetMethodProperty    getProperty = nullptr;
    Func7z_CreateCoder          createDecoder = nullptr;
    Func7z_CreateCoder          createEncoder = nullptr;
    Func7z_CreateObject         createObject = nullptr;
    Func7z_SetCodecs            setCodecs = nullptr;
};

// The ordinary libzstd DLL already used for standalone .zst files can also
// satisfy the ZSTD coder requested from inside a 7z container. It is a C API,
// not a 7-Zip plug-in, so this small COM adapter presents it as ICompressCoder.
struct NativeZstdApi
{
    HMODULE module = nullptr;
    std::wstring path;
    void*  (__cdecl* createDStream)() = nullptr;
    size_t (__cdecl* initDStream)(void*) = nullptr;
    size_t (__cdecl* decompressStream)(void*, void*, void*) = nullptr;
    size_t (__cdecl* freeDStream)(void*) = nullptr;
    void*  (__cdecl* createCStream)() = nullptr;
    size_t (__cdecl* initCStream)(void*, int) = nullptr;
    size_t (__cdecl* compressStream)(void*, void*, void*) = nullptr;
    size_t (__cdecl* endStream)(void*, void*) = nullptr;
    size_t (__cdecl* freeCStream)(void*) = nullptr;
    size_t (__cdecl* setPledgedSrcSize)(void*, unsigned long long) = nullptr;
    unsigned (__cdecl* isError)(size_t) = nullptr;
};

struct NativeZstdInBuffer  { const void* src; size_t size; size_t pos; };
struct NativeZstdOutBuffer { void* dst; size_t size; size_t pos; };

NativeZstdApi g_nativeZstd;
std::once_flag g_nativeZstdOnce;

void InitNativeZstdOnce()
{
    g_nativeZstd.module = ThirdParty::LoadComponent(L"zstd", &g_nativeZstd.path);
    if (!g_nativeZstd.module) return;

#define BIND_ZSTD(member, name) \
    g_nativeZstd.member = reinterpret_cast<decltype(g_nativeZstd.member)>( \
        GetProcAddress(g_nativeZstd.module, name))
    BIND_ZSTD(createDStream, "ZSTD_createDStream");
    BIND_ZSTD(initDStream, "ZSTD_initDStream");
    BIND_ZSTD(decompressStream, "ZSTD_decompressStream");
    BIND_ZSTD(freeDStream, "ZSTD_freeDStream");
    BIND_ZSTD(createCStream, "ZSTD_createCStream");
    BIND_ZSTD(initCStream, "ZSTD_initCStream");
    BIND_ZSTD(compressStream, "ZSTD_compressStream");
    BIND_ZSTD(endStream, "ZSTD_endStream");
    BIND_ZSTD(freeCStream, "ZSTD_freeCStream");
    BIND_ZSTD(setPledgedSrcSize, "ZSTD_CCtx_setPledgedSrcSize");
    BIND_ZSTD(isError, "ZSTD_isError");
#undef BIND_ZSTD

    if (!g_nativeZstd.createDStream || !g_nativeZstd.initDStream ||
        !g_nativeZstd.decompressStream || !g_nativeZstd.freeDStream ||
        !g_nativeZstd.isError)
    {
        // Keep the module loaded for process lifetime, just as CodecEngine
        // does, but don't publish a decoder backed by an incomplete ABI.
        g_nativeZstd.createDStream = nullptr;
    }
}

bool NativeZstdAvailable()
{
    std::call_once(g_nativeZstdOnce, InitNativeZstdOnce);
    return g_nativeZstd.createDStream != nullptr;
}

bool NativeZstdEncoderAvailable()
{
    std::call_once(g_nativeZstdOnce, InitNativeZstdOnce);
    return g_nativeZstd.createCStream && g_nativeZstd.initCStream &&
           g_nativeZstd.compressStream && g_nativeZstd.endStream &&
           g_nativeZstd.freeCStream && g_nativeZstd.isError;
}

class CNativeZstdDecoder final :
    public ICompressCoder7z,
    public ICompressSetDecoderProperties2_7z
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ICompressCoder7z))
            *ppv = static_cast<ICompressCoder7z*>(this);
        else if (IsEqualIID(riid, IID_ICompressSetDecoderProperties2_7z))
            *ppv = static_cast<ICompressSetDecoderProperties2_7z*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override
        { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG n = (ULONG)InterlockedDecrement(&m_ref);
        if (!n) delete this;
        return n;
    }

    STDMETHODIMP SetDecoderProperties2(const BYTE* data, UINT32 size) override
    {
        // 7-Zip ZS used 3/5 informational bytes; newer versions use one flag
        // byte. libzstd gets everything needed from the frame itself.
        if (size != 0 && !data) return E_INVALIDARG;
        return (size == 0 || size == 1 || size == 3 || size == 5)
            ? S_OK : E_NOTIMPL;
    }

    STDMETHODIMP Code(ISequentialInStream7z* inStream,
                       ISequentialOutStream7z* outStream,
                       const UINT64* inSize, const UINT64* outSize,
                       ICompressProgressInfo7z* progress) override
    {
        if (!inStream || !outStream || !NativeZstdAvailable()) return E_INVALIDARG;
        void* stream = g_nativeZstd.createDStream();
        if (!stream) return E_OUTOFMEMORY;

        HRESULT hr = S_OK;
        try
        {
            const size_t init = g_nativeZstd.initDStream(stream);
            hr = g_nativeZstd.isError(init) ? E_FAIL
                : Decode(stream, inStream, outStream, inSize, outSize, progress);
        }
        catch (const std::bad_alloc&) { hr = E_OUTOFMEMORY; }
        catch (...) { hr = E_FAIL; }
        g_nativeZstd.freeDStream(stream);
        return hr;
    }

private:
    static HRESULT Decode(void* stream,
                          ISequentialInStream7z* inStream,
                          ISequentialOutStream7z* outStream,
                          const UINT64* inSize, const UINT64* outSize,
                          ICompressProgressInfo7z* progress)
    {
        constexpr size_t kBufferSize = 256 * 1024;
        std::vector<BYTE> input(kBufferSize), output(kBufferSize);
        UINT64 totalIn = 0, totalOut = 0;
        size_t zstdResult = 1; // non-zero means a frame still needs data
        bool sawInput = false;

        for (;;)
        {
            UINT32 want = (UINT32)input.size();
            if (inSize)
            {
                if (totalIn >= *inSize) want = 0;
                else if (*inSize - totalIn < want)
                    want = (UINT32)(*inSize - totalIn);
            }

            UINT32 got = 0;
            HRESULT readHr = want ? inStream->Read(input.data(), want, &got) : S_FALSE;
            if (FAILED(readHr)) return readHr;
            if (got > want) return E_FAIL;
            if (!got) break;
            sawInput = true;
            totalIn += got;

            NativeZstdInBuffer zin{ input.data(), got, 0 };
            while (zin.pos < zin.size)
            {
                size_t outCapacity = output.size();
                if (outSize)
                {
                    if (totalOut >= *outSize) outCapacity = 0;
                    else if (*outSize - totalOut < outCapacity)
                        outCapacity = (size_t)(*outSize - totalOut);
                }

                NativeZstdOutBuffer zout{ output.data(), outCapacity, 0 };
                const size_t oldInPos = zin.pos;
                zstdResult = g_nativeZstd.decompressStream(stream, &zout, &zin);
                if (g_nativeZstd.isError(zstdResult)) return S_FALSE;
                if (zout.pos > outCapacity) return E_FAIL;

                // A solid/multithreaded stream can contain concatenated or
                // skippable frames. Reset explicitly for older libzstd ABIs.
                if (zstdResult == 0)
                {
                    const size_t reset = g_nativeZstd.initDStream(stream);
                    if (g_nativeZstd.isError(reset)) return E_FAIL;
                }

                size_t writtenTotal = 0;
                while (writtenTotal < zout.pos)
                {
                    UINT32 written = 0;
                    const UINT32 amount = (UINT32)(zout.pos - writtenTotal);
                    const HRESULT writeHr = outStream->Write(
                        output.data() + writtenTotal, amount, &written);
                    if (FAILED(writeHr)) return writeHr;
                    if (!written || written > amount) return E_FAIL;
                    writtenTotal += written;
                    totalOut += written;
                }

                if (zin.pos == oldInPos && zout.pos == 0)
                    return S_FALSE; // output limit reached or no forward progress
            }

            if (progress)
            {
                HRESULT progressHr = progress->SetRatioInfo(&totalIn, &totalOut);
                if (FAILED(progressHr)) return progressHr;
            }
        }

        if (!sawInput || zstdResult != 0) return S_FALSE;
        if (inSize && totalIn != *inSize) return S_FALSE;
        if (outSize && totalOut != *outSize) return S_FALSE;
        return S_OK;
    }

    LONG m_ref = 1;
};

class CNativeZstdEncoder final :
    public ICompressCoder7z,
    public ICompressSetCoderProperties7z,
    public ICompressWriteCoderProperties7z
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ICompressCoder7z))
            *ppv = static_cast<ICompressCoder7z*>(this);
        else if (IsEqualIID(riid, IID_ICompressSetCoderProperties7z))
            *ppv = static_cast<ICompressSetCoderProperties7z*>(this);
        else if (IsEqualIID(riid, IID_ICompressWriteCoderProperties7z))
            *ppv = static_cast<ICompressWriteCoderProperties7z*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override
        { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG n = (ULONG)InterlockedDecrement(&m_ref);
        if (!n) delete this;
        return n;
    }

    STDMETHODIMP SetCoderProperties(const PROPID* ids,
                                     const PROPVARIANT* values,
                                     UINT32 count) override
    {
        if ((!ids || !values) && count) return E_INVALIDARG;
        for (UINT32 i = 0; i < count; ++i)
            if (ids[i] == 15 /* NCoderPropID::kLevel */ &&
                values[i].vt == VT_UI4)
                m_level = (int)values[i].ulVal;
        if (m_level < 1) m_level = 1;
        if (m_level > 22) m_level = 22;
        return S_OK;
    }

    STDMETHODIMP WriteCoderProperties(ISequentialOutStream7z* out) override
    {
        if (!out) return E_POINTER;
        const BYTE props[5] = { 1, 5, (BYTE)m_level, 0, 0 };
        UINT32 written = 0;
        const HRESULT hr = out->Write(props, ARRAYSIZE(props), &written);
        return FAILED(hr) ? hr : (written == ARRAYSIZE(props) ? S_OK : E_FAIL);
    }

    STDMETHODIMP Code(ISequentialInStream7z* inStream,
                       ISequentialOutStream7z* outStream,
                       const UINT64* inSize, const UINT64*,
                       ICompressProgressInfo7z* progress) override
    {
        if (!inStream || !outStream || !NativeZstdEncoderAvailable())
            return E_INVALIDARG;
        void* stream = g_nativeZstd.createCStream();
        if (!stream) return E_OUTOFMEMORY;
        HRESULT hr = S_OK;
        try
        {
            const size_t init = g_nativeZstd.initCStream(stream, m_level);
            if (g_nativeZstd.isError(init)) hr = E_FAIL;
            else if (inSize && g_nativeZstd.setPledgedSrcSize &&
                     g_nativeZstd.isError(
                         g_nativeZstd.setPledgedSrcSize(stream, *inSize)))
                hr = E_FAIL;
            else
                hr = Encode(stream, inStream, outStream, inSize, progress);
        }
        catch (const std::bad_alloc&) { hr = E_OUTOFMEMORY; }
        catch (...) { hr = E_FAIL; }
        g_nativeZstd.freeCStream(stream);
        return hr;
    }

private:
    static HRESULT WriteOutput(ISequentialOutStream7z* out,
                               const BYTE* data, size_t size)
    {
        size_t done = 0;
        while (done < size)
        {
            UINT32 written = 0;
            const UINT32 want = (UINT32)std::min<size_t>(
                size - done, std::numeric_limits<UINT32>::max());
            const HRESULT hr = out->Write(data + done, want, &written);
            if (FAILED(hr)) return hr;
            if (!written || written > want) return E_FAIL;
            done += written;
        }
        return S_OK;
    }

    static HRESULT Encode(void* stream,
                          ISequentialInStream7z* in,
                          ISequentialOutStream7z* out,
                          const UINT64* inSize,
                          ICompressProgressInfo7z* progress)
    {
        constexpr size_t kBufferSize = 256 * 1024;
        std::vector<BYTE> input(kBufferSize), output(kBufferSize);
        UINT64 totalIn = 0, totalOut = 0;
        for (;;)
        {
            UINT32 want = (UINT32)input.size();
            if (inSize && *inSize - totalIn < want)
                want = (UINT32)(*inSize - totalIn);
            UINT32 got = 0;
            const HRESULT readHr = want ? in->Read(input.data(), want, &got) : S_FALSE;
            if (FAILED(readHr)) return readHr;
            if (!got) break;
            totalIn += got;

            NativeZstdInBuffer zin{ input.data(), got, 0 };
            while (zin.pos < zin.size)
            {
                NativeZstdOutBuffer zout{ output.data(), output.size(), 0 };
                const size_t rc = g_nativeZstd.compressStream(stream, &zout, &zin);
                if (g_nativeZstd.isError(rc)) return E_FAIL;
                const HRESULT writeHr = WriteOutput(out, output.data(), zout.pos);
                if (FAILED(writeHr)) return writeHr;
                totalOut += zout.pos;
            }
            if (progress)
            {
                const HRESULT progressHr = progress->SetRatioInfo(&totalIn, &totalOut);
                if (FAILED(progressHr)) return progressHr;
            }
        }

        for (;;)
        {
            NativeZstdOutBuffer zout{ output.data(), output.size(), 0 };
            const size_t remaining = g_nativeZstd.endStream(stream, &zout);
            if (g_nativeZstd.isError(remaining)) return E_FAIL;
            const HRESULT writeHr = WriteOutput(out, output.data(), zout.pos);
            if (FAILED(writeHr)) return writeHr;
            totalOut += zout.pos;
            if (remaining == 0) break;
        }
        return (!inSize || totalIn == *inSize) ? S_OK : S_FALSE;
    }

    LONG m_ref = 1;
    int  m_level = 5;
};

enum class NativeCodecKind { None, Zstd, Brotli, Lz4, Lz5, Lizard };

// Binary layout from lizard_frame.h.  Keep this local instead of including
// a deployment-specific SDK: ArchiveFldr loads the requested DLL at runtime.
struct NativeLizardFrameInfo
{
    int blockSizeID;
    int blockMode;
    int contentChecksumFlag;
    int frameType;
    unsigned long long contentSize;
    unsigned reserved[2];
};
struct NativeLizardPreferences
{
    NativeLizardFrameInfo frameInfo;
    int compressionLevel;
    unsigned autoFlush;
    unsigned reserved[4];
};
static_assert(sizeof(NativeLizardFrameInfo) == 32,
              "Lizard frame-info ABI layout changed");
static_assert(sizeof(NativeLizardPreferences) == 56,
              "Lizard preferences ABI layout changed");

struct NativeBrotliApi
{
    HMODULE decoderModule = nullptr, encoderModule = nullptr;
    std::wstring path;
    void* (__cdecl* createDecoder)(void*, void*, void*) = nullptr;
    void  (__cdecl* destroyDecoder)(void*) = nullptr;
    int   (__cdecl* decodeStream)(void*, size_t*, const BYTE**,
                                  size_t*, BYTE**, size_t*) = nullptr;
    void* (__cdecl* createEncoder)(void*, void*, void*) = nullptr;
    void  (__cdecl* destroyEncoder)(void*) = nullptr;
    int   (__cdecl* setEncoderParameter)(void*, int, UINT32) = nullptr;
    int   (__cdecl* encodeStream)(void*, int, size_t*, const BYTE**,
                                  size_t*, BYTE**, size_t*) = nullptr;
    int   (__cdecl* encoderFinished)(void*) = nullptr;
};

NativeBrotliApi g_nativeBrotli;
std::once_flag g_nativeBrotliOnce;

void InitNativeBrotliOnce()
{
    g_nativeBrotli.decoderModule =
        ThirdParty::LoadComponent(L"brotli", &g_nativeBrotli.path);
    if (!g_nativeBrotli.decoderModule) return;
    g_nativeBrotli.encoderModule = GetModuleHandleW(L"libbrotlienc.dll");
    if (!g_nativeBrotli.encoderModule)
        g_nativeBrotli.encoderModule = GetModuleHandleW(L"brotlienc.dll");

#define BIND_BROTLI(dst, module, name) \
    g_nativeBrotli.dst = module ? reinterpret_cast<decltype(g_nativeBrotli.dst)>( \
        GetProcAddress(module, name)) : nullptr
    BIND_BROTLI(createDecoder, g_nativeBrotli.decoderModule,
                "BrotliDecoderCreateInstance");
    BIND_BROTLI(destroyDecoder, g_nativeBrotli.decoderModule,
                "BrotliDecoderDestroyInstance");
    BIND_BROTLI(decodeStream, g_nativeBrotli.decoderModule,
                "BrotliDecoderDecompressStream");
    BIND_BROTLI(createEncoder, g_nativeBrotli.encoderModule,
                "BrotliEncoderCreateInstance");
    BIND_BROTLI(destroyEncoder, g_nativeBrotli.encoderModule,
                "BrotliEncoderDestroyInstance");
    BIND_BROTLI(setEncoderParameter, g_nativeBrotli.encoderModule,
                "BrotliEncoderSetParameter");
    BIND_BROTLI(encodeStream, g_nativeBrotli.encoderModule,
                "BrotliEncoderCompressStream");
    BIND_BROTLI(encoderFinished, g_nativeBrotli.encoderModule,
                "BrotliEncoderIsFinished");
#undef BIND_BROTLI
}

bool NativeBrotliDecoderAvailable()
{
    std::call_once(g_nativeBrotliOnce, InitNativeBrotliOnce);
    return g_nativeBrotli.createDecoder && g_nativeBrotli.destroyDecoder &&
           g_nativeBrotli.decodeStream;
}

bool NativeBrotliEncoderAvailable()
{
    std::call_once(g_nativeBrotliOnce, InitNativeBrotliOnce);
    return g_nativeBrotli.createEncoder && g_nativeBrotli.destroyEncoder &&
           g_nativeBrotli.encodeStream && g_nativeBrotli.encoderFinished;
}

struct NativeLzFrameApi
{
    HMODULE module = nullptr;
    std::wstring path;
    size_t (__cdecl* createDctx)(void**, unsigned) = nullptr;
    size_t (__cdecl* freeDctx)(void*) = nullptr;
    size_t (__cdecl* decompress)(void*, void*, size_t*,
                                 const void*, size_t*, const void*) = nullptr;
    size_t (__cdecl* createCctx)(void**, unsigned) = nullptr;
    size_t (__cdecl* freeCctx)(void*) = nullptr;
    size_t (__cdecl* compressBegin)(void*, void*, size_t, const void*) = nullptr;
    size_t (__cdecl* compressUpdate)(void*, void*, size_t,
                                     const void*, size_t, const void*) = nullptr;
    size_t (__cdecl* compressEnd)(void*, void*, size_t, const void*) = nullptr;
    size_t (__cdecl* compressBound)(size_t, const void*) = nullptr;
    unsigned (__cdecl* isError)(size_t) = nullptr;
};

NativeLzFrameApi g_nativeLz4, g_nativeLz5, g_nativeLizard;
std::once_flag g_nativeLz4Once, g_nativeLz5Once, g_nativeLizardOnce;

void InitNativeLzFrame(NativeLzFrameApi& api, const wchar_t* component,
                       const char* prefix, const char* fallbackPrefix = nullptr)
{
    api.module = ThirdParty::LoadComponent(component, &api.path);
    if (!api.module) return;
    auto bind = [&](auto& dst, const char* suffix)
    {
        const std::string name = std::string(prefix) + suffix;
        dst = reinterpret_cast<std::decay_t<decltype(dst)>>(
            GetProcAddress(api.module, name.c_str()));
        if (!dst && fallbackPrefix)
        {
            const std::string fallback = std::string(fallbackPrefix) + suffix;
            dst = reinterpret_cast<std::decay_t<decltype(dst)>>(
                GetProcAddress(api.module, fallback.c_str()));
        }
    };
    bind(api.createDctx, "createDecompressionContext");
    bind(api.freeDctx, "freeDecompressionContext");
    bind(api.decompress, "decompress");
    bind(api.createCctx, "createCompressionContext");
    bind(api.freeCctx, "freeCompressionContext");
    bind(api.compressBegin, "compressBegin");
    bind(api.compressUpdate, "compressUpdate");
    bind(api.compressEnd, "compressEnd");
    bind(api.compressBound, "compressBound");
    bind(api.isError, "isError");
}

NativeLzFrameApi* GetNativeLzFrame(NativeCodecKind kind, bool encoder)
{
    NativeLzFrameApi* api = nullptr;
    if (kind == NativeCodecKind::Lz4)
    {
        std::call_once(g_nativeLz4Once, [] {
            InitNativeLzFrame(g_nativeLz4, L"lz4", "LZ4F_"); });
        api = &g_nativeLz4;
    }
    else if (kind == NativeCodecKind::Lz5)
    {
        std::call_once(g_nativeLz5Once, [] {
            InitNativeLzFrame(g_nativeLz5, L"lz5", "LZ5F_"); });
        api = &g_nativeLz5;
    }
    else if (kind == NativeCodecKind::Lizard)
    {
        std::call_once(g_nativeLizardOnce, [] {
            InitNativeLzFrame(g_nativeLizard, L"lizard", "LizardF_", "LZ5F_"); });
        api = &g_nativeLizard;
    }
    if (!api || !api->isError) return nullptr;
    if (encoder)
        return api->createCctx && api->freeCctx && api->compressBegin &&
               api->compressUpdate && api->compressEnd && api->compressBound
            ? api : nullptr;
    return api->createDctx && api->freeDctx && api->decompress ? api : nullptr;
}

class CNativeAuxCoder final :
    public ICompressCoder7z,
    public ICompressSetCoderProperties7z,
    public ICompressSetDecoderProperties2_7z,
    public ICompressWriteCoderProperties7z
{
public:
    CNativeAuxCoder(NativeCodecKind kind, bool encode)
        : m_kind(kind), m_encode(encode),
          m_level(kind == NativeCodecKind::Lizard ? 15 : 5) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ICompressCoder7z))
            *ppv = static_cast<ICompressCoder7z*>(this);
        else if (IsEqualIID(riid, IID_ICompressSetCoderProperties7z))
            *ppv = static_cast<ICompressSetCoderProperties7z*>(this);
        else if (IsEqualIID(riid, IID_ICompressSetDecoderProperties2_7z))
            *ppv = static_cast<ICompressSetDecoderProperties2_7z*>(this);
        else if (IsEqualIID(riid, IID_ICompressWriteCoderProperties7z))
            *ppv = static_cast<ICompressWriteCoderProperties7z*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override
        { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG n = (ULONG)InterlockedDecrement(&m_ref);
        if (!n) delete this;
        return n;
    }

    STDMETHODIMP SetCoderProperties(const PROPID* ids,
                                     const PROPVARIANT* values,
                                     UINT32 count) override
    {
        if ((!ids || !values) && count) return E_INVALIDARG;
        for (UINT32 i = 0; i < count; ++i)
        {
            if (ids[i] == 15 && values[i].vt == VT_UI4)
                m_level = (int)values[i].ulVal;
            else if (ids[i] == 1 || ids[i] == 4)
            {
                // 7-Zip uses DictionarySize (1) for -md and some external
                // handlers forward the same frame choice as BlockSize (4).
                if (values[i].vt == VT_UI4) m_dictionaryBytes = values[i].ulVal;
                else if (values[i].vt == VT_UI8)
                    m_dictionaryBytes = values[i].uhVal.QuadPart;
            }
        }
        if (m_kind == NativeCodecKind::Lizard)
        {
            if (m_level < 10) m_level = 10;
            if (m_level > 49) m_level = 49;
        }
        else
        {
            if (m_level < 1) m_level = 1;
            if (m_level > (m_kind == NativeCodecKind::Brotli ? 11 : 16))
                m_level = m_kind == NativeCodecKind::Brotli ? 11 : 16;
        }
        return S_OK;
    }

    STDMETHODIMP SetDecoderProperties2(const BYTE* data, UINT32 size) override
    {
        if (size && !data) return E_INVALIDARG;
        return (size == 0 || size == 1 || size == 3 || size == 5)
            ? S_OK : E_NOTIMPL;
    }

    STDMETHODIMP WriteCoderProperties(ISequentialOutStream7z* out) override
    {
        if (!out) return E_POINTER;
        BYTE props[5] = { 1, 0, (BYTE)m_level, 0, 0 };
        UINT32 size = 5;
        if (m_kind == NativeCodecKind::Brotli)
        {
            props[1] = 1;
            size = 3;
        }
        else if (m_kind == NativeCodecKind::Lz4) props[1] = 10;
        else if (m_kind == NativeCodecKind::Lz5) props[1] = 5;
        else if (m_kind == NativeCodecKind::Lizard) size = 3;
        UINT32 written = 0;
        const HRESULT hr = out->Write(props, size, &written);
        return FAILED(hr) ? hr : (written == size ? S_OK : E_FAIL);
    }

    STDMETHODIMP Code(ISequentialInStream7z* in,
                       ISequentialOutStream7z* out,
                       const UINT64* inSize, const UINT64* outSize,
                       ICompressProgressInfo7z* progress) override
    {
        if (!in || !out) return E_INVALIDARG;
        try
        {
            if (m_kind == NativeCodecKind::Brotli)
                return m_encode ? EncodeBrotli(in, out, inSize, progress)
                                : DecodeBrotli(in, out, inSize, outSize, progress);
            return m_encode ? EncodeLz(in, out, inSize, progress)
                            : DecodeLz(in, out, inSize, outSize, progress);
        }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (...) { return E_FAIL; }
    }

private:
    static HRESULT WriteAll(ISequentialOutStream7z* out,
                            const BYTE* data, size_t size)
    {
        size_t done = 0;
        while (done < size)
        {
            UINT32 written = 0;
            const UINT32 want = (UINT32)std::min<size_t>(
                size - done, std::numeric_limits<UINT32>::max());
            const HRESULT hr = out->Write(data + done, want, &written);
            if (FAILED(hr)) return hr;
            if (!written || written > want) return E_FAIL;
            done += written;
        }
        return S_OK;
    }

    static UINT32 ReadAmount(const UINT64* size, UINT64 done, size_t capacity)
    {
        if (!size) return (UINT32)capacity;
        if (done >= *size) return 0;
        return (UINT32)std::min<UINT64>(*size - done, capacity);
    }

    HRESULT DecodeBrotli(ISequentialInStream7z* in,
                          ISequentialOutStream7z* out,
                          const UINT64* inSize, const UINT64* outSize,
                          ICompressProgressInfo7z* progress)
    {
        if (!NativeBrotliDecoderAvailable()) return E_NOTIMPL;
        void* state = g_nativeBrotli.createDecoder(nullptr, nullptr, nullptr);
        if (!state) return E_OUTOFMEMORY;
        constexpr size_t kBuf = 256 * 1024;
        std::vector<BYTE> input(kBuf), output(kBuf);
        UINT64 totalIn = 0, totalOut = 0;
        bool finished = false;
        HRESULT result = S_OK;

        while (!finished)
        {
            const UINT32 want = ReadAmount(inSize, totalIn, input.size());
            UINT32 got = 0;
            const HRESULT readHr = want ? in->Read(input.data(), want, &got) : S_FALSE;
            if (FAILED(readHr)) { result = readHr; break; }
            if (!got) { result = S_FALSE; break; }
            totalIn += got;
            size_t availableIn = got;
            const BYTE* nextIn = input.data();
            while (availableIn || !finished)
            {
                size_t availableOut = output.size();
                if (outSize)
                    availableOut = (size_t)std::min<UINT64>(
                        availableOut, totalOut < *outSize ? *outSize - totalOut : 0);
                BYTE* nextOut = output.data();
                size_t producedTotal = 0;
                const int rc = g_nativeBrotli.decodeStream(
                    state, &availableIn, &nextIn,
                    &availableOut, &nextOut, &producedTotal);
                const size_t produced = (size_t)(nextOut - output.data());
                const HRESULT writeHr = WriteAll(out, output.data(), produced);
                if (FAILED(writeHr)) { result = writeHr; finished = true; break; }
                totalOut += produced;
                if (rc == 1) { finished = true; break; }
                if (rc == 0) { result = S_FALSE; finished = true; break; }
                if (rc == 2) break;       // needs more input
                if (rc == 3 && !produced) { result = S_FALSE; finished = true; break; }
            }
            if (progress && SUCCEEDED(result))
            {
                const HRESULT hr = progress->SetRatioInfo(&totalIn, &totalOut);
                if (FAILED(hr)) { result = hr; break; }
            }
        }
        g_nativeBrotli.destroyDecoder(state);
        if (FAILED(result) || result == S_FALSE) return result;
        if ((inSize && totalIn != *inSize) || (outSize && totalOut != *outSize))
            return S_FALSE;
        return S_OK;
    }

    HRESULT EncodeBrotli(ISequentialInStream7z* in,
                          ISequentialOutStream7z* out,
                          const UINT64* inSize,
                          ICompressProgressInfo7z* progress)
    {
        if (!NativeBrotliEncoderAvailable()) return E_NOTIMPL;
        void* state = g_nativeBrotli.createEncoder(nullptr, nullptr, nullptr);
        if (!state) return E_OUTOFMEMORY;
        if (g_nativeBrotli.setEncoderParameter)
            g_nativeBrotli.setEncoderParameter(state, 1 /* quality */, (UINT32)m_level);
        constexpr size_t kBuf = 256 * 1024;
        std::vector<BYTE> input(kBuf), output(kBuf);
        UINT64 totalIn = 0, totalOut = 0;
        HRESULT result = S_OK;
        bool eof = false;
        while (!g_nativeBrotli.encoderFinished(state))
        {
            UINT32 got = 0;
            if (!eof)
            {
                const UINT32 want = ReadAmount(inSize, totalIn, input.size());
                const HRESULT readHr = want ? in->Read(input.data(), want, &got) : S_FALSE;
                if (FAILED(readHr)) { result = readHr; break; }
                eof = got == 0;
                totalIn += got;
            }
            size_t availableIn = got;
            const BYTE* nextIn = input.data();
            do
            {
                size_t availableOut = output.size();
                BYTE* nextOut = output.data();
                size_t producedTotal = 0;
                const int op = eof ? 2 /* FINISH */ : 0 /* PROCESS */;
                if (!g_nativeBrotli.encodeStream(state, op,
                        &availableIn, &nextIn, &availableOut, &nextOut,
                        &producedTotal))
                { result = E_FAIL; break; }
                const size_t produced = (size_t)(nextOut - output.data());
                const HRESULT writeHr = WriteAll(out, output.data(), produced);
                if (FAILED(writeHr)) { result = writeHr; break; }
                totalOut += produced;
                if (!produced && !availableIn && eof &&
                    !g_nativeBrotli.encoderFinished(state))
                { result = E_FAIL; break; }
            } while (availableIn || (eof && !g_nativeBrotli.encoderFinished(state)));
            if (FAILED(result)) break;
            if (progress)
            {
                const HRESULT hr = progress->SetRatioInfo(&totalIn, &totalOut);
                if (FAILED(hr)) { result = hr; break; }
            }
        }
        g_nativeBrotli.destroyEncoder(state);
        if (FAILED(result)) return result;
        return (!inSize || totalIn == *inSize) ? S_OK : S_FALSE;
    }

    static HRESULT LizardResultToHresult(LizardFrame::Result result)
    {
        switch (result)
        {
        case LizardFrame::Result::Ok:          return S_OK;
        case LizardFrame::Result::Unavailable: return E_NOTIMPL;
        case LizardFrame::Result::OutOfMemory: return E_OUTOFMEMORY;
        case LizardFrame::Result::Cancelled:   return E_ABORT;
        case LizardFrame::Result::InvalidData: return S_FALSE;
        default:                               return E_FAIL;
        }
    }

    HRESULT DecodeLizardRaw(ISequentialInStream7z* in,
                             ISequentialOutStream7z* out,
                             const UINT64* inSize, const UINT64* outSize,
                             ICompressProgressInfo7z* progress)
    {
        UINT64 totalIn = 0, totalOut = 0;
        HRESULT ioFailure = S_OK;
        const LizardFrame::Result result = LizardFrame::Decode(
            [&](void* buffer, size_t capacity, size_t* got) {
                if (capacity > std::numeric_limits<UINT32>::max())
                    return false;
                UINT32 amount = 0;
                const HRESULT hr = in->Read(buffer, (UINT32)capacity, &amount);
                if (FAILED(hr)) { ioFailure = hr; return false; }
                *got = amount;
                totalIn += amount;
                return true;
            },
            [&](const void* buffer, size_t size) {
                const HRESULT hr = WriteAll(out,
                    static_cast<const BYTE*>(buffer), size);
                if (FAILED(hr)) { ioFailure = hr; return false; }
                totalOut += size;
                return true;
            },
            outSize ? *outSize : 0,
            [&](uint64_t, uint64_t) {
                if (!progress) return true;
                const HRESULT hr = progress->SetRatioInfo(&totalIn, &totalOut);
                if (FAILED(hr)) { ioFailure = hr; return false; }
                return true;
            });
        if (FAILED(ioFailure)) return ioFailure;
        if (result != LizardFrame::Result::Ok)
            return LizardResultToHresult(result);
        if ((inSize && totalIn != *inSize) ||
            (outSize && totalOut != *outSize)) return S_FALSE;
        return S_OK;
    }

    HRESULT EncodeLizardRaw(ISequentialInStream7z* in,
                             ISequentialOutStream7z* out,
                             const UINT64* inSize,
                             ICompressProgressInfo7z* progress)
    {
        UINT64 totalIn = 0, totalOut = 0;
        HRESULT ioFailure = S_OK;
        const LizardFrame::Result result = LizardFrame::Encode(
            [&](void* buffer, size_t capacity, size_t* got) {
                if (capacity > std::numeric_limits<UINT32>::max())
                    return false;
                UINT32 amount = 0;
                const HRESULT hr = in->Read(buffer, (UINT32)capacity, &amount);
                if (FAILED(hr)) { ioFailure = hr; return false; }
                *got = amount;
                totalIn += amount;
                return true;
            },
            [&](const void* buffer, size_t size) {
                const HRESULT hr = WriteAll(out,
                    static_cast<const BYTE*>(buffer), size);
                if (FAILED(hr)) { ioFailure = hr; return false; }
                totalOut += size;
                return true;
            },
            inSize ? *inSize : 0, m_level, m_dictionaryBytes,
            [&](uint64_t, uint64_t) {
                if (!progress) return true;
                const HRESULT hr = progress->SetRatioInfo(&totalIn, &totalOut);
                if (FAILED(hr)) { ioFailure = hr; return false; }
                return true;
            });
        if (FAILED(ioFailure)) return ioFailure;
        if (result != LizardFrame::Result::Ok)
            return LizardResultToHresult(result);
        return (!inSize || totalIn == *inSize) ? S_OK : S_FALSE;
    }

    HRESULT DecodeLz(ISequentialInStream7z* in,
                      ISequentialOutStream7z* out,
                      const UINT64* inSize, const UINT64* outSize,
                      ICompressProgressInfo7z* progress)
    {
        NativeLzFrameApi* api = GetNativeLzFrame(m_kind, false);
        if (!api)
            return m_kind == NativeCodecKind::Lizard &&
                   LizardFrame::DecoderAvailable()
                ? DecodeLizardRaw(in, out, inSize, outSize, progress)
                : E_NOTIMPL;
        void* ctx = nullptr;
        size_t rc = api->createDctx(&ctx, 100);
        if (api->isError(rc) || !ctx) return E_FAIL;
        constexpr size_t kBuf = 256 * 1024;
        std::vector<BYTE> input(kBuf), output(kBuf);
        UINT64 totalIn = 0, totalOut = 0;
        size_t hint = 1;
        HRESULT result = S_OK;
        for (;;)
        {
            const UINT32 want = ReadAmount(inSize, totalIn, input.size());
            UINT32 got = 0;
            const HRESULT readHr = want ? in->Read(input.data(), want, &got) : S_FALSE;
            if (FAILED(readHr)) { result = readHr; break; }
            if (!got) break;
            totalIn += got;
            size_t pos = 0;
            while (pos < got)
            {
                size_t srcSize = got - pos;
                size_t dstSize = output.size();
                if (outSize)
                    dstSize = (size_t)std::min<UINT64>(
                        dstSize, totalOut < *outSize ? *outSize - totalOut : 0);
                hint = api->decompress(ctx, output.data(), &dstSize,
                                       input.data() + pos, &srcSize, nullptr);
                if (api->isError(hint) || (!srcSize && !dstSize))
                { result = S_FALSE; break; }
                pos += srcSize;
                const HRESULT writeHr = WriteAll(out, output.data(), dstSize);
                if (FAILED(writeHr)) { result = writeHr; break; }
                totalOut += dstSize;
            }
            if (FAILED(result) || result == S_FALSE) break;
            if (progress)
            {
                const HRESULT hr = progress->SetRatioInfo(&totalIn, &totalOut);
                if (FAILED(hr)) { result = hr; break; }
            }
        }
        api->freeDctx(ctx);
        if (FAILED(result) || result == S_FALSE) return result;
        if (hint != 0 || (inSize && totalIn != *inSize) ||
            (outSize && totalOut != *outSize)) return S_FALSE;
        return S_OK;
    }

    static int LizardBlockSizeId(UINT64 bytes)
    {
        if (!bytes) return 0; // library default
        if (bytes <= (128ull << 10)) return 1;
        if (bytes <= (256ull << 10)) return 2;
        if (bytes <= (1ull << 20)) return 3;
        if (bytes <= (4ull << 20)) return 4;
        if (bytes <= (16ull << 20)) return 5;
        if (bytes <= (64ull << 20)) return 6;
        return 7; // 256 MB, the largest Lizard frame block
    }

    HRESULT EncodeLz(ISequentialInStream7z* in,
                      ISequentialOutStream7z* out,
                      const UINT64* inSize,
                      ICompressProgressInfo7z* progress)
    {
        NativeLzFrameApi* api = GetNativeLzFrame(m_kind, true);
        if (!api)
            return m_kind == NativeCodecKind::Lizard &&
                   LizardFrame::EncoderAvailable()
                ? EncodeLizardRaw(in, out, inSize, progress)
                : E_NOTIMPL;
        void* ctx = nullptr;
        size_t rc = api->createCctx(&ctx, 100);
        if (api->isError(rc) || !ctx) return E_FAIL;

        NativeLizardPreferences lizardPrefs{};
        const void* preferences = nullptr;
        if (m_kind == NativeCodecKind::Lizard)
        {
            lizardPrefs.frameInfo.blockSizeID =
                LizardBlockSizeId(m_dictionaryBytes);
            lizardPrefs.frameInfo.contentSize = inSize ? *inSize : 0;
            lizardPrefs.compressionLevel = m_level;
            preferences = &lizardPrefs;
        }

        constexpr size_t kBuf = 256 * 1024;
        const size_t bound = api->compressBound(kBuf, preferences);
        if (api->isError(bound)) { api->freeCctx(ctx); return E_FAIL; }
        std::vector<BYTE> input(kBuf), output(std::max(kBuf * 2, bound));
        UINT64 totalIn = 0, totalOut = 0;
        HRESULT result = S_OK;

        size_t made = api->compressBegin(ctx, output.data(), output.size(), preferences);
        if (api->isError(made)) result = E_FAIL;
        else { result = WriteAll(out, output.data(), made); totalOut += made; }
        while (SUCCEEDED(result))
        {
            const UINT32 want = ReadAmount(inSize, totalIn, input.size());
            UINT32 got = 0;
            const HRESULT readHr = want ? in->Read(input.data(), want, &got) : S_FALSE;
            if (FAILED(readHr)) { result = readHr; break; }
            if (!got) break;
            totalIn += got;
            made = api->compressUpdate(ctx, output.data(), output.size(),
                                       input.data(), got, nullptr);
            if (api->isError(made)) { result = E_FAIL; break; }
            result = WriteAll(out, output.data(), made);
            totalOut += made;
            if (progress && SUCCEEDED(result))
            {
                const HRESULT hr = progress->SetRatioInfo(&totalIn, &totalOut);
                if (FAILED(hr)) { result = hr; break; }
            }
        }
        if (SUCCEEDED(result))
        {
            made = api->compressEnd(ctx, output.data(), output.size(), nullptr);
            result = api->isError(made) ? E_FAIL : WriteAll(out, output.data(), made);
        }
        api->freeCctx(ctx);
        if (FAILED(result)) return result;
        return (!inSize || totalIn == *inSize) ? S_OK : S_FALSE;
    }

    LONG m_ref = 1;
    NativeCodecKind m_kind;
    bool m_encode;
    int m_level = 5;
    UINT64 m_dictionaryBytes = 0;
};

struct ExternalCodecMethod
{
    size_t moduleIndex = 0;
    UINT32 methodIndex = 0;
    UINT64 methodId = 0;
    GUID   decoder{};
    GUID   encoder{};
    bool   hasDecoder = false;
    bool   hasEncoder = false;
    NativeCodecKind nativeKind = NativeCodecKind::None;
};

class CExternalCodecsInfo final : public ICompressCodecsInfo7z
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ICompressCodecsInfo7z))
            *ppv = static_cast<ICompressCodecsInfo7z*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override
        { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG n = (ULONG)InterlockedDecrement(&m_ref);
        if (!n) delete this;
        return n;
    }

    bool AddNativeCodecs()
    {
        auto add = [&](UINT64 id, NativeCodecKind kind,
                       bool decoder, bool encoder)
        {
            if (!decoder && !encoder) return;
            ExternalCodecMethod method;
            method.methodId = id;
            method.hasDecoder = decoder;
            method.hasEncoder = encoder;
            method.nativeKind = kind;
            m_methods.push_back(method);
        };

        // ZSTD has both the long-standing 7-Zip ZS ID and the newer official
        // coder ID. The other IDs are from 7-Zip's external-method registry.
        // Publish the official ID first so ZIP's method-name lookup chooses
        // its standardized ZSTD method; keep the ZS ID for older 7z archives.
        add(0x04015DULL, NativeCodecKind::Zstd,
            NativeZstdAvailable(), NativeZstdEncoderAvailable());
        add(0x04F71101ULL, NativeCodecKind::Zstd,
            NativeZstdAvailable(), NativeZstdEncoderAvailable());
        add(0x04F71102ULL, NativeCodecKind::Brotli,
            NativeBrotliDecoderAvailable(), NativeBrotliEncoderAvailable());
        add(0x04F71104ULL, NativeCodecKind::Lz4,
            GetNativeLzFrame(NativeCodecKind::Lz4, false) != nullptr,
            GetNativeLzFrame(NativeCodecKind::Lz4, true) != nullptr);
        add(0x04F71105ULL, NativeCodecKind::Lz5,
            GetNativeLzFrame(NativeCodecKind::Lz5, false) != nullptr,
            GetNativeLzFrame(NativeCodecKind::Lz5, true) != nullptr);
        add(0x04F71106ULL, NativeCodecKind::Lizard,
            GetNativeLzFrame(NativeCodecKind::Lizard, false) != nullptr ||
                LizardFrame::DecoderAvailable(),
            GetNativeLzFrame(NativeCodecKind::Lizard, true) != nullptr ||
                LizardFrame::EncoderAvailable());
        return !m_methods.empty();
    }

    STDMETHODIMP GetNumMethods(UINT32* numMethods) override
    {
        if (!numMethods) return E_POINTER;
        *numMethods = (UINT32)m_methods.size();
        return S_OK;
    }

    STDMETHODIMP GetProperty(UINT32 index, PROPID propID,
                              PROPVARIANT* value) override
    {
        if (!value) return E_POINTER;
        PropVariantInit(value);
        if (index >= m_methods.size()) return E_INVALIDARG;
        const ExternalCodecMethod& method = m_methods[index];

        if (method.nativeKind != NativeCodecKind::None)
        {
            if (propID == k7zMethodID)
            {
                value->vt = VT_UI8;
                value->uhVal.QuadPart = method.methodId;
                return S_OK;
            }
            if (propID == k7zMethodName)
            {
                const wchar_t* name = method.nativeKind == NativeCodecKind::Zstd ? L"ZSTD" :
                    method.nativeKind == NativeCodecKind::Brotli ? L"BROTLI" :
                    method.nativeKind == NativeCodecKind::Lz4 ? L"LZ4" :
                    method.nativeKind == NativeCodecKind::Lz5 ? L"LZ5" : L"LIZARD";
                value->bstrVal = SysAllocString(name);
                if (!value->bstrVal) return E_OUTOFMEMORY;
                value->vt = VT_BSTR;
                return S_OK;
            }
            if ((propID == k7zMethodDecoder && method.hasDecoder) ||
                (propID == k7zMethodEncoder && method.hasEncoder))
            {
                GUID clsid{};
                clsid.Data1 = 0x23170F69;
                clsid.Data2 = 0x40C1;
                clsid.Data3 = propID == k7zMethodDecoder
                    ? 0x2790 : 0x2791; // decoder / encoder class
                memcpy(clsid.Data4, &method.methodId, sizeof(clsid.Data4));
                value->bstrVal = SysAllocStringByteLen(
                    reinterpret_cast<const char*>(&clsid), sizeof(clsid));
                if (!value->bstrVal) return E_OUTOFMEMORY;
                value->vt = VT_BSTR;
                return S_OK;
            }
            if (propID == k7zMethodIsFilter)
            {
                value->vt = VT_BOOL;
                value->boolVal = VARIANT_FALSE;
                return S_OK;
            }
            // All other native-method properties are intentionally VT_EMPTY.
        }

        // Older plug-ins only publish the decoder/encoder CLSIDs. Newer
        // archive handlers ask these synthesized boolean properties first.
        if (propID == k7zMethodDecoderIsAssigned ||
            propID == k7zMethodEncoderIsAssigned)
        {
            value->vt = VT_BOOL;
            value->boolVal =
                ((propID == k7zMethodDecoderIsAssigned)
                    ? method.hasDecoder : method.hasEncoder)
                ? VARIANT_TRUE : VARIANT_FALSE;
            return S_OK;
        }

        if (method.nativeKind != NativeCodecKind::None) return S_OK;

        const ExternalCodecModule& module = m_modules[method.moduleIndex];
        return module.getProperty
            ? module.getProperty(method.methodIndex, propID, value)
            : E_NOTIMPL;
    }

    STDMETHODIMP CreateDecoder(UINT32 index, const GUID* iid,
                                void** coder) override
    {
        return CreateCoder(index, iid, coder, false);
    }

    STDMETHODIMP CreateEncoder(UINT32 index, const GUID* iid,
                                void** coder) override
    {
        return CreateCoder(index, iid, coder, true);
    }

    bool LoadFolder(const std::wstring& folder)
    {
        const size_t oldMethodCount = m_methods.size();
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((folder + L"\\*.dll").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) return false;
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                LoadModule(folder + L"\\" + fd.cFileName);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        return m_methods.size() > oldMethodCount;
    }

    size_t MethodCount() const { return m_methods.size(); }

    void ConnectModules()
    {
        // Codec plug-ins can themselves depend on another external method.
        for (const auto& module : m_modules)
            if (module.setCodecs) module.setCodecs(this);
    }

private:
    ~CExternalCodecsInfo() = default; // modules intentionally live for process lifetime

    static bool GetClass(Func7z_GetMethodProperty getProperty, UINT32 index,
                         PROPID propID, GUID& clsid)
    {
        PROPVARIANT value; PropVariantInit(&value);
        const HRESULT hr = getProperty(index, propID, &value);
        const bool ok = hr == S_OK && value.vt == VT_BSTR && value.bstrVal &&
                        SysStringByteLen(value.bstrVal) == sizeof(GUID);
        if (ok) memcpy(&clsid, value.bstrVal, sizeof(GUID));
        PropVariantClear(&value);
        return ok;
    }

    static bool GetMethodId(Func7z_GetMethodProperty getProperty,
                            UINT32 index, UINT64& methodId)
    {
        PROPVARIANT value; PropVariantInit(&value);
        const HRESULT hr = getProperty(index, k7zMethodID, &value);
        bool ok = hr == S_OK;
        if (value.vt == VT_UI8) methodId = value.uhVal.QuadPart;
        else if (value.vt == VT_UI4) methodId = value.ulVal;
        else ok = false;
        PropVariantClear(&value);
        return ok;
    }

    bool HasMethodId(UINT64 methodId) const
    {
        for (const auto& method : m_methods)
            if (method.methodId == methodId) return true;
        return false;
    }

    void LoadModule(const std::wstring& path)
    {
        HMODULE moduleHandle = LoadLibraryExW(
            path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!moduleHandle) return; // includes opposite-bitness plug-ins

        ExternalCodecModule module;
        module.module = moduleHandle;
        module.getProperty = reinterpret_cast<Func7z_GetMethodProperty>(
            GetProcAddress(moduleHandle, "GetMethodProperty"));
        if (!module.getProperty)
        {
            FreeLibrary(moduleHandle); // ordinary dependency DLL, not a codec
            return;
        }

        module.createDecoder = reinterpret_cast<Func7z_CreateCoder>(
            GetProcAddress(moduleHandle, "CreateDecoder"));
        module.createEncoder = reinterpret_cast<Func7z_CreateCoder>(
            GetProcAddress(moduleHandle, "CreateEncoder"));
        module.createObject = reinterpret_cast<Func7z_CreateObject>(
            GetProcAddress(moduleHandle, "CreateObject"));
        module.setCodecs = reinterpret_cast<Func7z_SetCodecs>(
            GetProcAddress(moduleHandle, "SetCodecs"));

        UINT32 count = 1; // old codec plug-ins exported exactly one method
        if (auto getCount = reinterpret_cast<Func7z_GetNumberOfMethods>(
                GetProcAddress(moduleHandle, "GetNumberOfMethods")))
        {
            if (getCount(&count) != S_OK || count == 0 || count > 4096)
            {
                FreeLibrary(moduleHandle);
                return;
            }
        }

        const size_t moduleIndex = m_modules.size();
        const size_t oldMethodCount = m_methods.size();
        m_modules.push_back(module);
        for (UINT32 i = 0; i < count; ++i)
        {
            ExternalCodecMethod method;
            method.moduleIndex = moduleIndex;
            method.methodIndex = i;
            const bool hasId = GetMethodId(module.getProperty, i, method.methodId);
            if (hasId && HasMethodId(method.methodId))
                continue; // prefer the already-published native decoder
            method.hasDecoder = GetClass(module.getProperty, i,
                                         k7zMethodDecoder, method.decoder);
            method.hasEncoder = GetClass(module.getProperty, i,
                                         k7zMethodEncoder, method.encoder);
            if (method.hasDecoder || method.hasEncoder)
                m_methods.push_back(method);
        }

        if (m_methods.size() == oldMethodCount)
        {
            m_modules.pop_back();
            FreeLibrary(moduleHandle);
        }
    }

    HRESULT CreateCoder(UINT32 index, const GUID* iid, void** coder,
                        bool encode)
    {
        if (!coder) return E_POINTER;
        *coder = nullptr;
        if (!iid || index >= m_methods.size()) return E_INVALIDARG;

        const ExternalCodecMethod& method = m_methods[index];
        const bool assigned = encode ? method.hasEncoder : method.hasDecoder;
        if (!assigned) return S_OK;

        if (method.nativeKind != NativeCodecKind::None)
        {
            if (!IsEqualIID(*iid, IID_ICompressCoder7z)) return E_NOINTERFACE;
            ICompressCoder7z* native = nullptr;
            if (method.nativeKind == NativeCodecKind::Zstd)
                native = encode
                    ? static_cast<ICompressCoder7z*>(new(std::nothrow) CNativeZstdEncoder())
                    : static_cast<ICompressCoder7z*>(new(std::nothrow) CNativeZstdDecoder());
            else
                native = new(std::nothrow) CNativeAuxCoder(method.nativeKind, encode);
            if (!native) return E_OUTOFMEMORY;
            *coder = native;
            return S_OK;
        }

        const ExternalCodecModule& module = m_modules[method.moduleIndex];
        Func7z_CreateCoder direct =
            encode ? module.createEncoder : module.createDecoder;
        if (direct) return direct(method.methodIndex, iid, coder);
        if (module.createObject)
            return module.createObject(
                encode ? &method.encoder : &method.decoder, iid, coder);
        return E_NOTIMPL;
    }

    LONG                             m_ref = 1;
    std::vector<ExternalCodecModule> m_modules;
    std::vector<ExternalCodecMethod> m_methods;
};

CExternalCodecsInfo* g_externalCodecs = nullptr; // process-lifetime object
std::once_flag       g_externalCodecsOnce;
std::wstring         g_externalCodecsFolder;

void InitExternalCodecsOnce()
{
    if (g_enginePath.empty() || !g_hLib) return;

    std::wstring folder = g_enginePath;
    PathRemoveFileSpecW(&folder[0]);
    folder.resize(wcslen(folder.c_str()));
    if (!folder.empty() && folder.back() != L'\\') folder += L'\\';
    folder += L"Codecs";

    auto* codecs = new(std::nothrow) CExternalCodecsInfo();
    if (!codecs) return;
    const bool nativeCodecs = codecs->AddNativeCodecs();
    const bool pluginCodecs = codecs->LoadFolder(folder);
    if (!nativeCodecs && !pluginCodecs)
    {
        codecs->Release();
        return;
    }

    g_externalCodecs = codecs; // keep its original ref for process lifetime
    if (pluginCodecs) g_externalCodecsFolder = folder;
    codecs->ConnectModules();

    // 7-Zip 15+ accepts the catalogue once at module scope. Older/custom
    // builds expose only ISetCompressCodecsInfo on each archive object; the
    // handler creation path below also covers that contract.
    if (auto setCodecs = reinterpret_cast<Func7z_SetCodecs>(
            GetProcAddress(g_hLib, "SetCodecs")))
        setCodecs(codecs);
}

ICompressCodecsInfo7z* GetExternalCodecs()
{
    std::call_once(g_externalCodecsOnce, InitExternalCodecsOnce);
    return g_externalCodecs;
}

size_t ExternalCodecMethodCount()
{
    auto* codecs = static_cast<CExternalCodecsInfo*>(GetExternalCodecs());
    return codecs ? codecs->MethodCount() : 0;
}

void AttachExternalCodecs(IUnknown* object)
{
    ICompressCodecsInfo7z* codecs = GetExternalCodecs();
    if (!object || !codecs) return;

    ComPtr<ISetCompressCodecsInfo7z> setter;
    if (object->QueryInterface(IID_ISetCompressCodecsInfo7z,
                               (void**)setter.GetAddressOf()) == S_OK && setter)
        setter->SetCompressCodecsInfo(codecs);
}

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
                                    public IArchiveOpenVolumeCallback7z,
                                    public ICryptoGetTextPassword7z
{
public:
    // `password` is handed over when the handler asks for one (encrypted
    // headers). `askedFlag`, when given, is set the moment it asks — that
    // is how the engine learns the open needed a password at all.
    explicit CArchiveOpenCallback(std::wstring password = std::wstring(),
                                  bool* askedFlag = nullptr,
                                  std::wstring archivePath = std::wstring())
        : m_password(std::move(password)), m_asked(askedFlag),
          m_archivePath(std::move(archivePath)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IArchiveOpenCallback7z))
            *ppv = static_cast<IArchiveOpenCallback7z*>(this);
        else if (IsEqualIID(riid, IID_IArchiveOpenVolumeCallback7z))
            *ppv = static_cast<IArchiveOpenVolumeCallback7z*>(this);
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

    STDMETHODIMP GetProperty(PROPID propID, PROPVARIANT* value) override
    {
        if (!value) return E_POINTER;
        PropVariantInit(value);
        if (propID == k7zPidName && !m_archivePath.empty())
        {
            value->bstrVal = SysAllocString(PathFindFileNameW(m_archivePath.c_str()));
            if (!value->bstrVal) return E_OUTOFMEMORY;
            value->vt = VT_BSTR;
        }
        return S_OK;
    }

    STDMETHODIMP GetStream(const wchar_t* name, IInStream7z** stream) override
    {
        if (!stream) return E_POINTER;
        *stream = nullptr;
        if (!name || !*name || m_archivePath.empty()) return S_FALSE;
        std::wstring folder = m_archivePath;
        if (!PathRemoveFileSpecW(&folder[0])) folder.clear();
        else folder.resize(wcslen(folder.c_str()));
        if (!folder.empty() && folder.back() != L'\\') folder += L'\\';
        // Volume names come from the archive handler. Keep lookup in the
        // archive's own directory even if a malformed name contains a path.
        const std::wstring candidate = folder + PathFindFileNameW(name);
        auto* file = new(std::nothrow) CInFileStream();
        if (!file) return E_OUTOFMEMORY;
        if (!file->OpenFile(candidate)) { file->Release(); return S_FALSE; }
        *stream = static_cast<IInStream7z*>(file);
        return S_OK;
    }

    STDMETHODIMP CryptoGetTextPassword(BSTR* password) override
    {
        if (m_asked) *m_asked = true;
        if (!password) return E_POINTER;
        // No password to give: abort the open cleanly rather than let the
        // handler chew on garbage. The engine turns this into
        // PasswordNeededToOpen() and the UI prompts.
        if (m_password.empty()) { *password = nullptr; return E_ABORT; }
        *password = SysAllocString(m_password.c_str());
        return *password ? S_OK : E_OUTOFMEMORY;
    }

private:
    LONG         m_ref = 1;
    std::wstring m_password;
    bool*        m_asked = nullptr;
    std::wstring m_archivePath;
};

// ═════════════════════════════════════════════════════════
// Method names
// ═════════════════════════════════════════════════════════
// kpidMethod is a human-readable coder chain ("LZMA2:24", "BCJ2") only
// for the codecs the loaded 7z.dll knows by name. Anything else — every
// external codec, in particular — comes back as the raw method ID in
// hex, which is what put "04F71101" in the Method column instead of
// "ZSTD". IDs are from DOC/Methods.txt in the 7-Zip source.
struct MethodId { const wchar_t* id; const wchar_t* name; };
static const MethodId kMethodIds[] = {
    { L"00",       L"Copy"       },
    { L"03",       L"Delta"      },
    { L"04",       L"BCJ"        },
    { L"05",       L"PPC"        },
    { L"06",       L"IA64"       },
    { L"07",       L"ARM"        },
    { L"08",       L"ARMT"       },
    { L"09",       L"SPARC"      },
    { L"0A",       L"ARM64"      },
    { L"0B",       L"RISCV"      },
    { L"21",       L"LZMA2"      },
    { L"030101",   L"LZMA"       },
    { L"03030103", L"BCJ"        },
    { L"0303011B", L"BCJ2"       },
    { L"030401",   L"PPMd"       },
    { L"040108",   L"Deflate"    },
    { L"040109",   L"Deflate64"  },
    { L"04015D",   L"ZSTD"       },
    { L"04015F",   L"XZ"         },
    { L"040162",   L"PPMd"       },
    { L"040163",   L"WinZip AES" },
    { L"040202",   L"BZip2"      },
    { L"040301",   L"RAR1"       },
    { L"040302",   L"RAR2"       },
    { L"040303",   L"RAR3"       },
    { L"040305",   L"RAR5"       },
    { L"04F71101", L"ZSTD"       },
    { L"04F71102", L"Brotli"     },
    { L"04F71104", L"LZ4"        },
    { L"04F71105", L"LZ5"        },
    { L"04F71106", L"Lizard"     },
    { L"06F10101", L"ZipCrypto"  },
    { L"06F10303", L"RAR AES-128"},
    { L"06F10701", L"AES-256"    },
};

// A raw ID is an even number of hex digits and nothing else. No codec
// 7-Zip names in text is spellable in hex ("BCJ", "AES", "LZMA" and the
// rest all contain non-hex letters), so there is nothing to collide with.
static bool LooksLikeMethodId(const std::wstring& tok)
{
    if (tok.empty() || tok.size() > 16 || (tok.size() % 2) != 0) return false;
    for (wchar_t c : tok)
    {
        const bool hex = (c >= L'0' && c <= L'9') ||
                         (c >= L'A' && c <= L'F') ||
                         (c >= L'a' && c <= L'f');
        if (!hex) return false;
    }
    return true;
}

static bool MethodUsesEncryption(const std::wstring& method)
{
    // kpidEncrypted is the authoritative answer, but not every handler /
    // 7z.dll version publishes it consistently.  The coder chain is a
    // second, independent signal: encrypted 7z and zip members contain an
    // AES or ZipCrypto coder even when kpidEncrypted arrived as VT_EMPTY.
    std::wstring upper = method;
    for (auto& ch : upper) ch = (wchar_t)towupper(ch);
    return upper.find(L"AES")       != std::wstring::npos ||
           upper.find(L"ZIPCRYPTO") != std::wstring::npos ||
           upper.find(L"06F10101")  != std::wstring::npos ||
           upper.find(L"06F10701")  != std::wstring::npos;
}

// Rewrite the hex members of a coder chain, leaving everything else —
// names, ":24" dictionary suffixes, separators — exactly as reported.
static std::wstring PrettifyMethod(const std::wstring& raw)
{
    if (raw.empty()) return raw;

    std::wstring out;
    size_t pos = 0;
    while (pos <= raw.size())
    {
        size_t sp = raw.find(L' ', pos);
        if (sp == std::wstring::npos) sp = raw.size();
        std::wstring tok = raw.substr(pos, sp - pos);

        if (LooksLikeMethodId(tok))
        {
            std::wstring upper = tok;
            for (auto& c : upper)
                if (c >= L'a' && c <= L'f') c = (wchar_t)(c - (L'a' - L'A'));
            for (const auto& m : kMethodIds)
                if (upper == m.id) { tok = m.name; break; }
        }

        if (!tok.empty())
        {
            if (!out.empty()) out += L' ';
            out += tok;
        }
        if (sp >= raw.size()) break;
        pos = sp + 1;
    }
    return out;
}

// ═════════════════════════════════════════════════════════
// CArchiveExtractCallback
// ═════════════════════════════════════════════════════════
class CArchiveExtractCallback final : public IArchiveExtractCallback7z,
                                       public ICryptoGetTextPassword7z
{
public:
    CArchiveExtractCallback(IInArchive7z* archive, std::wstring destDir,
                             UINT32 totalCount, ProgressFn cb,
                             std::wstring fallbackName = std::wstring(),
                             std::wstring password = std::wstring())
        : m_archive(archive), m_destDir(std::move(destDir)),
          m_total(totalCount ? totalCount : 1), m_cb(std::move(cb)),
          m_fallbackName(std::move(fallbackName)),
          m_password(std::move(password)) {}

    bool HadError() const { return m_hadError; }
    bool PasswordWasRequested() const { return m_passwordRequested; }
    INT32 LastOperationResult() const { return m_lastOperationResult; }
    DWORD LastStreamError() const { return m_lastStreamError; }
    // The failure pattern that means "wrong or missing password" rather
    // than corruption: the handler said so outright, or a password-backed
    // item failed with the data/CRC result older handlers use for this case.
    bool WrongPassword() const { return m_wrongPassword; }

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
        m_curOutSpec            = nullptr;
        m_curIsDir              = false;
        m_curPasswordRequested  = false;
        m_curDiskPath.clear();

        std::wstring path = PropGetString(m_archive, index, k7zPidPath);
        // The payload of a single-stream container has no name of its
        // own. The listing shows it named after the archive; extraction
        // has to agree. Without this the destination path was the output
        // directory itself, CreateFile failed, the item was skipped, and
        // dragging the file out of the folder died as E_FAIL —
        // "Error Copying File or Folder: Unspecified error".
        if (path.empty()) path = m_fallbackName;
        const std::wstring method =
            PropGetString(m_archive, index, k7zPidMethod);
        m_curEncrypted =
            PropGetBool(m_archive, index, k7zPidEncrypted, false) ||
            MethodUsesEncryption(method);
        for (auto& ch : path) if (ch == L'\\') ch = L'/';
        bool isDir = PropGetBool(m_archive, index, k7zPidIsDir, false);
        m_curMTime  = PropGetFileTime(m_archive, index, k7zPidMTime);
        m_curAttrib = (DWORD)PropGetUInt64(m_archive, index, k7zPidAttrib, 0);
        m_curIsDir  = isDir;
        m_curPath   = path;

        if (askExtractMode != N7zExtract::kExtract)
            return S_OK; // test / skip / read-external — no output stream needed

        // Nothing to build a path out of: fail the item rather than writing
        // to the destination directory itself. Record it explicitly because
        // S_FALSE is still a successful HRESULT to COM's SUCCEEDED() macro.
        if (path.empty())
        {
            m_hadError = true;
            m_lastStreamError = ERROR_INVALID_NAME;
            return S_FALSE;
        }

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
            m_hadError = true;
            m_lastStreamError = GetLastError();
            delete outRaw;
            return S_FALSE; // extraction of other items can still continue
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
            m_lastOperationResult = opRes;
            // kWrongPassword is explicit; a data/CRC error on an item that
            // either advertises encryption OR actually asked this callback
            // for a password is the same thing said less clearly.  The
            // latter is essential for handlers that return VT_EMPTY for
            // kpidEncrypted (and for older ZipCrypto handlers).
            if (opRes == N7zExtract::kWrongPassword ||
                ((m_curEncrypted || m_curPasswordRequested) &&
                 (opRes == N7zExtract::kDataError ||
                  opRes == N7zExtract::kCRCError)))
                m_wrongPassword = true;
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

    STDMETHODIMP CryptoGetTextPassword(BSTR* password) override
    {
        if (!password) return E_POINTER;
        m_passwordRequested    = true;
        m_curPasswordRequested = true;

        // For extraction, return an empty BSTR rather than E_ABORT when no
        // password has been supplied yet.  The handler then completes this
        // item with its normal wrong-password/data result, leaving the open
        // archive reusable for the prompted retry.  PasswordWasRequested()
        // still lets the engine distinguish this from corrupt plain data.
        *password = SysAllocString(m_password.c_str());
        return *password ? S_OK : E_OUTOFMEMORY;
    }

private:
    LONG           m_ref = 1;
    IInArchive7z*  m_archive; // not owned
    std::wstring   m_destDir;
    UINT32         m_total;
    UINT32         m_done = 0;
    ProgressFn     m_cb;
    bool           m_hadError = false;
    bool           m_wrongPassword = false;
    INT32          m_lastOperationResult = -1;
    DWORD          m_lastStreamError = ERROR_SUCCESS;
    bool           m_passwordRequested    = false;
    bool           m_curPasswordRequested = false;
    bool           m_curEncrypted         = false;
    std::wstring   m_password;
    // What to call the payload of a single-stream container, which
    // reports no path of its own. Empty for every other archive.
    std::wstring   m_fallbackName;

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

void C7zArchiveEngine::ClearPassword()
{
    ArchiveSecurity::SecureClear(m_password);
}

bool C7zArchiveEngine::Open(const std::wstring& path)
{
    Close();
    m_lastError.clear();
    m_needPasswordToOpen = false;
    m_wrongPassword      = false;
    m_passwordMissing    = false;
    m_filePath = path;
    m_readOnly = true;

    // A sibling Explorer shell object may already have verified this
    // archive's password. Reuse it before asking 7z.dll to open headers.
    if (m_password.empty()) m_password = RecallPassword(path);

    Func7z_CreateObject createObj = Get7zCreateObjectFunc();
    if (!createObj)
    {
        m_lastError = L"7-Zip engine not found. Place 7z.64.dll / 7z.32.dll under "
                      L"thirdparty\\7z\\ next to ArchiveFldr.";
        return false;
    }

    // Register the module-level catalogue before constructing handlers; the
    // per-object attachment below covers older engines as well.
    GetExternalCodecs();

    // Which handler? Start with the ones that claim this extension, then
    // fall back to every other handler, so a .tar that is really a .gz —
    // or a file with no extension at all — still opens. Each attempt gets
    // a fresh stream, because a failed Open leaves the position anywhere.
    LPCWSTR ext = PathFindExtensionW(path.c_str());

    std::vector<const Handler7z*> candidates;
    if (ext && _wcsicmp(ext, L".001") == 0)
    {
        // For our raw split volumes, prefer the handler named by the extension
        // immediately before .001 (archive.7z.001, archive.zip.001, ...).
        std::wstring base = path.substr(0, path.size() - 4);
        candidates = HandlersForExt(PathFindExtensionW(base.c_str()));
        const auto splitHandlers = HandlersForExt(ext);
        for (const Handler7z* h : splitHandlers)
            if (std::find(candidates.begin(), candidates.end(), h) == candidates.end())
                candidates.push_back(h);
    }
    else candidates = HandlersForExt(ext);

    // Optical images commonly carry both an ISO-9660 compatibility tree and
    // a UDF tree. Prefer UDF when it is available: it is the authoritative
    // filesystem on DVD/Blu-ray media and preserves files larger than 4 GiB,
    // Unicode names and the complete directory layout. A plain ISO simply
    // makes the UDF handler return S_FALSE and falls through to ISO-9660.
    if (ext && (_wcsicmp(ext, L".iso") == 0 || _wcsicmp(ext, L".udf") == 0))
    {
        auto udf = std::find_if(candidates.begin(), candidates.end(),
            [](const Handler7z* h) { return _wcsicmp(h->name.c_str(), L"Udf") == 0; });
        if (udf != candidates.end())
            std::rotate(candidates.begin(), udf, udf + 1);
    }

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
    bool                 passwordAsked          = false;
    bool                 passwordVerifiedByOpen = false;

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

        // A raw 7z.dll client must explicitly give archive handlers the
        // methods discovered in the adjacent Codecs folder. This is what
        // lets the 7z handler instantiate external ZSTD/Brotli/LZ4 decoders.
        AttachExternalCodecs(candidate.Get());

        // Keep the signal local to this handler.  A password request means
        // that it recognised encrypted headers; no unrelated fallback
        // handler may subsequently claim the same bytes as an empty archive.
        bool askedThisHandler = false;
        ComPtr<IArchiveOpenCallback7z> openCb;
        openCb.Attach(new CArchiveOpenCallback(
            m_password, &askedThisHandler, path));

        UINT64 maxCheckStartPosition = 1 << 20;   // tolerate SFX stubs etc.
        const HRESULT openHr = candidate->Open(
            inStream.Get(), &maxCheckStartPosition, openCb.Get());

        // IInArchive::Open uses S_FALSE for "not my format".  SUCCEEDED()
        // is therefore wrong here: accepting S_FALSE was the direct cause
        // of encrypted-header .7z files opening through a fallback handler
        // as an apparently valid archive containing zero items.
        if (openHr != S_OK)
        {
            if (askedThisHandler) passwordAsked = true;
            candidate->Close();
            return false;
        }

        archive    = candidate;
        chosenName = name;
        passwordVerifiedByOpen = askedThisHandler && !m_password.empty();
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
        {
            if (tryHandler(h->clsid, h->name)) break;
            // A handler only asks for a header password after recognising
            // its archive.  Stop now; trying permissive fallbacks can only
            // hide the password request or misidentify the encrypted bytes.
            if (passwordAsked) break;
        }
    }

    if (!archive)
    {
        if (passwordAsked)
        {
            // The handler recognised the format but could not read the
            // headers with the password it was (or wasn't) given. This is
            // the "ask the user and try again" case, not a diagnostic one.
            ForgetPassword(path, m_password); // do not seed sibling engines with a bad value
            m_needPasswordToOpen = true;
            m_lastError = m_password.empty()
                ? L"This archive's headers are encrypted: a password is "
                  L"needed even to list the files inside."
                : L"The password did not open this archive.";
            return false;
        }

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

        m_lastError += L"\nThe file may be damaged or incomplete.";
        return false;
    }

    // Prefer the friendly name from the format table over 7-Zip's short
    // handler id, so the UI says "Windows image" rather than "wim".
    {
        std::wstring pretty = Formats::NameFor(ext);
        m_formatName = pretty.empty() ? chosenName : pretty;
    }
    m_handlerName = chosenName;   // the handler id itself ("zip", "7z", ...)

    m_archive = archive;
    m_open    = true;
    BuildEntryList();
    if (passwordVerifiedByOpen)
        RememberPassword(path, m_password);
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
    m_innerName.clear();
    m_handlerName.clear();
    ClearPassword();
    m_open = false;
}

void C7zArchiveEngine::BuildEntryList()
{
    m_allEntries.clear();
    if (!m_archive) return;

    UINT32 numItems = 0;
    m_archive->GetNumberOfItems(&numItems);

    // A lone nameless payload is named after the archive. Decided here,
    // once, so the extract callback can use the same answer.
    m_innerName = (numItems == 1) ? Formats::InnerNameFor(m_filePath)
                                  : std::wstring();

    // Single-stream containers record almost nothing about what they
    // hold: no name, usually no size, often no packed size and no
    // timestamp. What the container itself can answer for, it should.
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    const bool haveStat = GetFileAttributesExW(m_filePath.c_str(),
                                               GetFileExInfoStandard,
                                               &fad) != FALSE;

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
            const bool lonePayload = path.empty();
            if (lonePayload)
            {
                path = m_innerName;              // empty unless numItems == 1
                if (path.empty()) continue;      // genuinely unnamed, skip
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
            const std::wstring rawMethod =
                PropGetString(m_archive.Get(), i, k7zPidMethod);
            e.isEncrypted =
                PropGetBool(m_archive.Get(), i, k7zPidEncrypted, false) ||
                MethodUsesEncryption(rawMethod);
            // kpidMethod is the per-item coder chain ("LZMA2:24", "Copy", …),
            // except for codecs the loaded 7z.dll has no name for, which
            // arrive as a bare hex method ID — hence the lookup.
            e.compressionMethod  = PrettifyMethod(rawMethod);
            // Only .7z used to reach this engine, so an unreported method
            // was labelled "7z". Now that tar, zip, iso and the rest come
            // through here that would be a plain lie — a tar member is
            // stored, not 7z-compressed. Leave it empty and let the view
            // show "Store", which is what an unreported method means. The
            // single-stream containers below are the one exception.

            if (lonePayload)
            {
                // .bz2 and friends hold exactly one stream, compressed
                // with the one codec the format is named after, so an
                // unreported method here is not "stored" — it is BZip2.
                if (e.compressionMethod.empty())
                {
                    const Formats::Format* f =
                        Formats::Find(PathFindExtensionW(m_filePath.c_str()));
                    e.compressionMethod = f ? std::wstring(f->name)
                                            : m_formatName;
                }
                // bzip2 and xz record no original size. A handler that
                // answers "0" for a stream plainly holding data has not
                // reported a size at all, and the view must say so
                // rather than print "0 KB" for a 2 MB file.
                if (e.sizeKnown && e.uncompressedSize == 0 &&
                    e.compressedSize > 0)
                    e.sizeKnown = false;
                // The whole file is the packed stream, so its size on
                // disk is the packed size. Reporting 0 made the view
                // print "0 KB" for a 1.9 MB file.
                if (e.compressedSize == 0 && haveStat)
                    e.compressedSize = ((uint64_t)fad.nFileSizeHigh << 32) |
                                        fad.nFileSizeLow;
                // Likewise the timestamp: gzip keeps one, bzip2 and xz
                // do not, and the container's own is closer to the truth
                // than a blank cell.
                if (!e.modifiedTime.dwLowDateTime &&
                    !e.modifiedTime.dwHighDateTime && haveStat)
                    e.modifiedTime = fad.ftLastWriteTime;
            }

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
    m_wrongPassword   = false;
    m_passwordMissing = false;
    if (!m_open || !m_archive || indices.empty()) return false;
    SHCreateDirectoryExW(nullptr, destDir.c_str(), nullptr);

    auto* cbRaw = new CArchiveExtractCallback(m_archive.Get(), destDir,
                                               (UINT32)indices.size(), cb,
                                               m_innerName, m_password);
    ComPtr<IArchiveExtractCallback7z> extractCb;
    extractCb.Attach(cbRaw);

    const HRESULT hr = m_archive->Extract(
        indices.data(), (UINT32)indices.size(), 0, extractCb.Get());
    // Unlike IInArchive::Open (where S_FALSE specifically means "wrong
    // format"), Extract follows normal COM success semantics. Some handlers
    // return a non-S_OK success status after producing the requested stream;
    // the per-item callback is the authority on actual extraction errors.
    const bool ok = SUCCEEDED(hr) && !cbRaw->HadError();
    if (!ok)
    {
        // A decoder request with an empty engine password is the initial
        // "password required" case. Once one exists, the callback's per-item
        // result distinguishes a wrong password from unrelated corruption.
        m_passwordMissing =
            cbRaw->PasswordWasRequested() && m_password.empty();
        m_wrongPassword = cbRaw->WrongPassword() || m_passwordMissing;
        if (m_wrongPassword && !m_passwordMissing)
            ForgetPassword(m_filePath, m_password);
        if (m_wrongPassword)
        {
            m_lastError = L"One or more items are encrypted and the password is "
                          L"missing or wrong.";
        }
        else if (cbRaw->LastOperationResult() ==
                 N7zExtract::kUnsupportedMethod)
        {
            wchar_t count[32] = {};
            swprintf_s(count, ARRAYSIZE(count), L"%u",
                       (unsigned)ExternalCodecMethodCount());
            m_lastError =
                L"The loaded 7-Zip engine has no decoder for this item's "
                L"compression method.\n\nEngine: " + Get7zEnginePath() +
                L"\nExternal codec methods loaded: " + count;
            if (NativeZstdAvailable())
                m_lastError += L"\nNative ZSTD library: " + g_nativeZstd.path;
            if (NativeBrotliDecoderAvailable())
                m_lastError += L"\nNative Brotli library: " + g_nativeBrotli.path;
            if (GetNativeLzFrame(NativeCodecKind::Lz4, false))
                m_lastError += L"\nNative LZ4 library: " + g_nativeLz4.path;
            if (GetNativeLzFrame(NativeCodecKind::Lz5, false))
                m_lastError += L"\nNative LZ5 library: " + g_nativeLz5.path;
            if (GetNativeLzFrame(NativeCodecKind::Lizard, false) ||
                LizardFrame::DecoderAvailable())
                m_lastError += L"\nNative Lizard library: " +
                    (g_nativeLizard.path.empty() ? LizardFrame::LibraryPath()
                                                 : g_nativeLizard.path);
            if (!g_externalCodecsFolder.empty())
                m_lastError += L"\nCodecs folder: " + g_externalCodecsFolder;
            else if (!NativeZstdAvailable() && !NativeBrotliDecoderAvailable() &&
                     !GetNativeLzFrame(NativeCodecKind::Lz4, false) &&
                     !GetNativeLzFrame(NativeCodecKind::Lz5, false) &&
                     !GetNativeLzFrame(NativeCodecKind::Lizard, false) &&
                     !LizardFrame::DecoderAvailable())
                m_lastError +=
                    L"\nNo compatible native ZSTD/Brotli/LZ4/LZ5/Lizard library "
                    L"or adjacent Codecs folder was found.";
        }
        else
        {
            wchar_t detail[160] = {};
            swprintf_s(detail, ARRAYSIZE(detail),
                       L"Extraction failed (7-Zip HRESULT 0x%08X, "
                       L"item result %d, stream error %lu).",
                       (unsigned)hr, (int)cbRaw->LastOperationResult(),
                       (unsigned long)cbRaw->LastStreamError());
            m_lastError = detail;
        }
    }
    else
    {
        if (cbRaw->PasswordWasRequested())
            RememberPassword(m_filePath, m_password);
        m_lastError.clear();
    }
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
    // Adding means updating THIS archive with its own handler, so it
    // takes an open archive whose handler both multi-file and writable
    // in the user's copy of 7z.dll.
    c.canAdd      = c.canExtract && m_open && !m_handlerName.empty() &&
                    ArchiveWriter::CanAddToFormat(m_handlerName) &&
                    ArchiveWriter::FormatIsWritable(m_handlerName);
    if (!c.canExtract)
        c.unavailableReason =
            L"No usable 7-Zip engine DLL was found, so .7z archives cannot "
            L"be read.\n\n" + ThirdParty::DescribeSearch(L"7z");
    return c;
}

bool C7zArchiveEngine::Test(ProgressFn cb)
{
    m_wrongPassword   = false;
    m_passwordMissing = false;
    if (!m_open || !m_archive) return false;
    UINT32 numItems = 0;
    m_archive->GetNumberOfItems(&numItems);

    auto* cbRaw = new CArchiveExtractCallback(m_archive.Get(), L"", numItems, cb,
                                               m_innerName, m_password);
    ComPtr<IArchiveExtractCallback7z> extractCb;
    extractCb.Attach(cbRaw);

    const HRESULT hr = m_archive->Extract(
        nullptr, (UINT32)-1, 1 /*testMode*/, extractCb.Get());
    const bool ok = SUCCEEDED(hr) && !cbRaw->HadError();
    if (!ok)
    {
        m_passwordMissing =
            cbRaw->PasswordWasRequested() && m_password.empty();
        m_wrongPassword = cbRaw->WrongPassword() || m_passwordMissing;
        if (m_wrongPassword && !m_passwordMissing)
            ForgetPassword(m_filePath, m_password);
        m_lastError = m_wrongPassword
            ? L"Encrypted items could not be verified: the password is "
              L"missing or wrong."
            : L"Archive integrity test reported errors.";
    }
    else
    {
        if (cbRaw->PasswordWasRequested())
            RememberPassword(m_filePath, m_password);
        m_lastError.clear();
    }
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
// Serves two shapes of update with one index space:
//
//   creating:  every index is a new item from disk.
//   updating:  indices 0..keepOld.size()-1 are items copied from the
//              archive being updated (the handler reads them itself,
//              through the still-open IInArchive); the rest are new.
//
// Both password interfaces are implemented: ICryptoGetTextPassword2 is
// the write side (encrypt new items with opt.password), and
// ICryptoGetTextPassword is the read side — the handler needs it when
// re-coding old encrypted items, e.g. a solid 7z block.
class CUpdateCallback final : public IArchiveUpdateCallback7z,
                              public ICryptoGetTextPassword2_7z,
                              public ICryptoGetTextPassword7z
{
public:
    CUpdateCallback(const std::vector<ArchiveWriter::Item>& items,
                    std::wstring password, ProgressFn progress,
                    std::vector<UINT32> keepOld = {},
                    std::wstring readPassword = std::wstring())
        : m_items(items), m_password(std::move(password)),
          m_progress(std::move(progress)),
          m_keepOld(std::move(keepOld)),
          m_readPassword(std::move(readPassword)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_IArchiveUpdateCallback7z))
            *ppv = static_cast<IArchiveUpdateCallback7z*>(this);
        else if (IsEqualIID(riid, IID_ICryptoGetTextPassword2_7z))
            *ppv = static_cast<ICryptoGetTextPassword2_7z*>(this);
        else if (IsEqualIID(riid, IID_ICryptoGetTextPassword7z))
            *ppv = static_cast<ICryptoGetTextPassword7z*>(this);
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
    STDMETHODIMP GetUpdateItemInfo(UINT32 index, INT32* newData,
                                   INT32* newProps, UINT32* indexInArchive) override
    {
        if (index < m_keepOld.size())
        {
            // Copied from the open archive: the handler takes data and
            // properties from the old item named here.
            if (newData)        *newData  = 0;
            if (newProps)       *newProps = 0;
            if (indexInArchive) *indexInArchive = m_keepOld[index];
            return S_OK;
        }
        // A new item from disk.
        if (newData)        *newData  = 1;
        if (newProps)       *newProps = 1;
        if (indexInArchive) *indexInArchive = (UINT32)(INT32)-1;
        return S_OK;
    }

    STDMETHODIMP GetProperty(UINT32 index, PROPID propID, PROPVARIANT* value) override
    {
        if (!value) return E_POINTER;
        PropVariantInit(value);
        // Copied items keep their old properties; the handler should not
        // ask, but an empty answer is the safe one if it does.
        if (index < m_keepOld.size()) return S_OK;
        index -= (UINT32)m_keepOld.size();
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
        if (index < m_keepOld.size()) return S_OK;   // old data: handler copies it
        index -= (UINT32)m_keepOld.size();
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

    // ICryptoGetTextPassword2 — encrypting what is being written.
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

    // ICryptoGetTextPassword — reading old encrypted items back during
    // an update. Falls back to the write password: the common case is
    // one password for the whole archive.
    STDMETHODIMP CryptoGetTextPassword(BSTR* password) override
    {
        if (!password) return E_POINTER;
        const std::wstring& pw = m_readPassword.empty() ? m_password
                                                        : m_readPassword;
        *password = SysAllocString(pw.c_str());
        return *password ? S_OK : E_OUTOFMEMORY;
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
    std::vector<UINT32> m_keepOld;       // archive indices copied as-is
    std::wstring m_readPassword;         // for decrypting those
};

bool IsLizardVariant(const std::wstring& method)
{
    return _wcsicmp(method.c_str(), L"Lizard, fastLZ4") == 0 ||
           _wcsicmp(method.c_str(), L"Lizard, LIZv1") == 0 ||
           _wcsicmp(method.c_str(), L"Lizard, fastLZ4 + Huffman") == 0 ||
           _wcsicmp(method.c_str(), L"Lizard, LIZv1 + Huffman") == 0;
}

int LizardLevelFor(const std::wstring& method, int uiLevel)
{
    int base = 10;
    if (_wcsicmp(method.c_str(), L"Lizard, LIZv1") == 0) base = 20;
    else if (_wcsicmp(method.c_str(), L"Lizard, fastLZ4 + Huffman") == 0) base = 30;
    else if (_wcsicmp(method.c_str(), L"Lizard, LIZv1 + Huffman") == 0) base = 40;
    if (uiLevel < 0) uiLevel = 0;
    if (uiLevel > 9) uiLevel = 9;
    return base + uiLevel;
}

// ── Apply Options to a writer ────────────────────────────
// Shared by Compress (new archive) and C7zArchiveEngine::AddItems
// (update): one place decides which property names each format gets,
// because one unknown name fails the whole SetProperties call.
HRESULT ApplyWriterProps(IOutArchive7z* outArc,
                         const ArchiveWriter::Options& opt,
                         const std::wstring& handlerName)
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
    auto addStr = [&](const wchar_t* n, const std::wstring& v)
    {
        BSTR b = SysAllocString(v.c_str());
        if (!b) return;
        names.emplace_back(n);
        PROPVARIANT pv; PropVariantInit(&pv);
        pv.vt = VT_BSTR; pv.bstrVal = b;
        values.push_back(pv);
    };

    const bool is7z  = _wcsicmp(handlerName.c_str(), L"7z")  == 0;
    const bool isZip = _wcsicmp(handlerName.c_str(), L"zip") == 0;

    const bool lizardVariant = IsLizardVariant(opt.method);
    int level = lizardVariant ? LizardLevelFor(opt.method, opt.level) : opt.level;
    if (!lizardVariant)
    {
        if (level < 0) level = 0;
        if (level > 9) level = 9;
    }
    addUInt(L"x", (UINT32)level);

    // The compression method, under the name each format uses for it:
    // -mm=Deflate for zip, -m0=LZMA2 for 7z. All four Lizard choices
    // share one external coder; their 10..49 level band chooses the variant.
    if (!opt.method.empty())
    {
        const std::wstring method = lizardVariant ? L"LIZARD" : opt.method;
        if (isZip)      addStr(L"m", method);
        else if (is7z)  addStr(L"0", method);
    }

    if (is7z)
    {
        if (opt.dictionaryBytes)
            addUInt(L"d", (UINT32)std::min<uint64_t>(
                opt.dictionaryBytes, std::numeric_limits<UINT32>::max()));
        if (opt.wordBytes) addUInt(L"fb", opt.wordBytes);
        if (opt.solid && opt.solidBlockBytes)
            addStr(L"s", std::to_wstring(opt.solidBlockBytes));
        else
            addBool(L"s", opt.solid);
        if (!opt.password.empty() && opt.encryptNames)
            addBool(L"he", true);
    }
    if (isZip && !opt.password.empty() && !opt.encMethod.empty())
    {
        // ZipCrypto (every unzip ever, no real security) or AES-256
        // (WinZip-style, needs a modern extractor).
        addStr(L"em", opt.encMethod);
    }
    if (opt.threads > 0 && (is7z || isZip ||
        _wcsicmp(handlerName.c_str(), L"xz") == 0 ||
        _wcsicmp(handlerName.c_str(), L"bzip2") == 0))
    {
        addUInt(L"mt", (UINT32)opt.threads);
    }

    HRESULT result = E_NOINTERFACE;
    Microsoft::WRL::ComPtr<ISetProperties7z> setProps;
    if (SUCCEEDED(outArc->QueryInterface(IID_ISetProperties7z,
                                         (void**)setProps.GetAddressOf())) && setProps)
    {
        std::vector<const wchar_t*> namePtrs;
        namePtrs.reserve(names.size());
        for (const auto& n : names) namePtrs.push_back(n.c_str());
        result = setProps->SetProperties(namePtrs.data(), values.data(),
                                         (UINT32)namePtrs.size());
    }
    for (auto& pv : values) PropVariantClear(&pv);
    return result;
}

} // anonymous namespace

namespace ArchiveWriter {

NativeCodecKind NativeKindForFormat(const std::wstring& format)
{
    if (_wcsicmp(format.c_str(), L"zstd") == 0)   return NativeCodecKind::Zstd;
    if (_wcsicmp(format.c_str(), L"brotli") == 0) return NativeCodecKind::Brotli;
    if (_wcsicmp(format.c_str(), L"lz4") == 0) return NativeCodecKind::Lz4;
    if (_wcsicmp(format.c_str(), L"lz5") == 0) return NativeCodecKind::Lz5;
    if (_wcsicmp(format.c_str(), L"lizard") == 0) return NativeCodecKind::Lizard;
    return NativeCodecKind::None;
}

bool NativeFormatWritable(const std::wstring& format)
{
    const NativeCodecKind kind = NativeKindForFormat(format);
    if (kind == NativeCodecKind::Zstd) return NativeZstdEncoderAvailable();
    if (kind == NativeCodecKind::Brotli) return NativeBrotliEncoderAvailable();
    if (kind == NativeCodecKind::Lz4 || kind == NativeCodecKind::Lz5)
        return GetNativeLzFrame(kind, true) != nullptr;
    if (kind == NativeCodecKind::Lizard)
        return GetNativeLzFrame(kind, true) != nullptr ||
               LizardFrame::EncoderAvailable();
    return false;
}

bool FinalizeCompressedOutput(const std::wstring& tempPath,
                              const std::wstring& outPath,
                              uint64_t volumeBytes,
                              std::wstring* error)
{
    auto fail = [&](const std::wstring& msg)
        { if (error) *error = msg; return false; };
    if (!volumeBytes)
    {
        if (MoveFileExW(tempPath.c_str(), outPath.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        const DWORD code = GetLastError();
        ::DeleteFileW(tempPath.c_str());
        return fail(L"The archive was built but could not be moved into place (error " +
                    std::to_wstring(code) + L").");
    }

    HANDLE in = CreateFileW(tempPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE)
    {
        ::DeleteFileW(tempPath.c_str());
        return fail(L"The completed archive could not be reopened for splitting.");
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(in, &fileSize) || fileSize.QuadPart <= 0)
    { CloseHandle(in); ::DeleteFileW(tempPath.c_str());
      return fail(L"The completed archive is empty and cannot be split."); }

    std::vector<std::wstring> staged, finalPaths;
    std::vector<BYTE> buffer(1024 * 1024);
    bool ok = true;
    uint64_t remaining = (uint64_t)fileSize.QuadPart;
    for (UINT32 part = 1; ok && remaining; ++part)
    {
        wchar_t suffix[24] = {};
        swprintf_s(suffix, L".%03u", part);
        const std::wstring finalPath = outPath + suffix;
        const std::wstring stagePath = tempPath + suffix;
        HANDLE out = CreateFileW(stagePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) { ok = false; break; }
        staged.push_back(stagePath);
        finalPaths.push_back(finalPath);
        uint64_t left = std::min<uint64_t>(volumeBytes, remaining);
        while (left)
        {
            const DWORD want = (DWORD)std::min<uint64_t>(left, buffer.size());
            DWORD got = 0;
            if (!ReadFile(in, buffer.data(), want, &got, nullptr) || !got)
            { ok = false; break; }
            DWORD written = 0;
            if (!WriteFile(out, buffer.data(), got, &written, nullptr) || written != got)
            { ok = false; break; }
            left -= got;
            remaining -= got;
        }
        CloseHandle(out);
    }
    CloseHandle(in);
    ::DeleteFileW(tempPath.c_str());
    if (!ok)
    {
        for (const auto& p : staged) ::DeleteFileW(p.c_str());
        return fail(L"The archive was built but could not be split into volumes.");
    }

    // Do not touch an older volume set until every new part has been written.
    size_t installed = 0;
    for (; installed < staged.size(); ++installed)
        if (!MoveFileExW(staged[installed].c_str(), finalPaths[installed].c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            break;
    if (installed != staged.size())
    {
        for (size_t i = installed; i < staged.size(); ++i)
            ::DeleteFileW(staged[i].c_str());
        for (size_t i = 0; i < installed; ++i)
            ::DeleteFileW(finalPaths[i].c_str());
        return fail(L"The archive was split but its volumes could not be moved into place.");
    }

    // Remove stale tail parts left by an older, larger split archive.
    for (UINT32 part = (UINT32)finalPaths.size() + 1; ; ++part)
    {
        wchar_t suffix[24] = {};
        swprintf_s(suffix, L".%03u", part);
        if (!::DeleteFileW((outPath + suffix).c_str())) break;
    }
    ::DeleteFileW(outPath.c_str());
    return true;
}

bool CompressNativeStream(const std::wstring& outPath,
                          const std::vector<Item>& items,
                          const Options& opt,
                          ProgressFn progress,
                          std::wstring* error)
{
    auto fail = [&](const std::wstring& msg)
        { if (error) *error = msg; return false; };
    if (items.size() != 1 || items[0].isDir || items[0].diskPath.empty())
        return fail(L"This format stores exactly one file stream. Select one file, not a folder.");

    const NativeCodecKind kind = NativeKindForFormat(opt.format);
    if (kind == NativeCodecKind::None || !NativeFormatWritable(opt.format))
        return fail(L"The required native encoder DLL is unavailable.");

    std::wstring tempPath = outPath + L".part";
    for (int n = 2; PathFileExistsW(tempPath.c_str()) && n < 100; ++n)
        tempPath = outPath + L".part" + std::to_wstring(n);

    auto* inRaw = new(std::nothrow) CInFileStream();
    if (!inRaw) return fail(L"Out of memory.");
    if (!inRaw->OpenFile(items[0].diskPath))
    { delete inRaw; return fail(L"The source file could not be opened."); }
    ComPtr<ISequentialInStream7z> in;
    in.Attach(inRaw);

    auto* outRaw = new(std::nothrow) COutSeekFileStream();
    if (!outRaw) return fail(L"Out of memory.");
    if (!outRaw->CreateOutputFile(tempPath))
    { outRaw->Release(); return fail(L"The output file could not be created."); }
    ComPtr<ISequentialOutStream7z> out;
    out.Attach(static_cast<ISequentialOutStream7z*>(outRaw));

    ICompressCoder7z* coderRaw = kind == NativeCodecKind::Zstd
        ? static_cast<ICompressCoder7z*>(new(std::nothrow) CNativeZstdEncoder())
        : static_cast<ICompressCoder7z*>(
            new(std::nothrow) CNativeAuxCoder(kind, true));
    if (!coderRaw)
    { outRaw->CloseFile(); ::DeleteFileW(tempPath.c_str()); return fail(L"Out of memory."); }
    ComPtr<ICompressCoder7z> coder;
    coder.Attach(coderRaw);

    HRESULT propsHr = S_OK;
    ComPtr<ICompressSetCoderProperties7z> props;
    if (coder->QueryInterface(IID_ICompressSetCoderProperties7z,
                              (void**)props.GetAddressOf()) == S_OK && props)
    {
        PROPID ids[2] = { 15, 1 }; // level, dictionary/block size
        PROPVARIANT values[2];
        PropVariantInit(&values[0]); PropVariantInit(&values[1]);
        values[0].vt = VT_UI4;
        values[0].ulVal = (ULONG)(kind == NativeCodecKind::Lizard
            ? LizardLevelFor(opt.method, opt.level) : opt.level);
        UINT32 count = 1;
        if (kind == NativeCodecKind::Lizard && opt.dictionaryBytes)
        {
            values[1].vt = VT_UI8;
            values[1].uhVal.QuadPart = opt.dictionaryBytes;
            count = 2;
        }
        propsHr = props->SetCoderProperties(ids, values, count);
    }

    class CProgress final : public ICompressProgressInfo7z
    {
    public:
        explicit CProgress(ProgressFn fn) : m_fn(std::move(fn)) {}
        STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            if (IsEqualIID(iid, IID_IUnknown) ||
                IsEqualIID(iid, IID_ICompressProgressInfo7z))
                *ppv = static_cast<ICompressProgressInfo7z*>(this);
            else { *ppv = nullptr; return E_NOINTERFACE; }
            AddRef(); return S_OK;
        }
        STDMETHODIMP_(ULONG) AddRef() override
            { return (ULONG)InterlockedIncrement(&m_ref); }
        STDMETHODIMP_(ULONG) Release() override
        { ULONG n=(ULONG)InterlockedDecrement(&m_ref); if(!n) delete this; return n; }
        STDMETHODIMP SetRatioInfo(const UINT64*, const UINT64* outSize) override
        { if (m_fn) m_fn(0, L""); (void)outSize; return S_OK; }
    private:
        LONG m_ref = 1; ProgressFn m_fn;
    };

    ComPtr<ICompressProgressInfo7z> progressObj;
    if (progress) progressObj.Attach(new CProgress(progress));
    const UINT64 size = items[0].size;
    const HRESULT hr = FAILED(propsHr) ? propsHr :
        coder->Code(in.Get(), out.Get(), &size, nullptr, progressObj.Get());
    outRaw->CloseFile();
    out.Reset();
    if (hr != S_OK)
    {
        ::DeleteFileW(tempPath.c_str());
        wchar_t code[32] = {}; swprintf_s(code, L"0x%08X", (unsigned)hr);
        return fail(L"Native compression failed (" + std::wstring(code) + L").");
    }
    return FinalizeCompressedOutput(tempPath, outPath, opt.volumeBytes, error);
}

bool IsAvailable()
{
    return (Get7zCreateObjectFunc() != nullptr && !WritableHandlers().empty()) ||
           NativeZstdEncoderAvailable() ||
           NativeBrotliEncoderAvailable() ||
           GetNativeLzFrame(NativeCodecKind::Lz4, true) ||
           GetNativeLzFrame(NativeCodecKind::Lz5, true) ||
           GetNativeLzFrame(NativeCodecKind::Lizard, true) ||
           LizardFrame::EncoderAvailable();
}

std::vector<std::wstring> WritableFormats()
{
    std::vector<std::wstring> out;
    for (const auto& h : WritableHandlers())
        if (!h.name.empty()) out.push_back(h.name);
    for (const wchar_t* format : { L"zstd", L"brotli", L"lz4", L"lz5", L"lizard" })
        if (NativeFormatWritable(format) &&
            std::none_of(out.begin(), out.end(), [&](const std::wstring& f) {
                return _wcsicmp(f.c_str(), format) == 0; }))
            out.emplace_back(format);
    return out;
}

bool FormatIsWritable(const std::wstring& format)
{
    if (NativeFormatWritable(format)) return true;
    for (const auto& h : WritableHandlers())
        if (_wcsicmp(h.name.c_str(), format.c_str()) == 0) return true;
    return false;
}

// ── Choice lists for the Add to Archive dialog ───────────
std::vector<std::wstring> MethodsFor(const std::wstring& format)
{
    // First entry is the format's own default. Native runtime adapters
    // publish the additional ZSTD/Brotli/LZ4/LZ5/Lizard coder names to
    // 7z.dll; an explicitly rejected selection is reported by Compress().
    if (_wcsicmp(format.c_str(), L"7z") == 0)
        return { L"LZMA2", L"ZSTD", L"BROTLI", L"LZ4", L"LZ5",
                 L"Lizard, fastLZ4", L"Lizard, LIZv1",
                 L"Lizard, fastLZ4 + Huffman", L"Lizard, LIZv1 + Huffman",
                 L"LZMA", L"PPMd", L"BZip2", L"Copy" };
    if (_wcsicmp(format.c_str(), L"zip") == 0)
        return { L"Deflate", L"ZSTD", L"Deflate64", L"BZip2", L"LZMA",
                 L"PPMd", L"Copy" };
    if (_wcsicmp(format.c_str(), L"xz") == 0)    return { L"LZMA2" };
    if (_wcsicmp(format.c_str(), L"gzip") == 0)  return { L"Deflate" };
    if (_wcsicmp(format.c_str(), L"bzip2") == 0) return { L"BZip2" };
    if (_wcsicmp(format.c_str(), L"zstd") == 0)  return { L"Zstandard" };
    if (_wcsicmp(format.c_str(), L"brotli") == 0)return { L"Brotli" };
    if (_wcsicmp(format.c_str(), L"lz4") == 0)   return { L"LZ4" };
    if (_wcsicmp(format.c_str(), L"lz5") == 0)   return { L"LZ5" };
    if (_wcsicmp(format.c_str(), L"lizard") == 0)
        return { L"Lizard, fastLZ4", L"Lizard, LIZv1",
                 L"Lizard, fastLZ4 + Huffman", L"Lizard, LIZv1 + Huffman" };
    // tar and wim have exactly one way to store data.
    return {};
}

std::vector<std::wstring> EncryptionMethodsFor(const std::wstring& format)
{
    // First entry is the default offered.
    if (_wcsicmp(format.c_str(), L"zip") == 0)
        return { L"AES256", L"ZipCrypto" };
    if (_wcsicmp(format.c_str(), L"7z") == 0)
        return { L"AES256" };               // 7z has no other cipher
    return {};                               // format cannot encrypt
}

bool CanAddToFormat(const std::wstring& format)
{
    // Multi-file containers whose handlers implement update. gzip,
    // bzip2 and xz hold exactly one stream — "add" has no meaning —
    // and the read-only handlers (rar, iso, ...) never get here.
    static const wchar_t* const kUpdatable[] = { L"zip", L"7z", L"tar", L"wim" };
    for (const wchar_t* f : kUpdatable)
        if (_wcsicmp(format.c_str(), f) == 0) return true;
    return false;
}

// Extensions that are a known container under a name 7z.dll's handler
// does not happen to advertise.
//
// A handler publishes the extension list its own build was compiled
// with, and the zip handler's is "zip z01 zipx jar xpi odt ods docx xlsx
// epub ipa apk appx" -- which leaves .cbz, .smzip, .zab, .war and .ear
// out even though every one of them is an ordinary zip. Without this
// table a target name ending in .cbz matches no handler, so compressing
// to one was refused outright. The same applies to the comic and tarball
// spellings of the other containers.
//
// This only names the *container*. Whether that container can actually
// be written is still 7z.dll's answer, checked below.
struct ExtAlias { const wchar_t* ext; const wchar_t* handler; };
static const ExtAlias kExtAliases[] =
{
    // zip family
    { L"cbz",   L"zip"   },
    { L"smzip", L"zip"   },
    { L"zab",   L"zip"   },
    { L"war",   L"zip"   },
    { L"ear",   L"zip"   },
    // the others that share a container under a different spelling
    { L"cb7",   L"7z"    },
    { L"cbt",   L"tar"   },
    { L"tb2",   L"bzip2" },
    { L"tbz",   L"bzip2" },
    { L"tbz2",  L"bzip2" },
    { L"dz",    L"gzip"  },
    // native single-stream writers
    { L"zst",   L"zstd"  },
    { L"zstd",  L"zstd"  },
    { L"zsd",   L"zstd"  },
    { L"tzst",  L"zstd"  },
    { L"br",    L"brotli"},
    { L"lz4",   L"lz4"   },
    { L"lz5",   L"lz5"   },
    { L"liz",   L"lizard"},
};

std::wstring FormatForTargetName(const std::wstring& fileName)
{
    const wchar_t* dot = PathFindExtensionW(fileName.c_str());
    if (!dot || !*dot) return L"";
    const std::wstring bare = dot + 1;

    for (const auto& h : WritableHandlers())
        for (const auto& e : h.exts)
            if (_wcsicmp(e.c_str(), bare.c_str()) == 0) return h.name;

    // Nothing advertised it. Try the aliases, and still insist the
    // handler they name is one this copy of 7z.dll can write.
    for (const auto& a : kExtAliases)
        if (_wcsicmp(a.ext, bare.c_str()) == 0 && FormatIsWritable(a.handler))
            return a.handler;

    return L"";
}

std::wstring DefaultExtensionFor(const std::wstring& format)
{
    if (_wcsicmp(format.c_str(), L"zstd") == 0) return L".zst";
    if (_wcsicmp(format.c_str(), L"brotli") == 0) return L".br";
    if (_wcsicmp(format.c_str(), L"lizard") == 0) return L".liz";
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

    if (NativeKindForFormat(opt.format) != NativeCodecKind::None &&
        NativeFormatWritable(opt.format))
        return CompressNativeStream(outPath, items, opt, progress, error);

    Func7z_CreateObject createObj = Get7zCreateObjectFunc();
    if (!createObj)
        return fail(L"7z.dll could not be loaded, so ArchiveFldr cannot create "
                    L"archives.\n\n" + ThirdParty::DescribeSearch(L"7z"));

    GetExternalCodecs();

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

    AttachExternalCodecs(outArc.Get());

    // ── Compression settings ─────────────────────────────
    hr = ApplyWriterProps(outArc.Get(), opt, handler->name);
    const bool methodWasApplied =
        _wcsicmp(handler->name.c_str(), L"7z") == 0 ||
        _wcsicmp(handler->name.c_str(), L"zip") == 0;
    if (FAILED(hr) && methodWasApplied && !opt.method.empty())
        return fail(L"The selected compression method is not accepted by this "
                    L"7-Zip handler.");

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
        ::DeleteFileW(tempPath.c_str());
        if (hr == E_ABORT) return fail(L"Cancelled.");
        wchar_t buf[64]; swprintf_s(buf, L"0x%08X", (unsigned)hr);
        return fail(L"Compression failed (" + std::wstring(buf) + L").");
    }

    if (!FinalizeCompressedOutput(tempPath, outPath, opt.volumeBytes, error))
        return false;

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

// ═════════════════════════════════════════════════════════
// C7zArchiveEngine::AddItems — add files to THIS archive
// ═════════════════════════════════════════════════════════
// One IOutArchive update pass: the open handler copies the items it
// already holds (newData = 0) and compresses the new ones from disk.
// Defined down here because it uses the writer machinery above —
// CUpdateCallback, COutSeekFileStream, ApplyWriterProps.
bool C7zArchiveEngine::AddItems(const std::vector<ArchiveWriter::Item>& items,
                                const ArchiveWriter::Options& opt,
                                const std::wstring& destPath,
                                ProgressFn cb,
                                std::wstring* err)
{
    auto fail = [&](const std::wstring& msg)
    { m_lastError = msg; if (err) *err = msg; return false; };

    if (!m_open || !m_archive) return fail(L"The archive is not open.");
    if (items.empty())         return fail(L"Nothing to add.");
    if (!ArchiveWriter::CanAddToFormat(m_handlerName) ||
        !ArchiveWriter::FormatIsWritable(m_handlerName))
        return fail(L"Files cannot be added to a \"" + m_handlerName +
                    L"\" archive.");

    // The handler object behind m_archive is also the writer for its
    // format — that is what lets it copy old items without recoding.
    ComPtr<IOutArchive7z> outArc;
    if (FAILED(m_archive->QueryInterface(IID_IOutArchive7z,
                                         (void**)outArc.GetAddressOf())) || !outArc)
        return fail(L"This copy of 7z.dll cannot update \"" + m_handlerName +
                    L"\" archives.");

    // ── Which old items survive ──────────────────────────
    // Everything, except items a new file replaces (same stored path).
    auto normKey = [](std::wstring s)
    {
        for (auto& ch : s)
        {
            if (ch == L'\\') ch = L'/';
            ch = towlower(ch);
        }
        while (!s.empty() && s.back() == L'/') s.pop_back();
        return s;
    };

    std::unordered_map<std::wstring, bool> newNames;   // normalized path -> present
    for (const auto& it : items)
        newNames[normKey(it.nameInArchive)] = true;

    UINT32 numOld = 0;
    m_archive->GetNumberOfItems(&numOld);
    std::vector<UINT32> keepOld;
    keepOld.reserve(numOld);
    for (UINT32 i = 0; i < numOld; ++i)
    {
        std::wstring path = PropGetString(m_archive.Get(), i, k7zPidPath);
        if (path.empty()) path = m_innerName;
        if (newNames.count(normKey(path))) continue;   // replaced by a new file
        keepOld.push_back(i);
    }

    // ── Where the result goes ────────────────────────────
    const bool inPlace = destPath.empty() ||
                         _wcsicmp(destPath.c_str(), m_filePath.c_str()) == 0;
    const std::wstring finalPath = inPlace ? m_filePath : destPath;

    std::wstring tempPath = finalPath + L".part";
    for (int n = 2; PathFileExistsW(tempPath.c_str()) && n < 100; ++n)
        tempPath = finalPath + L".part" + std::to_wstring(n);

    auto* outStream = new (std::nothrow) COutSeekFileStream();
    if (!outStream) return fail(L"Out of memory.");
    if (!outStream->CreateOutputFile(tempPath))
    {
        const DWORD e = ::GetLastError();
        outStream->Release();
        return fail(L"Could not create \"" + tempPath + L"\" (error " +
                    std::to_wstring(e) + L").");
    }

    const HRESULT propsHr = ApplyWriterProps(outArc.Get(), opt, m_handlerName);
    const bool methodWasApplied =
        _wcsicmp(m_handlerName.c_str(), L"7z") == 0 ||
        _wcsicmp(m_handlerName.c_str(), L"zip") == 0;
    if (FAILED(propsHr) && methodWasApplied && !opt.method.empty())
    {
        outStream->CloseFile(); outStream->Release();
        ::DeleteFileW(tempPath.c_str());
        return fail(L"The selected compression method is not accepted by this "
                    L"7-Zip handler.");
    }

    // opt.password encrypts the new items; m_password (the one the
    // archive was opened with) decrypts old ones if recoding needs it.
    auto* callback = new (std::nothrow)
        CUpdateCallback(items, opt.password, cb, keepOld, m_password);
    if (!callback) { outStream->Release(); return fail(L"Out of memory."); }

    const UINT32 total = (UINT32)(keepOld.size() + items.size());
    HRESULT hr = outArc->UpdateItems(
        static_cast<ISequentialOutStream7z*>(outStream), total, callback);

    const std::vector<std::wstring> skipped = callback->Skipped();
    callback->Release();
    outStream->CloseFile();
    outStream->Release();
    outArc.Reset();

    if (FAILED(hr))
    {
        ::DeleteFileW(tempPath.c_str());
        if (hr == E_ABORT) return fail(L"Cancelled.");
        wchar_t buf[64]; swprintf_s(buf, L"0x%08X", (unsigned)hr);
        return fail(L"Updating the archive failed (" + std::wstring(buf) +
                    L").\n\nIf items in it are encrypted, the archive's "
                    L"password may be needed to rewrite them.");
    }

    // ── Move the result into place ───────────────────────
    bool ok = true;
    if (inPlace)
    {
        // The engine's own read stream holds the file: close everything,
        // swap, reopen. Open() rebuilds the entry list, which is also
        // what makes the new files appear.
        const std::wstring reopenPath = m_filePath;
        const std::wstring password   = m_password;
        Close();

        if (!MoveFileExW(tempPath.c_str(), reopenPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING))
        {
            const DWORD e = ::GetLastError();
            ::DeleteFileW(tempPath.c_str());
            m_password = password;
            Open(reopenPath);                       // put the original back up
            return fail(L"The updated archive was built but could not "
                        L"replace the original (error " +
                        std::to_wstring(e) + L").");
        }

        m_password = password;
        ok = Open(reopenPath);
        if (!ok)
            return fail(L"The archive was updated, but could not be "
                        L"reopened: " + m_lastError);
    }
    else
    {
        ::DeleteFileW(finalPath.c_str());
        if (!MoveFileW(tempPath.c_str(), finalPath.c_str()))
        {
            const DWORD e = ::GetLastError();
            ::DeleteFileW(tempPath.c_str());
            return fail(L"The updated archive was built but could not be "
                        L"moved to \"" + finalPath + L"\" (error " +
                        std::to_wstring(e) + L").");
        }
    }

    if (!skipped.empty() && err)
    {
        *err = L"Finished, but " + std::to_wstring(skipped.size()) +
               L" file(s) could not be read and were left out:\n\n";
        for (size_t i = 0; i < skipped.size() && i < 10; ++i)
            *err += L"  " + skipped[i] + L"\n";
        if (skipped.size() > 10) *err += L"  ...\n";
    }
    return true;
}

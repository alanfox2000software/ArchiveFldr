// Sdk7z.h
// Minimal, self-contained re-declaration of the handful of 7-Zip SDK
// "COM-lite" interfaces ArchiveFldr needs to drive the external 7z.dll engine
// (thirdparty\7z\7z.64.dll / thirdparty\7z\7z.32.dll).
//
// We intentionally do NOT vendor the full 7-Zip C++ SDK. These interfaces
// are a stable, binary (vtable-level) contract that hasn't changed across
// 7-Zip releases for decades, so hand-declaring just what we call is safe
// and keeps the dependency footprint tiny. Method order/signatures below
// were cross-checked against the official 7-Zip SDK headers
// (CPP/7zip/Archive/IArchive.h, CPP/7zip/IStream.h, CPP/7zip/IProgress.h,
// CPP/7zip/IPassword.h, CPP/7zip/IDecl.h, CPP/7zip/PropID.h).
//
// GUID scheme (see CPP/7zip/IDecl.h):
//   23170F69-40C1-278A-0000-00{group:02X}00{sub:02X}0000
//
#pragma once
#include "stdafx.h"

// ─────────────────────────────────────────────────────────
// Property IDs we care about (subset of CPP/7zip/PropID.h)
// ─────────────────────────────────────────────────────────
enum Sdk7zPropID : PROPID
{
    k7zPidPath      = 3,
    k7zPidName      = 4,
    k7zPidIsDir     = 6,
    k7zPidSize      = 7,
    k7zPidPackSize  = 8,
    k7zPidAttrib    = 9,
    k7zPidCTime     = 10,
    k7zPidATime     = 11,
    k7zPidMTime     = 12,
    k7zPidEncrypted = 15,
    k7zPidCRC       = 19,
    k7zPidMethod    = 22,
    k7zPidBlock     = 27,   // solid-block (folder) index
};

// ─────────────────────────────────────────────────────────
// Extract operation enums (NArchive::NExtract in the SDK)
// ─────────────────────────────────────────────────────────
namespace N7zExtract
{
    enum AskMode
    {
        kExtract = 0,
        kTest,
        kSkip,
        kReadExternal
    };

    enum OperationResult
    {
        kOK = 0,
        kUnsupportedMethod,
        kDataError,
        kCRCError,
        kUnavailable,
        kUnexpectedEnd,
        kDataAfterEnd,
        kIsNotArc,
        kHeadersError,
        kWrongPassword
    };
}

// ─────────────────────────────────────────────────────────
// Streams (CPP/7zip/IStream.h) — group 3
// ─────────────────────────────────────────────────────────
struct ISequentialInStream7z : public IUnknown
{
    STDMETHOD(Read)(void* data, UINT32 size, UINT32* processedSize) PURE;
};

struct ISequentialOutStream7z : public IUnknown
{
    STDMETHOD(Write)(const void* data, UINT32 size, UINT32* processedSize) PURE;
};

struct IInStream7z : public ISequentialInStream7z
{
    STDMETHOD(Seek)(INT64 offset, UINT32 seekOrigin, UINT64* newPosition) PURE;
};

struct IOutStream7z : public ISequentialOutStream7z
{
    STDMETHOD(Seek)(INT64 offset, UINT32 seekOrigin, UINT64* newPosition) PURE;
    STDMETHOD(SetSize)(UINT64 newSize) PURE;
};

// ─────────────────────────────────────────────────────────
// IProgress (CPP/7zip/IProgress.h) — group 0 / sub 5
// ─────────────────────────────────────────────────────────
struct IProgress7z : public IUnknown
{
    STDMETHOD(SetTotal)(UINT64 total) PURE;
    STDMETHOD(SetCompleted)(const UINT64* completeValue) PURE;
};

// ─────────────────────────────────────────────────────────
// Compression coders and external codecs (CPP/7zip/ICoder.h) — group 4
//
// The coder interfaces let ArchiveFldr adapt its existing raw codec DLLs to
// methods requested by a 7z archive. The catalogue also exposes real 7-Zip
// plug-ins discovered in the engine's adjacent Codecs directory.
// ─────────────────────────────────────────────────────────
struct ICompressProgressInfo7z : public IUnknown
{
    STDMETHOD(SetRatioInfo)(const UINT64* inSize, const UINT64* outSize) PURE;
};

struct ICompressCoder7z : public IUnknown
{
    STDMETHOD(Code)(ISequentialInStream7z* inStream,
                    ISequentialOutStream7z* outStream,
                    const UINT64* inSize, const UINT64* outSize,
                    ICompressProgressInfo7z* progress) PURE;
};

struct ICompressSetCoderProperties7z : public IUnknown
{
    STDMETHOD(SetCoderProperties)(const PROPID* propIDs,
                                  const PROPVARIANT* props,
                                  UINT32 numProps) PURE;
};

struct ICompressSetDecoderProperties2_7z : public IUnknown
{
    STDMETHOD(SetDecoderProperties2)(const BYTE* data, UINT32 size) PURE;
};

struct ICompressWriteCoderProperties7z : public IUnknown
{
    STDMETHOD(WriteCoderProperties)(ISequentialOutStream7z* outStream) PURE;
};

struct ICompressCodecsInfo7z : public IUnknown
{
    STDMETHOD(GetNumMethods)(UINT32* numMethods) PURE;
    STDMETHOD(GetProperty)(UINT32 index, PROPID propID,
                           PROPVARIANT* value) PURE;
    STDMETHOD(CreateDecoder)(UINT32 index, const GUID* iid,
                             void** coder) PURE;
    STDMETHOD(CreateEncoder)(UINT32 index, const GUID* iid,
                             void** coder) PURE;
};

struct ISetCompressCodecsInfo7z : public IUnknown
{
    STDMETHOD(SetCompressCodecsInfo)(ICompressCodecsInfo7z* codecs) PURE;
};

// Method properties exported by codec plug-ins (NMethodPropID).
enum Sdk7zMethodPropID : PROPID
{
    k7zMethodID                = 0,
    k7zMethodName              = 1,
    k7zMethodDecoder           = 2,
    k7zMethodEncoder           = 3,
    k7zMethodPackStreams       = 4,
    k7zMethodUnpackStreams     = 5,
    k7zMethodDescription       = 6,
    k7zMethodDecoderIsAssigned = 7,
    k7zMethodEncoderIsAssigned = 8,
    k7zMethodDigestSize        = 9,
    k7zMethodIsFilter          = 10,
};

// ─────────────────────────────────────────────────────────
// Archive callbacks (CPP/7zip/Archive/IArchive.h) — group 6
// ─────────────────────────────────────────────────────────
struct IArchiveOpenCallback7z : public IUnknown
{
    STDMETHOD(SetTotal)(const UINT64* files, const UINT64* bytes) PURE;
    STDMETHOD(SetCompleted)(const UINT64* files, const UINT64* bytes) PURE;
};

struct IArchiveOpenVolumeCallback7z : public IUnknown
{
    STDMETHOD(GetProperty)(PROPID propID, PROPVARIANT* value) PURE;
    STDMETHOD(GetStream)(const wchar_t* name, IInStream7z** inStream) PURE;
};

struct IArchiveExtractCallback7z : public IProgress7z
{
    STDMETHOD(GetStream)(UINT32 index, ISequentialOutStream7z** outStream,
                          INT32 askExtractMode) PURE;
    STDMETHOD(PrepareOperation)(INT32 askExtractMode) PURE;
    STDMETHOD(SetOperationResult)(INT32 opRes) PURE;
};

// ─────────────────────────────────────────────────────────
// IInArchive (CPP/7zip/Archive/IArchive.h) — group 6 / sub 0x60
// ─────────────────────────────────────────────────────────
struct IInArchive7z : public IUnknown
{
    STDMETHOD(Open)(IInStream7z* stream, const UINT64* maxCheckStartPosition,
                     IArchiveOpenCallback7z* openCallback) PURE;
    STDMETHOD(Close)() PURE;
    STDMETHOD(GetNumberOfItems)(UINT32* numItems) PURE;
    STDMETHOD(GetProperty)(UINT32 index, PROPID propID, PROPVARIANT* value) PURE;
    STDMETHOD(Extract)(const UINT32* indices, UINT32 numItems, INT32 testMode,
                        IArchiveExtractCallback7z* extractCallback) PURE;
    STDMETHOD(GetArchiveProperty)(PROPID propID, PROPVARIANT* value) PURE;
    STDMETHOD(GetNumberOfProperties)(UINT32* numProps) PURE;
    STDMETHOD(GetPropertyInfo)(UINT32 index, BSTR* name, PROPID* propID,
                               VARTYPE* varType) PURE;
    STDMETHOD(GetNumberOfArchiveProperties)(UINT32* numProps) PURE;
    STDMETHOD(GetArchivePropertyInfo)(UINT32 index, BSTR* name, PROPID* propID,
                                      VARTYPE* varType) PURE;
};

// ─────────────────────────────────────────────────────────
// ICryptoGetTextPassword (CPP/7zip/IPassword.h) — group 5 / sub 0x10
// ─────────────────────────────────────────────────────────
struct ICryptoGetTextPassword7z : public IUnknown
{
    STDMETHOD(CryptoGetTextPassword)(BSTR* password) PURE;
};

// ICryptoGetTextPassword2 — the write-side counterpart. 7-Zip asks
// whether a password was supplied at all, so an empty answer means
// "no encryption" rather than "encrypt with an empty password".
struct ICryptoGetTextPassword2_7z : public IUnknown
{
    STDMETHOD(CryptoGetTextPassword2)(INT32* passwordIsDefined, BSTR* password) PURE;
};

// ─────────────────────────────────────────────────────────
// Writing archives
// ─────────────────────────────────────────────────────────
// IArchiveUpdateCallback drives creation the way IArchiveExtractCallback
// drives extraction: 7-Zip asks us, item by item, what to store and
// where to read it from.
//
// GetUpdateItemInfo tells the handler whether each item is new data,
// new properties, or copied from an archive already open. Creating from
// scratch means "new data, new properties, no index in archive" for
// every item.
struct IArchiveUpdateCallback7z : public IProgress7z
{
    STDMETHOD(GetUpdateItemInfo)(UINT32 index, INT32* newData, INT32* newProps,
                                 UINT32* indexInArchive) PURE;
    STDMETHOD(GetProperty)(UINT32 index, PROPID propID, PROPVARIANT* value) PURE;
    STDMETHOD(GetStream)(UINT32 index, ISequentialInStream7z** inStream) PURE;
    STDMETHOD(SetOperationResult)(INT32 operationResult) PURE;
};

struct IOutArchive7z : public IUnknown
{
    STDMETHOD(UpdateItems)(ISequentialOutStream7z* outStream, UINT32 numItems,
                           IArchiveUpdateCallback7z* updateCallback) PURE;
    STDMETHOD(GetFileTimeType)(UINT32* type) PURE;
};

// Compression settings are passed as named properties — "x" for level,
// "s" for solid, "mt" for thread count. Which names a handler accepts
// depends on the format, and one unknown name fails the whole call, so
// only ever send names the target format is known to take.
struct ISetProperties7z : public IUnknown
{
    STDMETHOD(SetProperties)(const wchar_t* const* names, const PROPVARIANT* values,
                             UINT32 numProps) PURE;
};

// ─────────────────────────────────────────────────────────
// GUIDs
// Storage is emitted exactly once, from dllmain.cpp (which includes
// <initguid.h> before this header) — every other translation unit only
// gets an `extern` declaration, matching the convention already used by
// GUIDs.h in this project.
// ─────────────────────────────────────────────────────────
DEFINE_GUID(IID_ISequentialInStream7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x03,0x00,0x01,0x00,0x00);
DEFINE_GUID(IID_ISequentialOutStream7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x03,0x00,0x02,0x00,0x00);
DEFINE_GUID(IID_IInStream7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x03,0x00,0x03,0x00,0x00);
DEFINE_GUID(IID_IOutStream7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x03,0x00,0x04,0x00,0x00);
DEFINE_GUID(IID_ICompressProgressInfo7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x04,0x00,0x04,0x00,0x00);
DEFINE_GUID(IID_ICompressCoder7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x04,0x00,0x05,0x00,0x00);
DEFINE_GUID(IID_ICompressSetCoderProperties7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x04,0x00,0x20,0x00,0x00);
DEFINE_GUID(IID_ICompressSetDecoderProperties2_7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x04,0x00,0x22,0x00,0x00);
DEFINE_GUID(IID_ICompressWriteCoderProperties7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x04,0x00,0x23,0x00,0x00);
DEFINE_GUID(IID_ICompressCodecsInfo7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x04,0x00,0x60,0x00,0x00);
DEFINE_GUID(IID_ISetCompressCodecsInfo7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x04,0x00,0x61,0x00,0x00);
DEFINE_GUID(IID_IArchiveOpenCallback7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x06,0x00,0x10,0x00,0x00);
DEFINE_GUID(IID_IArchiveExtractCallback7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x06,0x00,0x20,0x00,0x00);
DEFINE_GUID(IID_IArchiveOpenVolumeCallback7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x06,0x00,0x30,0x00,0x00);
DEFINE_GUID(IID_IInArchive7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x06,0x00,0x60,0x00,0x00);
DEFINE_GUID(IID_ICryptoGetTextPassword7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x05,0x00,0x10,0x00,0x00);
DEFINE_GUID(IID_ICryptoGetTextPassword2_7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x05,0x00,0x11,0x00,0x00);
DEFINE_GUID(IID_ISetProperties7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x06,0x00,0x03,0x00,0x00);
DEFINE_GUID(IID_IArchiveUpdateCallback7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x06,0x00,0x80,0x00,0x00);
DEFINE_GUID(IID_IOutArchive7z,
    0x23170F69, 0x40C1, 0x278A, 0x00,0x00,0x00,0x06,0x00,0xA0,0x00,0x00);

// 7z format class id — {23170F69-40C1-278A-1000-000110070000}
DEFINE_GUID(CLSID_CFormat7z,
    0x23170F69, 0x40C1, 0x278A, 0x10,0x00,0x00,0x01,0x10,0x07,0x00,0x00);

// ─────────────────────────────────────────────────────────
// 7z.dll exported entry points we call via GetProcAddress
// ─────────────────────────────────────────────────────────
// 7z.dll publishes its own format list. Asking it which formats it has,
// instead of hard-coding a table of class GUIDs, means ArchiveFldr supports
// exactly what the user's copy of 7z.dll supports — including formats
// added in versions newer than this source.
typedef HRESULT (WINAPI *Func7z_GetNumberOfFormats)(UINT32* numFormats);
typedef HRESULT (WINAPI *Func7z_GetHandlerProperty2)(UINT32 formatIndex,
                                                     PROPID propID,
                                                     PROPVARIANT* value);

// PROPIDs understood by GetHandlerProperty2.
enum
{
    kHandlerName      = 0,   // VT_BSTR  "tar"
    kHandlerClassID   = 1,   // VT_BSTR  raw 16-byte GUID
    kHandlerExtension = 2,   // VT_BSTR  "tar ova"
    kHandlerUpdate    = 4,   // VT_BOOL  handler can create/modify archives
};

typedef HRESULT (WINAPI *Func7z_CreateObject)(
    const GUID* clsid, const GUID* iid, void** outObject);

// Exports used by DLLs in 7-Zip's Codecs folder.
typedef HRESULT (WINAPI *Func7z_GetNumberOfMethods)(UINT32* numMethods);
typedef HRESULT (WINAPI *Func7z_GetMethodProperty)(UINT32 methodIndex,
                                                   PROPID propID,
                                                   PROPVARIANT* value);
typedef HRESULT (WINAPI *Func7z_CreateCoder)(UINT32 methodIndex,
                                             const GUID* iid,
                                             void** outObject);
typedef HRESULT (WINAPI *Func7z_SetCodecs)(ICompressCodecsInfo7z* codecs);

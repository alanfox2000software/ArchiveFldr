// Formats.cpp — see Formats.h
#include "stdafx.h"
#include "Formats.h"

namespace Formats {

using EK = EngineKind;

// ─────────────────────────────────────────────────────────────────────────
// The table.
//
// ADDING A FORMAT — one row, then (only if it needs a new DLL) one row in
// ThirdParty.cpp's kComponents[].
//
// progId == nullptr means "open it if asked, but do not take the file type
// over". Office containers are deliberately left that way: .docx really is
// a zip, but hijacking Word's file association is hostile.
// ─────────────────────────────────────────────────────────────────────────
static const Format kFormats[] =
{
//    ext        name              progId                 engine        component  codec     unwrap
    // ── 7-Zip handles these ──────────────────────────────────────────────
    { L".7z",    L"7-Zip",         L"ShellNSE.7zFile",    EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".7zip",  L"7-Zip",         nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".zip",   L"ZIP",           L"ShellNSE.ZipFile",   EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".zipx",  L"ZIPX",          nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".jar",   L"Java archive",  L"ShellNSE.JarFile",   EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".war",   L"Web archive",   nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".ear",   L"EAR",           nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".apk",   L"Android pack",  L"ShellNSE.ApkFile",   EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".ipa",   L"iOS app",       nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".docx",  L"Word document", nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".xlsx",  L"Excel workbook",nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".pptx",  L"PowerPoint",    nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".odt",   L"OpenDocument",  nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".ods",   L"OpenDocument",  nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".odp",   L"OpenDocument",  nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".tar",   L"TAR",           L"ShellNSE.TarFile",   EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".tgz",   L"TAR + GZip",    L"ShellNSE.TgzFile",   EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".tbz2",  L"TAR + BZip2",   nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".txz",   L"TAR + XZ",      nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".tlz",   L"TAR + LZMA",    nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".gz",    L"GZip",          L"ShellNSE.GzFile",    EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".gzip",  L"GZip",          nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".bz2",   L"BZip2",         L"ShellNSE.Bz2File",   EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".bzip2", L"BZip2",         nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".xz",    L"XZ",            L"ShellNSE.XzFile",    EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".z",     L"compress",      nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".lz",    L"Lzip",          nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".lzma",  L"LZMA",          nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".lzh",   L"LZH",           L"ShellNSE.LzhFile",   EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".lha",   L"LZH",           nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".arj",   L"ARJ",           nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".cab",   L"Cabinet",       L"ShellNSE.CabFile",   EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".iso",   L"ISO image",     L"ShellNSE.IsoFile",   EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".img",   L"Disk image",    nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".nrg",   L"Nero image",    nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".mdf",   L"Disk image",    nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".msi",   L"Windows installer", nullptr,           EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".msm",   L"Merge module",  nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".msp",   L"Windows patch", nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".rpm",   L"RPM package",   nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".deb",   L"Debian package",nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".cpio",  L"CPIO",          nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".dmg",   L"Apple image",   nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".vhd",   L"Virtual disk",  nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },
    { L".vhdx",  L"Virtual disk",  nullptr,               EK::SevenZip, L"7z",     nullptr,  nullptr },

    // ── RAR: prefer unrar.dll, fall back to 7-Zip ────────────────────────
    { L".rar",   L"RAR",           L"ShellNSE.RarFile",   EK::Unrar,    L"Unrar",  nullptr,  nullptr },
    { L".cbr",   L"Comic book RAR",L"ShellNSE.CbrFile",   EK::Unrar,    L"Unrar",  nullptr,  nullptr },
    { L".r00",   L"RAR volume",    nullptr,               EK::Unrar,    L"Unrar",  nullptr,  nullptr },
    { L".r01",   L"RAR volume",    nullptr,               EK::Unrar,    L"Unrar",  nullptr,  nullptr },
    { L".r02",   L"RAR volume",    nullptr,               EK::Unrar,    L"Unrar",  nullptr,  nullptr },

    // ── WIM family: prefer wimlib, fall back to 7-Zip ────────────────────
    { L".wim",   L"Windows image", L"ShellNSE.WimFile",   EK::Wim,      L"WimLib", nullptr,  nullptr },
    { L".swm",   L"Split WIM",     nullptr,               EK::Wim,      L"WimLib", nullptr,  nullptr },
    { L".esd",   L"Encrypted WIM", nullptr,               EK::Wim,      L"WimLib", nullptr,  nullptr },

    // ── Single-stream codecs ─────────────────────────────────────────────
    // One compressed stream = one file. 7-Zip proper ships none of these
    // codecs, so there is no fallback: without the DLL the file cannot be
    // read, and the UI says which DLL is missing and where it looked.
    { L".zst",   L"Zstandard",     L"ShellNSE.ZstFile",   EK::Codec,    L"zstd",   L"zstd",   nullptr },
    { L".zstd",  L"Zstandard",     nullptr,               EK::Codec,    L"zstd",   L"zstd",   nullptr },
    { L".tzst",  L"Zstandard TAR", L"ShellNSE.TzstFile",  EK::Codec,    L"zstd",   L"zstd",   L".tar" },
    { L".br",    L"Brotli",        L"ShellNSE.BrFile",    EK::Codec,    L"brotli", L"brotli", nullptr },
    { L".lz4",   L"LZ4",           L"ShellNSE.Lz4File",   EK::Codec,    L"lz4",    L"lz4",    nullptr },
    { L".tlz4",  L"LZ4 TAR",       nullptr,               EK::Codec,    L"lz4",    L"lz4",    L".tar" },
    { L".lz5",   L"LZ5",           L"ShellNSE.Lz5File",   EK::Codec,    L"lz5",    L"lz5",    nullptr },
    { L".liz",   L"Lizard",        L"ShellNSE.LizFile",   EK::Codec,    L"lizard", L"lizard", nullptr },
};

const Format* Find(const wchar_t* ext)
{
    if (!ext || !*ext) return nullptr;
    for (const auto& f : kFormats)
        if (_wcsicmp(ext, f.ext) == 0) return &f;
    return nullptr;
}

bool IsArchiveExtension(const wchar_t* ext) { return Find(ext) != nullptr; }

std::wstring NameFor(const wchar_t* ext)
{
    const Format* f = Find(ext);
    return f ? f->name : L"Archive";
}

std::vector<const Format*> All()
{
    std::vector<const Format*> v;
    v.reserve(ARRAYSIZE(kFormats));
    for (const auto& f : kFormats) v.push_back(&f);
    return v;
}

std::vector<const Format*> Registrable()
{
    std::vector<const Format*> v;
    for (const auto& f : kFormats)
        if (f.progId) v.push_back(&f);
    return v;
}

std::wstring InnerNameFor(const std::wstring& archiveFileName)
{
    // Strip any directory part first — callers sometimes pass a full path.
    std::wstring name = archiveFileName;
    size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);

    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos)
        return name.empty() ? std::wstring(L"data") : name;

    // A name that is nothing but the suffix (".zst") leaves no stem to
    // reuse. Fall back to a neutral name instead of extracting a file
    // literally called ".zst".
    if (dot == 0)
        return Find(name.c_str()) ? std::wstring(L"data") : name;

    const Format* f = Find(name.c_str() + dot);
    std::wstring stem = name.substr(0, dot);

    if (f && f->unwrapTo) return stem + f->unwrapTo;   // backup.tzst -> backup.tar
    if (stem.empty())     return L"data";
    return stem;                                       // notes.txt.zst -> notes.txt
}

} // namespace Formats

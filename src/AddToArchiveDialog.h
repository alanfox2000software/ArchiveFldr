// AddToArchiveDialog.h
// ─────────────────────────────────────────────────────────────────────────
// The "Add to Archive" window (IDD_ADDTOARCHIVE): shown before files are
// compressed into an archive, the way 7-Zip and WinRAR ask. It collects
//
//   archive location        where the result is written (edit + browse)
//   archive format          zip / 7z / tar / wim — whatever the user's
//                           7z.dll can write (locked when adding to an
//                           archive that already exists: old items can
//                           only be copied by their own handler)
//   compression level       Store … Ultra
//   compression method      per format (Deflate/LZMA/… for zip,
//                           LZMA2/PPMd/… for 7z)
//   password + confirm      typed twice, with a Show box
//   encryption method       ZipCrypto / AES-256 for zip, AES-256 for 7z
//   solid archive           7z only
//   encrypt file names      7z only, needs a password
//   threads                 Auto / 1 / 2 / 4 / 8
//
// The dialog only collects; the caller does the compressing.
// ─────────────────────────────────────────────────────────────────────────
#pragma once
#include "stdafx.h"
#include "ArchiveWriter.h"

namespace AddToArchiveDialog {

struct Request
{
    std::wstring path;         // initial archive location
    std::wstring format;       // handler name ("zip", "7z", ...)
    bool         lockFormat = false;  // true when updating an existing archive
    size_t       fileCount  = 0;      // how many files are being added (caption)
};

struct Result
{
    std::wstring           path;   // where to write
    ArchiveWriter::Options opt;    // how to compress (format, level, password...)
};

// Modal. False = cancelled.
bool Show(HWND parent, const Request& rq, Result& out);

} // namespace AddToArchiveDialog

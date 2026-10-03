// LizardFrame.h
// Fallback Lizard frame reader/writer for official liblizard DLL builds.
//
// The project's liblizard.def exports the raw Lizard block API but not the
// optional LizardF_* frame API.  These helpers build and parse the standard
// 0x184D2206 frame around those raw blocks so both DLL layouts work.
#pragma once
#include "stdafx.h"

namespace LizardFrame {

enum class Result
{
    Ok,
    Unavailable,
    ReadError,
    WriteError,
    InvalidData,
    OutOfMemory,
    Cancelled,
};

using ReadFn = std::function<bool(void* buffer, size_t capacity, size_t* read)>;
using WriteFn = std::function<bool(const void* buffer, size_t size)>;
using ProgressFn = std::function<bool(uint64_t inputBytes, uint64_t outputBytes)>;

bool EncoderAvailable();
bool DecoderAvailable();
std::wstring LibraryPath();

// `blockBytes == 0` uses Lizard's 128-KB frame default. Other values are
// rounded up to a supported frame block: 128K, 256K, 1M, 4M, 16M, 64M,
// or 256M. The writer emits independent blocks and records contentSize.
Result Encode(const ReadFn& read, const WriteFn& write,
              uint64_t contentSize, int compressionLevel,
              uint64_t blockBytes, const ProgressFn& progress = {});

// Decodes both independent and linked Lizard frames. Linked frames retain
// Lizard's 16-MB rolling dictionary. `outputLimit == 0` means unlimited.
Result Decode(const ReadFn& read, const WriteFn& write,
              uint64_t outputLimit = 0,
              const ProgressFn& progress = {});

const wchar_t* ResultMessage(Result result);

} // namespace LizardFrame

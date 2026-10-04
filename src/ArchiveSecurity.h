#pragma once
#include "stdafx.h"

namespace ArchiveSecurity
{
// Wipe a password buffer before releasing its storage. std::wstring::clear()
// alone does not overwrite the old characters.
inline void SecureClear(std::wstring& value)
{
    if (!value.empty()) {
        SecureZeroMemory(&value[0], value.size() * sizeof(wchar_t));
        value.clear();
    }
    // Swap with an empty string so the old allocation is released by its
    // destructor. The bytes were already wiped above; this avoids relying
    // on shrink_to_fit(), which is only a non-binding request.
    std::wstring empty;
    value.swap(empty);
}
}

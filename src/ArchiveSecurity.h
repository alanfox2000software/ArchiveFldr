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
    value.shrink_to_fit();
}
}

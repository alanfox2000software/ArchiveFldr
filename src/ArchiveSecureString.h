#pragma once
#include "stdafx.h"

// Controlled storage for short-lived secrets. The allocation is page-backed,
// locked where the OS permits it, and wiped before release. This type is
// intentionally non-copyable and has no implicit std::wstring conversion.
class SecureWString final
{
public:
    SecureWString() = default;
    ~SecureWString() { Clear(); }
    SecureWString(const SecureWString&) = delete;
    SecureWString& operator=(const SecureWString&) = delete;

    bool Assign(const wchar_t* text, size_t count)
    {
        Clear(); if (!text && count) return false;
        const SIZE_T bytes = (count + 1) * sizeof(wchar_t);
        m_data = static_cast<wchar_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!m_data) return false;
        m_bytes = bytes; m_chars = count;
        if (count) memcpy(m_data, text, count * sizeof(wchar_t));
        m_data[count] = L'\0';
        VirtualLock(m_data, m_bytes); // best effort; failure is not fatal
        return true;
    }
    bool Assign(const std::wstring& text) { return Assign(text.data(), text.size()); }
    void Clear()
    {
        if (!m_data) return;
        SecureZeroMemory(m_data, m_bytes);
        VirtualUnlock(m_data, m_bytes);
        VirtualFree(m_data, 0, MEM_RELEASE);
        m_data = nullptr; m_bytes = m_chars = 0;
    }
    const wchar_t* Data() const { return m_data ? m_data : L""; }
    size_t Size() const { return m_chars; }
    // Explicit API-boundary conversion only. The returned copy must be
    // cleared by the caller with ArchiveSecurity::SecureClear().
    std::wstring ToWString() const { return m_data ? std::wstring(m_data, m_chars) : std::wstring(); }
    bool Empty() const { return m_chars == 0; }
private:
    wchar_t* m_data = nullptr;
    SIZE_T m_bytes = 0;
    size_t m_chars = 0;
};

// Lang.cpp — see Lang.h
#include "stdafx.h"
#include "Lang.h"

namespace Lang {

namespace {

std::unordered_map<UINT, std::wstring> g_table;
std::wstring                           g_current = L"en";

// Directory the running module lives in, with a trailing backslash.
std::wstring ModuleDir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&ModuleDir, &self);

    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(self, path, ARRAYSIZE(path))) return L"";
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return L"";
    slash[1] = 0;
    return path;
}

// Read a whole file as UTF-8 (or UTF-16 if it carries that BOM) and hand
// back wide text. Language files are small; reading them in one go keeps
// the parser from having to care about line splitting across buffers.
bool ReadTextFile(const std::wstring& path, std::wstring& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (8 << 20))
    {
        CloseHandle(h);
        return false;                       // not a language file
    }

    std::string raw((size_t)size.QuadPart, '\0');
    DWORD got = 0;
    const bool ok = raw.empty() ||
        (ReadFile(h, &raw[0], (DWORD)raw.size(), &got, nullptr) &&
         got == raw.size());
    CloseHandle(h);
    if (!ok) return false;

    // UTF-16 LE with a BOM: take it verbatim.
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF &&
                           (unsigned char)raw[1] == 0xFE)
    {
        out.assign((const wchar_t*)(raw.data() + 2),
                   (raw.size() - 2) / sizeof(wchar_t));
        return true;
    }

    size_t at = 0;
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
                           (unsigned char)raw[1] == 0xBB &&
                           (unsigned char)raw[2] == 0xBF)
        at = 3;                             // UTF-8 BOM

    const int need = MultiByteToWideChar(CP_UTF8, 0, raw.data() + at,
                                         (int)(raw.size() - at), nullptr, 0);
    if (need <= 0) { out.clear(); return raw.size() == at; }
    out.resize((size_t)need);
    MultiByteToWideChar(CP_UTF8, 0, raw.data() + at, (int)(raw.size() - at),
                        &out[0], need);
    return true;
}

void Trim(std::wstring& s)
{
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) { s.clear(); return; }
    size_t e = s.find_last_not_of(L" \t\r\n");
    s = s.substr(b, e - b + 1);
}

// "\n" and "\t" inside a quoted value; "\\" for a literal backslash.
std::wstring Unescape(const std::wstring& in)
{
    std::wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] != L'\\' || i + 1 == in.size()) { out += in[i]; continue; }
        switch (in[++i])
        {
        case L'n': out += L'\n'; break;
        case L't': out += L'\t'; break;
        case L'r': out += L'\r'; break;
        default:   out += in[i]; break;
        }
    }
    return out;
}

// One "<id> = "text"" line. Comments start with ';' and blank lines are
// skipped. An unquoted value is taken as-is to the end of the line, so a
// translator who forgets the quotes still gets their string.
void ParseInto(const std::wstring& text,
               std::unordered_map<UINT, std::wstring>& table)
{
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = text.substr(pos, eol - pos);
        pos = eol + 1;

        Trim(line);
        if (line.empty() || line[0] == L';') continue;

        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;

        std::wstring key = line.substr(0, eq);
        std::wstring val = line.substr(eq + 1);
        Trim(key);
        Trim(val);
        if (key.empty()) continue;

        // The id may be plain or zero-padded ("00000000"); both are
        // decimal, which is what 7-Zip's files use.
        wchar_t* end = nullptr;
        const unsigned long id = wcstoul(key.c_str(), &end, 10);
        if (!end || *end) continue;         // not a number: ignore the line

        if (val.size() >= 2 && val.front() == L'"')
        {
            const size_t close = val.rfind(L'"');
            val = (close > 0) ? val.substr(1, close - 1) : val.substr(1);
        }
        table[(UINT)id] = Unescape(val);
    }
}

} // namespace

std::wstring Dir() { return ModuleDir() + L"Lang\\"; }

const std::wstring& Current() { return g_current; }

bool Load(const std::wstring& code)
{
    g_table.clear();
    g_current = code.empty() ? L"en" : code;

    std::wstring text;
    if (!ReadTextFile(Dir() + g_current + L".txt", text)) return false;
    ParseInto(text, g_table);
    return true;
}

std::wstring Str(UINT id, const wchar_t* fallback)
{
    auto it = g_table.find(id);
    if (it != g_table.end() && !it->second.empty()) return it->second;
    return fallback ? fallback : L"";
}

std::wstring Format1(UINT id, const wchar_t* fallback, const std::wstring& arg)
{
    std::wstring t = Str(id, fallback);
    const size_t at = t.find(L"%s");
    if (at == std::wstring::npos) return t;    // translator dropped it
    t.replace(at, 2, arg);
    return t;
}

void Apply(HWND hDlg, UINT dlgId)
{
    if (!hDlg || g_table.empty()) return;

    auto it = g_table.find(dlgId);
    if (it != g_table.end() && !it->second.empty())
        SetWindowTextW(hDlg, it->second.c_str());

    // Walk the children rather than the table: a table entry whose id is
    // not on this dialog belongs to another page, and SetDlgItemText on a
    // missing id is a silent no-op that would hide typos.
    for (HWND child = GetWindow(hDlg, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT))
    {
        const int id = GetDlgCtrlID(child);
        if (id <= 0) continue;              // -1 is IDC_STATIC, shared

        // Language-file id 1 is metadata: the language's English name.
        // Win32 also defines IDOK as 1, so applying it to dialog children
        // renamed every standard OK button to "English" (visible in the
        // Add to Archive screenshot). Standard buttons keep the caption
        // from the dialog resource; translated dialogs use dedicated ids.
        if (id == IDOK) continue;

        auto e = g_table.find((UINT)id);
        if (e != g_table.end() && !e->second.empty())
            SetWindowTextW(child, e->second.c_str());
    }
}

std::vector<Entry> Available()
{
    std::vector<Entry> out;

    const std::wstring dir = Dir();
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            std::wstring stem = fd.cFileName;
            const size_t dot = stem.find_last_of(L'.');
            if (dot != std::wstring::npos) stem.resize(dot);
            if (stem.empty()) continue;

            std::wstring text;
            if (!ReadTextFile(dir + fd.cFileName, text)) continue;

            std::unordered_map<UINT, std::wstring> t;
            ParseInto(text, t);

            Entry e;
            e.code    = stem;
            e.native  = t.count(0) ? t[0] : stem;
            e.english = t.count(1) ? t[1] : e.native;
            out.push_back(std::move(e));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // English is always offered, even with no Lang folder at all: the
    // dialog templates are English, so selecting it is always honest.
    if (std::find_if(out.begin(), out.end(),
            [](const Entry& e) { return _wcsicmp(e.code.c_str(), L"en") == 0; })
        == out.end())
        out.push_back({ L"en", L"English", L"English" });

    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        const bool ae = _wcsicmp(a.code.c_str(), L"en") == 0;
        const bool be = _wcsicmp(b.code.c_str(), L"en") == 0;
        if (ae != be) return ae;            // English first
        return _wcsicmp(a.english.c_str(), b.english.c_str()) < 0;
    });
    return out;
}

} // namespace Lang

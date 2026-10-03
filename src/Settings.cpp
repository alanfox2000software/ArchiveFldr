// Settings.cpp
#include "stdafx.h"
#include "Settings.h"
#include "Formats.h"
#include "GUIDs.h"

Settings::Settings()
{
    // Set tempDirPath and logFilePath defaults at runtime
    wchar_t buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf))) {
        tempDirPath = std::wstring(buf) + L"\\ArchiveFldr\\Temp";
        logFilePath = std::wstring(buf) + L"\\ArchiveFldr\\ArchiveFldr.log";
    }
}

// ── Registry helpers ──────────────────────────────────────
DWORD Settings::ReadDword(HKEY hk, const wchar_t* n, DWORD def) {
    DWORD v = def, sz = sizeof(v), type = 0;
    // Check the type as well as the result: a value of some other type
    // copies its first four bytes into v and would be read as a number.
    if (RegQueryValueExW(hk, n, nullptr, &type, (BYTE*)&v, &sz)
            != ERROR_SUCCESS || type != REG_DWORD || sz != sizeof(v))
        return def;
    return v;
}
bool Settings::ReadBool(HKEY hk, const wchar_t* n, bool def) {
    return ReadDword(hk, n, def ? 1 : 0) != 0;
}
std::wstring Settings::ReadStr(HKEY hk, const wchar_t* n, const wchar_t* def) {
    // One byte of slack and an explicit length.
    //
    // RegQueryValueEx does not promise a terminator — a REG_SZ written
    // by something that did not count one is handed back exactly as
    // stored — so constructing a std::wstring from the buffer could run
    // off the end of it. Build the string from the byte count instead,
    // and keep a spare element so a value that fills the buffer still
    // has somewhere to put the NUL.
    wchar_t buf[MAX_PATH * 2 + 1] = {};
    DWORD sz = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    if (RegQueryValueExW(hk, n, nullptr, &type, (BYTE*)buf, &sz)
            == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ))
    {
        size_t len = sz / sizeof(wchar_t);
        buf[len] = L'\0';
        while (len && buf[len - 1] == L'\0') --len;   // stored terminator
        return std::wstring(buf, len);
    }
    return def ? def : L"";
}
void Settings::WriteDword(HKEY hk, const wchar_t* n, DWORD v) {
    RegSetValueExW(hk, n, 0, REG_DWORD, (BYTE*)&v, sizeof(v));
}
void Settings::WriteBool(HKEY hk, const wchar_t* n, bool v) {
    DWORD dw = v ? 1 : 0;
    RegSetValueExW(hk, n, 0, REG_DWORD, (BYTE*)&dw, sizeof(dw));
}
void Settings::WriteStr(HKEY hk, const wchar_t* n, const std::wstring& v) {
    RegSetValueExW(hk, n, 0, REG_SZ,
        (BYTE*)v.c_str(), (DWORD)((v.size()+1)*sizeof(wchar_t)));
}

// A ";"-separated extension list -- of the formats the user switched
// OFF, not the ones left on.
//
// Storing the ticked set quietly broke every time a format was added to
// the table: an extension that did not exist when the list was last
// saved reads back as "not wanted", so new formats arrived switched off
// and invisible, and nothing said why. Recording the exclusions instead
// means the default is always "everything this build knows about", and
// only a deliberate untick survives an upgrade.
static bool ReadExtList(HKEY hk, const wchar_t* value,
                        std::set<std::wstring>& out)
{
    wchar_t buf[8192 + 1] = {};
    DWORD sz = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    out.clear();
    if (RegQueryValueExW(hk, value, nullptr, &type, (BYTE*)buf, &sz)
            != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ))
        return false;

    // Not necessarily NUL-terminated in the registry. See Settings::ReadStr.
    size_t chars = sz / sizeof(wchar_t);
    buf[chars] = L'\0';
    while (chars && buf[chars - 1] == L'\0') --chars;

    const std::wstring packed(buf, chars);
    size_t at = 0;
    while (at <= packed.size())
    {
        size_t sep = packed.find(L';', at);
        if (sep == std::wstring::npos) sep = packed.size();
        if (sep > at)
        {
            std::wstring e = packed.substr(at, sep - at);
            if (!e.empty() && e[0] != L'.') e = L"." + e;
            for (auto& ch : e) ch = (wchar_t)towlower(ch);
            out.insert(e);
        }
        if (sep == packed.size()) break;
        at = sep + 1;
    }
    return true;
}

static void LoadAssoc(HKEY hk, const wchar_t* offValue,
                      std::set<std::wstring>& out)
{
    // Start from everything this build can register, lower-cased.
    // Everyone who looks a type up in these sets — ReadExtList above,
    // PackAssocOff below, CRegistry::ExtensionIsWanted — lower-cases
    // first, so storing an extension any other way would make it
    // unfindable by all of them.
    out.clear();
    for (const auto* f : Formats::Registrable())
    {
        std::wstring e = f->ext;
        for (auto& ch : e) ch = (wchar_t)towlower(ch);
        out.insert(e);
    }

    // ...and take away what the user turned off. Absent means nothing
    // was turned off, which is the same answer a fresh install gives.
    std::set<std::wstring> off;
    if (!ReadExtList(hk, offValue, off)) return;
    for (const auto& e : off) out.erase(e);
}

// The complement, for writing: what this build can register, minus what
// is ticked.
static std::wstring PackAssocOff(const std::set<std::wstring>& ticked)
{
    std::wstring packed;
    for (const auto* f : Formats::Registrable())
    {
        std::wstring e = f->ext;
        for (auto& ch : e) ch = (wchar_t)towlower(ch);
        if (ticked.find(e) != ticked.end()) continue;
        if (!packed.empty()) packed += L';';
        packed += e;
    }
    return packed;
}

// ── Load ──────────────────────────────────────────────────
void Settings::Load()
{
    HKEY hk = nullptr;
    // KEY_WOW64_64KEY, always. These settings are machine-wide and say
    // nothing about the bitness of whoever is reading them -- the two
    // association lists are separate values in this one key, not
    // separate keys. Without the flag a 32-bit process is redirected to
    // SOFTWARE\WOW6432Node\ArchiveFldr and sees an empty key, so the
    // 32-bit DLL would read defaults while the 64-bit settings program
    // wrote the real thing somewhere it could never look. On 32-bit
    // Windows the flag is ignored, which is exactly what we want.
    //
    // Opened, not created: this is the read path, and every caller of it
    // is a shell extension running in somebody else's process. Creating
    // an HKLM key needs rights that an ordinary Explorer does not have,
    // so the create could only ever fail there — and where it did
    // succeed (an elevated host) it left an empty key behind as a side
    // effect of reading. A missing key simply means "defaults".
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegKeySettings,
        0, KEY_READ | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS)
        return;

    language            = ReadStr (hk, L"Language",        language.c_str());

    // General
    showPreviewPane     = ReadBool(hk, L"ShowPreview",      showPreviewPane);
    showThumbnails      = ReadBool(hk, L"ShowThumbnails",   showThumbnails);
    showContextMenu     = ReadBool(hk, L"ShowContextMenu",  showContextMenu);
    openArchiveOnDblClk = ReadBool(hk, L"OpenOnDblClk",    openArchiveOnDblClk);
    promptForPath       = ReadBool(hk, L"PromptPath",       promptForPath);
    rememberLastPath    = ReadBool(hk, L"RememberPath",     rememberLastPath);
    defaultExtractPath  = ReadStr (hk, L"ExtractPath",      defaultExtractPath.c_str());
    defaultFormat       = ReadStr (hk, L"DefaultFormat",    defaultFormat.c_str());
    defaultCompLevel    = (CompLevel)ReadDword(hk, L"CompLevel", (DWORD)defaultCompLevel);
    createSolidArchive  = ReadBool(hk, L"SolidArchive",    createSolidArchive);
    encryptFileNames    = ReadBool(hk, L"EncryptNames",     encryptFileNames);

    // Formats. Two REG_SZ values, each a ";"-separated list of the
    // extensions the user switched OFF; absent means none of them, so a
    // build that adds a format has it ticked by default instead of
    // silently inheriting "not in the list, therefore not wanted".
    LoadAssoc(hk, L"Associations32Off", assoc32);
    LoadAssoc(hk, L"Associations64Off", assoc64);
    registerAsDefaultApp = ReadBool(hk, L"RegisterAsDefaultApp", registerAsDefaultApp);

    // Context menu
    ctxExtract       = ReadBool(hk, L"CtxExtract",      ctxExtract);
    ctxExtractHere   = ReadBool(hk, L"CtxExtractHere",  ctxExtractHere);
    ctxAddToArchive  = ReadBool(hk, L"CtxAdd",          ctxAddToArchive);
    ctxCompressHere  = ReadBool(hk, L"CtxCompressHere",  ctxCompressHere);
    ctxCompressEmail = ReadBool(hk, L"CtxEmail",        ctxCompressEmail);
    ctxOpenInShell   = ReadBool(hk, L"CtxOpen",         ctxOpenInShell);
    ctxTestArchive   = ReadBool(hk, L"CtxTest",         ctxTestArchive);
    ctxArchiveInfo   = ReadBool(hk, L"CtxInfo",         ctxArchiveInfo);
    ctxSettings      = ReadBool(hk, L"CtxSettings",     ctxSettings);
    ctxUseSubMenu    = ReadBool(hk, L"CtxSubmenu",      ctxUseSubMenu);
    ctxMenuIcons     = ReadBool(hk, L"CtxMenuIcons",    ctxMenuIcons);
    ctxSubMenuTitle  = ReadStr (hk, L"CtxSubmenuTitle", ctxSubMenuTitle.c_str());

    // One per bitness, both defaulting to true: absent means "nobody has
    // said otherwise", which has to keep meaning "register the context
    // menu" or an upgrade would silently take the right-click menu away
    // from every existing install. See Settings.h.
    ctxMenu32        = ReadBool(hk, L"ContextMenu32",   ctxMenu32);
    ctxMenu64        = ReadBool(hk, L"ContextMenu64",   ctxMenu64);

    // Appearance
    showSizeColumn    = ReadBool (hk, L"ColSize",         showSizeColumn);
    showDateColumn    = ReadBool (hk, L"ColDate",         showDateColumn);
    showRatioColumn   = ReadBool (hk, L"ColRatio",        showRatioColumn);
    showMethodColumn  = ReadBool (hk, L"ColMethod",       showMethodColumn);
    showCrcColumn     = ReadBool (hk, L"ColCrc",          showCrcColumn);
    alternateRowColors= ReadBool (hk, L"AltRows",         alternateRowColors);
    dateFormat        = (DateFmt)ReadDword(hk, L"DateFmt",(DWORD)dateFormat);
    fontFace          = ReadStr  (hk, L"FontFace",        fontFace.c_str());
    fontSize          = (int)ReadDword(hk, L"FontSize",   (DWORD)fontSize);

    // Advanced
    multiThreaded   = ReadBool (hk, L"MultiThread",    multiThreaded);
    threadCount     = (int)ReadDword(hk, L"Threads",   (DWORD)threadCount);
    useTempDir      = ReadBool (hk, L"UseTempDir",     useTempDir);
    tempDirPath     = ReadStr  (hk, L"TempDir",        tempDirPath.c_str());
    logErrors       = ReadBool (hk, L"LogErrors",      logErrors);
    logFilePath     = ReadStr  (hk, L"LogFile",        logFilePath.c_str());

    RegCloseKey(hk);
}

// ── Save ──────────────────────────────────────────────────
void Settings::Save() const
{
    HKEY hk = nullptr;
    // 64-bit view, to match Load(). See the note there.
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegKeySettings,
        0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY,
        nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return;

    // General
    WriteBool (hk, L"ShowPreview",     showPreviewPane);
    WriteBool (hk, L"ShowThumbnails",  showThumbnails);
    WriteBool (hk, L"ShowContextMenu", showContextMenu);
    WriteBool (hk, L"OpenOnDblClk",   openArchiveOnDblClk);
    WriteBool (hk, L"PromptPath",      promptForPath);
    WriteBool (hk, L"RememberPath",    rememberLastPath);
    WriteStr  (hk, L"ExtractPath",     defaultExtractPath);
    WriteStr  (hk, L"DefaultFormat",   defaultFormat);
    WriteDword(hk, L"CompLevel",       (DWORD)defaultCompLevel);
    WriteBool (hk, L"SolidArchive",    createSolidArchive);
    WriteBool (hk, L"EncryptNames",    encryptFileNames);

    // Formats
    WriteStr(hk, L"Associations32Off", PackAssocOff(assoc32));
    WriteStr(hk, L"Associations64Off", PackAssocOff(assoc64));

    // The old ticked-set values cannot be interpreted safely -- they
    // cannot tell a format the user switched off from one that did not
    // exist when they were written -- so they are removed rather than
    // left behind to be misread by something later.
    RegDeleteValueW(hk, L"Associations32");
    RegDeleteValueW(hk, L"Associations64");
    WriteBool(hk, L"RegisterAsDefaultApp", registerAsDefaultApp);

    // Language
    WriteStr (hk, L"Language", language);

    // Context menu
    WriteBool(hk, L"CtxExtract",      ctxExtract);
    WriteBool(hk, L"CtxExtractHere",  ctxExtractHere);
    WriteBool(hk, L"CtxAdd",          ctxAddToArchive);
    WriteBool(hk, L"CtxCompressHere", ctxCompressHere);
    WriteBool(hk, L"CtxEmail",        ctxCompressEmail);
    WriteBool(hk, L"CtxOpen",         ctxOpenInShell);
    WriteBool(hk, L"CtxTest",         ctxTestArchive);
    WriteBool(hk, L"CtxInfo",         ctxArchiveInfo);
    WriteBool(hk, L"CtxSettings",     ctxSettings);
    WriteBool(hk, L"CtxSubmenu",      ctxUseSubMenu);
    WriteBool(hk, L"CtxMenuIcons",    ctxMenuIcons);
    WriteStr (hk, L"CtxSubmenuTitle", ctxSubMenuTitle);
    WriteBool(hk, L"ContextMenu32",   ctxMenu32);
    WriteBool(hk, L"ContextMenu64",   ctxMenu64);

    // Appearance
    WriteBool (hk, L"ColSize",     showSizeColumn);
    WriteBool (hk, L"ColDate",     showDateColumn);
    WriteBool (hk, L"ColRatio",    showRatioColumn);
    WriteBool (hk, L"ColMethod",   showMethodColumn);
    WriteBool (hk, L"ColCrc",      showCrcColumn);
    WriteBool (hk, L"AltRows",     alternateRowColors);
    WriteDword(hk, L"DateFmt",     (DWORD)dateFormat);
    WriteStr  (hk, L"FontFace",    fontFace);
    WriteDword(hk, L"FontSize",    (DWORD)fontSize);

    // Advanced
    WriteBool (hk, L"MultiThread", multiThreaded);
    WriteDword(hk, L"Threads",     (DWORD)threadCount);
    WriteBool (hk, L"UseTempDir",  useTempDir);
    WriteStr  (hk, L"TempDir",     tempDirPath);
    WriteBool (hk, L"LogErrors",   logErrors);
    WriteStr  (hk, L"LogFile",     logFilePath);

    RegCloseKey(hk);
}

void Settings::Reset()
{
    // ── General ──────────────────────────────────────────
    showPreviewPane      = true;
    showThumbnails       = true;
    showContextMenu      = true;
    openArchiveOnDblClk  = true;
    promptForPath        = true;
    rememberLastPath     = true;
    defaultExtractPath.clear();
    defaultFormat        = L"zip";
    defaultCompLevel     = CompLevel::Normal;
    createSolidArchive   = false;
    encryptFileNames     = false;

    // ── Formats ───────────────────────────────────────────
    assoc32.clear();
    assoc64.clear();
    for (const auto* f : Formats::Registrable())
    {
        assoc32.insert(f->ext);
        assoc64.insert(f->ext);
    }
    registerAsDefaultApp = false;

    // ── Context menu ──────────────────────────────────────
    ctxExtract       = true;
    ctxExtractHere   = true;
    ctxCompressHere  = true;
    ctxMenuIcons     = true;
    language         = L"en";
    ctxAddToArchive  = true;
    ctxCompressEmail = true;
    ctxOpenInShell   = true;
    ctxTestArchive   = true;
    ctxArchiveInfo   = true;
    ctxSettings      = true;
    ctxUseSubMenu    = true;
    ctxSubMenuTitle  = L"ArchiveFldr";
    ctxMenu32        = true;
    ctxMenu64        = true;

    // ── Appearance ────────────────────────────────────────
    showSizeColumn    = true;
    showDateColumn    = true;
    showRatioColumn   = true;
    showMethodColumn  = true;
    showCrcColumn     = false;
    alternateRowColors= true;
    dateFormat        = DateFmt::ISO8601;
    fontFace          = L"Segoe UI";
    fontSize          = 9;

    // ── Advanced ──────────────────────────────────────────
    multiThreaded    = true;
    threadCount      = 0;
    useTempDir       = false;
    logErrors        = true;

    // Restore runtime-computed paths
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA,
                                   nullptr, 0, buf)))
    {
        tempDirPath = std::wstring(buf) + L"\\ArchiveFldr\\Temp";
        logFilePath = std::wstring(buf) + L"\\ArchiveFldr\\ArchiveFldr.log";
    }

    Save();
}
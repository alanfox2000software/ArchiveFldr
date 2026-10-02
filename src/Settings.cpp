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
    DWORD v = def, sz = sizeof(v);
    RegQueryValueExW(hk, n, nullptr, nullptr, (BYTE*)&v, &sz);
    return v;
}
bool Settings::ReadBool(HKEY hk, const wchar_t* n, bool def) {
    return ReadDword(hk, n, def ? 1 : 0) != 0;
}
std::wstring Settings::ReadStr(HKEY hk, const wchar_t* n, const wchar_t* def) {
    wchar_t buf[MAX_PATH*2] = {};
    DWORD sz = sizeof(buf);
    if (RegQueryValueExW(hk, n, nullptr, nullptr, (BYTE*)buf, &sz) == ERROR_SUCCESS)
        return buf;
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

// A ";"-separated extension list. The sentinel means "never written",
// in which case the default is every registrable format -- the same list
// registration itself uses, so a fresh install claims what it says it
// claims rather than nothing.
static void LoadAssoc(HKEY hk, const wchar_t* value,
                      std::set<std::wstring>& out)
{
    wchar_t buf[8192] = {};
    DWORD sz = sizeof(buf);
    out.clear();
    if (RegQueryValueExW(hk, value, nullptr, nullptr, (BYTE*)buf, &sz)
            != ERROR_SUCCESS)
    {
        for (const auto* f : Formats::Registrable()) out.insert(f->ext);
        return;
    }

    const std::wstring packed = buf;
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
}

static std::wstring PackAssoc(const std::set<std::wstring>& in)
{
    std::wstring packed;
    for (const auto& e : in)
    {
        if (!packed.empty()) packed += L';';
        packed += e;
    }
    return packed;
}

// ── Load ──────────────────────────────────────────────────
void Settings::Load()
{
    HKEY hk = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegKeySettings,
        0, nullptr, 0, KEY_READ, nullptr, &hk, nullptr) != ERROR_SUCCESS)
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

    // Formats. Two REG_SZ values, each a ";"-separated extension list;
    // absent means "never written", which defaults to every registrable
    // format -- the same list registration itself uses.
    LoadAssoc(hk, L"Associations32", assoc32);
    LoadAssoc(hk, L"Associations64", assoc64);
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
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegKeySettings,
        0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS)
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
    WriteStr(hk, L"Associations32", PackAssoc(assoc32));
    WriteStr(hk, L"Associations64", PackAssoc(assoc64));
    WriteBool(hk, L"RegisterAsDefaultApp", registerAsDefaultApp);

    // Context menu
    WriteStr (hk, L"Language",        language);
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
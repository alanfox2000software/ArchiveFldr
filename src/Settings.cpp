// Settings.cpp
#include "stdafx.h"
#include "Settings.h"
#include "GUIDs.h"

Settings::Settings()
{
    // Set tempDirPath and logFilePath defaults at runtime
    wchar_t buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf))) {
        tempDirPath = std::wstring(buf) + L"\\ShellNSE\\Temp";
        logFilePath = std::wstring(buf) + L"\\ShellNSE\\ShellNSE.log";
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

// ── Load ──────────────────────────────────────────────────
void Settings::Load()
{
    HKEY hk = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKeySettings,
        0, nullptr, 0, KEY_READ, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return;

    // General
    showPreviewPane     = ReadBool(hk, L"ShowPreview",      showPreviewPane);
    showThumbnails      = ReadBool(hk, L"ShowThumbnails",   showThumbnails);
    showIconOverlay     = ReadBool(hk, L"ShowOverlay",      showIconOverlay);
    showContextMenu     = ReadBool(hk, L"ShowContextMenu",  showContextMenu);
    openArchiveOnDblClk = ReadBool(hk, L"OpenOnDblClk",    openArchiveOnDblClk);
    promptForPath       = ReadBool(hk, L"PromptPath",       promptForPath);
    rememberLastPath    = ReadBool(hk, L"RememberPath",     rememberLastPath);
    defaultExtractPath  = ReadStr (hk, L"ExtractPath",      defaultExtractPath.c_str());
    defaultFormat       = ReadStr (hk, L"DefaultFormat",    defaultFormat.c_str());
    defaultCompLevel    = (CompLevel)ReadDword(hk, L"CompLevel", (DWORD)defaultCompLevel);
    createSolidArchive  = ReadBool(hk, L"SolidArchive",    createSolidArchive);
    encryptFileNames    = ReadBool(hk, L"EncryptNames",     encryptFileNames);
    autoCloseAfterOp    = ReadBool(hk, L"AutoClose",        autoCloseAfterOp);

    // Formats
    handleZip    = ReadBool(hk, L"FmtZip",    handleZip);
    handle7z     = ReadBool(hk, L"Fmt7z",     handle7z);
    handleRar    = ReadBool(hk, L"FmtRar",    handleRar);
    handleTar    = ReadBool(hk, L"FmtTar",    handleTar);
    handleGz     = ReadBool(hk, L"FmtGz",     handleGz);
    handleBz2    = ReadBool(hk, L"FmtBz2",    handleBz2);
    handleXz     = ReadBool(hk, L"FmtXz",     handleXz);
    handleLzma   = ReadBool(hk, L"FmtLzma",   handleLzma);
    handleZst    = ReadBool(hk, L"FmtZst",    handleZst);
    handleIso    = ReadBool(hk, L"FmtIso",    handleIso);
    handleCab    = ReadBool(hk, L"FmtCab",    handleCab);
    handleLzh    = ReadBool(hk, L"FmtLzh",    handleLzh);
    handleArj    = ReadBool(hk, L"FmtArj",    handleArj);
    handleWim    = ReadBool(hk, L"FmtWim",    handleWim);
    handleMsi    = ReadBool(hk, L"FmtMsi",    handleMsi);
    handleOffice = ReadBool(hk, L"FmtOffice", handleOffice);

    // Context menu
    ctxExtract       = ReadBool(hk, L"CtxExtract",      ctxExtract);
    ctxExtractHere   = ReadBool(hk, L"CtxExtractHere",  ctxExtractHere);
    ctxAddToArchive  = ReadBool(hk, L"CtxAdd",          ctxAddToArchive);
    ctxCompressEmail = ReadBool(hk, L"CtxEmail",        ctxCompressEmail);
    ctxOpenInShell   = ReadBool(hk, L"CtxOpen",         ctxOpenInShell);
    ctxTestArchive   = ReadBool(hk, L"CtxTest",         ctxTestArchive);
    ctxArchiveInfo   = ReadBool(hk, L"CtxInfo",         ctxArchiveInfo);
    ctxSettings      = ReadBool(hk, L"CtxSettings",     ctxSettings);
    ctxUseSubMenu    = ReadBool(hk, L"CtxSubmenu",      ctxUseSubMenu);
    ctxSubMenuTitle  = ReadStr (hk, L"CtxSubmenuTitle", ctxSubMenuTitle.c_str());

    // Appearance
    darkMode          = ReadBool (hk, L"DarkMode",        darkMode);
    useCustomIcons    = ReadBool (hk, L"CustomIcons",     useCustomIcons);
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
    checkForUpdates = ReadBool (hk, L"CheckUpdates",   checkForUpdates);
    sendUsageData   = ReadBool (hk, L"SendUsage",      sendUsageData);
    maxMemoryMB     = (int)ReadDword(hk, L"MaxMemMB",  (DWORD)maxMemoryMB);
    cacheThumbnails = ReadBool (hk, L"CacheThumbs",    cacheThumbnails);
    cacheSizeMB     = (int)ReadDword(hk, L"CacheMB",   (DWORD)cacheSizeMB);

    RegCloseKey(hk);
}

// ── Save ──────────────────────────────────────────────────
void Settings::Save() const
{
    HKEY hk = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKeySettings,
        0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return;

    // General
    WriteBool (hk, L"ShowPreview",     showPreviewPane);
    WriteBool (hk, L"ShowThumbnails",  showThumbnails);
    WriteBool (hk, L"ShowOverlay",     showIconOverlay);
    WriteBool (hk, L"ShowContextMenu", showContextMenu);
    WriteBool (hk, L"OpenOnDblClk",   openArchiveOnDblClk);
    WriteBool (hk, L"PromptPath",      promptForPath);
    WriteBool (hk, L"RememberPath",    rememberLastPath);
    WriteStr  (hk, L"ExtractPath",     defaultExtractPath);
    WriteStr  (hk, L"DefaultFormat",   defaultFormat);
    WriteDword(hk, L"CompLevel",       (DWORD)defaultCompLevel);
    WriteBool (hk, L"SolidArchive",    createSolidArchive);
    WriteBool (hk, L"EncryptNames",    encryptFileNames);
    WriteBool (hk, L"AutoClose",       autoCloseAfterOp);

    // Formats
    WriteBool(hk, L"FmtZip",    handleZip);
    WriteBool(hk, L"Fmt7z",     handle7z);
    WriteBool(hk, L"FmtRar",    handleRar);
    WriteBool(hk, L"FmtTar",    handleTar);
    WriteBool(hk, L"FmtGz",     handleGz);
    WriteBool(hk, L"FmtBz2",    handleBz2);
    WriteBool(hk, L"FmtXz",     handleXz);
    WriteBool(hk, L"FmtLzma",   handleLzma);
    WriteBool(hk, L"FmtZst",    handleZst);
    WriteBool(hk, L"FmtIso",    handleIso);
    WriteBool(hk, L"FmtCab",    handleCab);
    WriteBool(hk, L"FmtLzh",    handleLzh);
    WriteBool(hk, L"FmtArj",    handleArj);
    WriteBool(hk, L"FmtWim",    handleWim);
    WriteBool(hk, L"FmtMsi",    handleMsi);
    WriteBool(hk, L"FmtOffice", handleOffice);

    // Context menu
    WriteBool(hk, L"CtxExtract",      ctxExtract);
    WriteBool(hk, L"CtxExtractHere",  ctxExtractHere);
    WriteBool(hk, L"CtxAdd",          ctxAddToArchive);
    WriteBool(hk, L"CtxEmail",        ctxCompressEmail);
    WriteBool(hk, L"CtxOpen",         ctxOpenInShell);
    WriteBool(hk, L"CtxTest",         ctxTestArchive);
    WriteBool(hk, L"CtxInfo",         ctxArchiveInfo);
    WriteBool(hk, L"CtxSettings",     ctxSettings);
    WriteBool(hk, L"CtxSubmenu",      ctxUseSubMenu);
    WriteStr (hk, L"CtxSubmenuTitle", ctxSubMenuTitle);

    // Appearance
    WriteBool (hk, L"DarkMode",    darkMode);
    WriteBool (hk, L"CustomIcons", useCustomIcons);
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
    WriteBool (hk, L"CheckUpdates",checkForUpdates);
    WriteBool (hk, L"SendUsage",   sendUsageData);
    WriteDword(hk, L"MaxMemMB",    (DWORD)maxMemoryMB);
    WriteBool (hk, L"CacheThumbs", cacheThumbnails);
    WriteDword(hk, L"CacheMB",     (DWORD)cacheSizeMB);

    RegCloseKey(hk);
}

void Settings::Reset()
{
    *this = Settings();
    Save();
}
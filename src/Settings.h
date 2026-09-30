// Settings.h — Persistent settings for ShellNSE (registry-backed)
#pragma once
#include "stdafx.h"

// ── Compression Levels ────────────────────────────────────
enum class CompLevel : int {
    Store   = 0,
    Fastest = 1,
    Fast    = 3,
    Normal  = 5,
    Maximum = 7,
    Ultra   = 9
};

// ── Default Extract Path Mode ─────────────────────────────
enum class ExtractPathMode : int {
    AskUser        = 0,
    SameAsArchive  = 1,
    CustomPath     = 2,
    Desktop        = 3,
    Downloads      = 4
};

// ── Date Format ───────────────────────────────────────────
enum class DateFmt : int {
    ISO8601    = 0,   // 2024-12-31 23:59
    DDMMYYYY   = 1,   // 31/12/2024 23:59
    MMDDYYYY   = 2,   // 12/31/2024 11:59 PM
    Relative   = 3    // 2 hours ago
};

// ─────────────────────────────────────────────────────────
// Settings — Singleton holding all user preferences
// ─────────────────────────────────────────────────────────
class Settings
{
public:
    static Settings& Get() noexcept {
        static Settings s_instance;
        return s_instance;
    }

    // ── Persistence ───────────────────────────────────────
    void Load();
    void Save() const;
    void Reset();

    // ── General ───────────────────────────────────────────
    bool          showPreviewPane      = true;
    bool          showThumbnails       = true;
    bool          showIconOverlay      = true;
    bool          showContextMenu      = true;
    bool          openArchiveOnDblClk  = true;   // true=open, false=extract
    bool          promptForPath        = true;
    bool          rememberLastPath     = true;
    std::wstring  defaultExtractPath;
    std::wstring  defaultFormat        = L"zip";
    CompLevel     defaultCompLevel     = CompLevel::Normal;
    bool          createSolidArchive   = false;
    bool          encryptFileNames     = false;
    bool          autoCloseAfterOp     = false;

    // ── Formats (which extensions to handle) ─────────────
    std::unordered_set<std::wstring> enabledFormats;
    bool          handleZip    = true;
    bool          handle7z     = true;
    bool          handleRar    = true;   // extract only
    bool          handleTar    = true;
    bool          handleGz     = true;
    bool          handleBz2    = true;
    bool          handleXz     = true;
    bool          handleLzma   = true;
    bool          handleZst    = true;
    bool          handleIso    = true;
    bool          handleCab    = true;
    bool          handleLzh    = true;
    bool          handleArj    = true;   // extract only
    bool          handleWim    = true;
    bool          handleMsi    = false;
    bool          handleOffice = true;   // .docx .xlsx etc

    // ── Context Menu Items ────────────────────────────────
    bool          ctxExtract          = true;
    bool          ctxExtractHere      = true;
    bool          ctxAddToArchive     = true;
    bool          ctxCompressEmail    = true;
    bool          ctxOpenInShell      = true;
    bool          ctxTestArchive      = true;
    bool          ctxArchiveInfo      = true;
    bool          ctxSettings         = true;
    bool          ctxUseSubMenu       = true;
    std::wstring  ctxSubMenuTitle     = L"ShellNSE";

    // ── Appearance ────────────────────────────────────────
    bool          darkMode            = false;
    bool          useCustomIcons      = true;
    bool          showSizeColumn      = true;
    bool          showDateColumn      = true;
    bool          showRatioColumn     = true;
    bool          showMethodColumn    = true;
    bool          showCrcColumn       = false;
    bool          alternateRowColors  = true;
    DateFmt       dateFormat          = DateFmt::ISO8601;
    std::wstring  fontFace            = L"Segoe UI";
    int           fontSize            = 9;

    // ── Advanced ──────────────────────────────────────────
    bool          multiThreaded       = true;
    int           threadCount         = 0;   // 0 = auto (CPU count)
    bool          useTempDir          = false;
    std::wstring  tempDirPath;
    bool          logErrors           = true;
    std::wstring  logFilePath;
    bool          checkForUpdates     = true;
    bool          sendUsageData       = false;
    int           maxMemoryMB         = 256;
    bool          cacheThumbnails     = true;
    int           cacheSizeMB         = 128;

private:
    Settings();
    ~Settings() = default;
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    // Registry helpers
    static DWORD  ReadDword (HKEY, const wchar_t* name, DWORD def);
    static bool   ReadBool  (HKEY, const wchar_t* name, bool  def);
    static std::wstring ReadStr(HKEY, const wchar_t* name, const wchar_t* def=L"");
    static void   WriteDword(HKEY, const wchar_t* name, DWORD val);
    static void   WriteBool (HKEY, const wchar_t* name, bool  val);
    static void   WriteStr  (HKEY, const wchar_t* name, const std::wstring& val);
};
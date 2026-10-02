// Settings.h — Persistent settings for ArchiveFldr (registry-backed)
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

// ── Working folder ────────────────────────────────────────
// Where an extract-to-temp lands before the shell copies it out.
enum class WorkDirMode : int {
    SystemTemp = 0,   // whatever GetTempPath says
    Specified  = 1    // workDirPath
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
    // Loaded on first use. This used to be driven from DllMain, which is
    // not allowed to touch the registry (loader lock), and meant every
    // short-lived shell host paid for it whether it read a setting or not.
    static Settings& Get() noexcept {
        static Settings s_instance;
        static const bool s_loaded = [] {
            s_instance.Load();
            return true;
        }();
        (void)s_loaded;
        return s_instance;
    }

    // ── Persistence ───────────────────────────────────────
    void Load();
    void Save() const;
    void Reset();

    // ── Language ──────────────────────────────────────────
    // File stem under Lang\, so "en" means Lang\en.txt.
    std::wstring  language             = L"en";

    // ── General ───────────────────────────────────────────
    bool          showPreviewPane      = true;
    bool          showThumbnails       = true;
    bool          showContextMenu      = true;
    bool          openArchiveOnDblClk  = true;   // true=open, false=extract
    bool          promptForPath        = true;
    bool          rememberLastPath     = true;
    std::wstring  defaultExtractPath;
    std::wstring  defaultFormat        = L"zip";
    CompLevel     defaultCompLevel     = CompLevel::Normal;
    bool          createSolidArchive   = false;
    bool          encryptFileNames     = false;

    // ── Formats (which extensions to handle) ─────────────
    std::unordered_set<std::wstring> enabledFormats;
    // Which extensions ArchiveFldr offers to handle, as a set of
    // lower-case extensions with the dot (".zip"). This drives the
    // Capabilities\FileAssociations key, which is what makes ArchiveFldr
    // selectable per type in Settings > Default apps.
    //
    // It replaces sixteen hard-coded handleXxx booleans that nothing ever
    // read: the format table in Formats.cpp is the real list, and it has
    // 21 registrable entries, not 16.
    // Two sets, because the association genuinely is per bitness: a
    // 64-bit Explorer loads ArchiveFldr.64.dll and a 32-bit host loads
    // ArchiveFldr.32.dll, each registering in its own view of
    // HKLM\Software\Classes. One tick could not describe both.
    std::set<std::wstring> assoc32;
    std::set<std::wstring> assoc64;

    // The set belonging to the build that is asking. Registration code
    // wants its own bitness and nothing else.
    std::set<std::wstring>& AssociatedHere() {
#ifdef _WIN64
        return assoc64;
#else
        return assoc32;
#endif
    }
    const std::set<std::wstring>& AssociatedHere() const {
#ifdef _WIN64
        return assoc64;
#else
        return assoc32;
#endif
    }

    // Listed in HKLM\SOFTWARE\RegisteredApplications, so Windows shows
    // ArchiveFldr in Settings > Default apps. Writing it needs admin, so
    // the settings program reports failure rather than silently not doing
    // it.
    bool          registerAsDefaultApp = false;

    // ── Context Menu Items ────────────────────────────────
    bool          ctxExtract          = true;
    bool          ctxExtractHere      = true;
    bool          ctxAddToArchive     = true;
    // "Add to <name>.<ext>", the one-click compress. Separate from
    // ctxAddToArchive because they are two menu entries and 7-Zip's
    // options list treats them as two.
    bool          ctxCompressHere     = true;
    bool          ctxCompressEmail    = true;
    bool          ctxOpenInShell      = true;
    bool          ctxTestArchive      = true;
    bool          ctxArchiveInfo      = true;
    bool          ctxSettings         = true;
    bool          ctxUseSubMenu       = true;   // "Cascaded context menu"
    bool          ctxMenuIcons        = true;   // "Icons in context menu"
    std::wstring  ctxSubMenuTitle     = L"ArchiveFldr";

    // ── Appearance ────────────────────────────────────────
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
    // Working folder (Folders page). useTempDir stays as the on-disk
    // name of the same switch so existing installs keep their setting.
    bool          useTempDir          = false;  // true = WorkDirMode::Specified
    std::wstring  tempDirPath;
    WorkDirMode   WorkDir() const {
        return useTempDir ? WorkDirMode::Specified : WorkDirMode::SystemTemp;
    }
    bool          logErrors           = true;
    std::wstring  logFilePath;

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
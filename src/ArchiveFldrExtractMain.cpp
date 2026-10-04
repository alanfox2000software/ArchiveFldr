#include "stdafx.h"
#include "ArchiveEngine.h"

static void Usage()
{
    fwprintf(stderr, L"Usage: ArchiveFldrExtract.exe --archive <file> --entry <path> --dest <folder>\n");
}

static bool FindEntry(const std::shared_ptr<IArchiveEngine>& engine,
                      const std::wstring& dir, const std::wstring& wanted,
                      ArchiveEntry& result)
{
    for (const auto& e : engine->List(dir)) {
        if (_wcsicmp(e.fullPath.c_str(), wanted.c_str()) == 0 ||
            _wcsicmp(e.name.c_str(), wanted.c_str()) == 0) {
            result = e;
            return true;
        }
        if (e.isDirectory && FindEntry(engine, e.fullPath, wanted, result))
            return true;
    }
    return false;
}

static const wchar_t* Value(int& i, int argc, wchar_t** argv)
{
    if (++i >= argc) return nullptr;
    return argv[i];
}

int wmain(int argc, wchar_t** argv)
{
    std::wstring archive, entryPath, dest, password; bool passwordStdin = false;
    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--archive")) { auto v = Value(i, argc, argv); if (v) archive = v; }
        else if (!_wcsicmp(argv[i], L"--entry")) { auto v = Value(i, argc, argv); if (v) entryPath = v; }
        else if (!_wcsicmp(argv[i], L"--dest")) { auto v = Value(i, argc, argv); if (v) dest = v; }
        else if (!_wcsicmp(argv[i], L"--password-stdin")) passwordStdin = true;
        else if (!_wcsicmp(argv[i], L"--help")) { Usage(); return 0; }
        else { Usage(); return 2; }
    }
    if (archive.empty() || entryPath.empty() || dest.empty()) { Usage(); return 2; }
    if (passwordStdin) { wchar_t buffer[256] = {}; DWORD n = 0; while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer)-sizeof(wchar_t), &n, nullptr) && n) { buffer[n/sizeof(wchar_t)] = L'\0'; password += buffer; } while (!password.empty() && (password.back()==L'\r' || password.back()==L'\n')) password.pop_back(); }

    auto engine = CreateArchiveEngine(archive);
    if (!engine) {
        fwprintf(stderr, L"Unable to create archive engine.\n");
        return 3;
    }
    if (!password.empty()) engine->SetPassword(password);
    if (!engine->Open(archive)) {
        fwprintf(stderr, L"Unable to open archive: %ls\n", archive.c_str());
        return 3;
    }
    if (!engine->GetCaps().canExtract) {
        fwprintf(stderr, L"Archive format is not extractable.\n");
        return 4;
    }

    ArchiveEntry entry;
    if (!FindEntry(engine, L"", entryPath, entry)) {
        fwprintf(stderr, L"Entry not found: %ls\n", entryPath.c_str());
        return 5;
    }
    SHCreateDirectoryExW(nullptr, dest.c_str(), nullptr);
    const bool ok = engine->ExtractFile(entry, dest,
        [](int pct, const std::wstring& name) {
            fwprintf(stdout, L"PROGRESS %d %ls\n", pct, name.c_str());
            fflush(stdout);
        });
    if (!ok) {
        fwprintf(stderr, L"Extraction failed.\n");
        return 6;
    }
    fwprintf(stdout, L"COMPLETE\n");
    return 0;
}

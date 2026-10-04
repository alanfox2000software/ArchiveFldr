#include "stdafx.h"
#include "ArchiveWriter.h"
#include "ArchiveSecurity.h"

static HANDLE g_cancelEvent = nullptr;
static void CheckCancelled() { if (g_cancelEvent && WaitForSingleObject(g_cancelEvent, 0) == WAIT_OBJECT_0) ExitProcess(ERROR_CANCELLED); }

static void Usage() { fwprintf(stderr, L"Usage: ArchiveFldrCompress.exe --out <file> --format <name> [options] <source>...\n"); }
static std::wstring Next(int& i, int argc, wchar_t** argv) { return ++i < argc ? argv[i] : L""; }
int wmain(int argc, wchar_t** argv)
{
    std::wstring out, format = L"7z", cancelName; std::vector<std::wstring> sources;
    ArchiveWriter::Options opt;
    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--out")) out = Next(i, argc, argv);
        else if (!_wcsicmp(argv[i], L"--cancel-event")) cancelName = Next(i, argc, argv);
        else if (!_wcsicmp(argv[i], L"--format")) { format = Next(i, argc, argv); opt.format = format; }
        else if (!_wcsicmp(argv[i], L"--level")) opt.level = _wtoi(Next(i, argc, argv).c_str());
        else if (!_wcsicmp(argv[i], L"--threads")) opt.threads = _wtoi(Next(i, argc, argv).c_str());
        else if (!_wcsicmp(argv[i], L"--solid")) opt.solid = true;
        else if (!_wcsicmp(argv[i], L"--encrypt-names")) opt.encryptNames = true;
        else if (!_wcsicmp(argv[i], L"--password-stdin")) {
            std::wstring value;
            wchar_t buffer[256] = {};
            DWORD n = 0;
            while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer,
                            sizeof(buffer) - sizeof(wchar_t), &n, nullptr) && n) {
                buffer[n / sizeof(wchar_t)] = L'\0';
                value += buffer;
            }
            SecureZeroMemory(buffer, sizeof(buffer));
            while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n')) value.pop_back();
            opt.password = value;
        }
        else if (!_wcsicmp(argv[i], L"--help")) { Usage(); return 0; }
        else if (argv[i][0] == L'-') { Usage(); return 2; }
        else sources.push_back(argv[i]);
    }
    if (out.empty() || sources.empty()) { Usage(); return 2; }
    if (!cancelName.empty()) g_cancelEvent = OpenEventW(SYNCHRONIZE, FALSE, cancelName.c_str());
    opt.format = format;
    uint64_t totalBytes = 0;
    auto items = ArchiveWriter::CollectItems(sources, &totalBytes);
    if (items.empty()) return 3;
    std::wstring error;
    const bool ok = ArchiveWriter::Compress(out, items, opt,
        [&](int pct, const std::wstring& name) {
            CheckCancelled();
            fwprintf(stdout, L"PROGRESS %d %llu %llu %ls\n", pct, (unsigned long long)((totalBytes * (uint64_t)pct) / 100), (unsigned long long)totalBytes, name.c_str()); fflush(stdout);
        }, &error);
    ArchiveSecurity::SecureClear(opt.password);
    if (!ok) { if (!error.empty()) fwprintf(stderr, L"%ls\n", error.c_str()); return 4; }
    SHChangeNotify(SHCNE_CREATE, SHCNF_PATH, out.c_str(), nullptr);
    std::wstring dir = out;
    if (PathRemoveFileSpecW(&dir[0])) {
        dir.resize(wcslen(dir.c_str()));
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH, dir.c_str(), nullptr);
    }
    fwprintf(stdout, L"COMPLETE\n");
    return 0;
}

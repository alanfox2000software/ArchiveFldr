// stdafx.h — Precompiled Header
// Targets: Windows 10+ (0x0A00), C++20
#pragma once

#define STRICT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT  0x0A00
#define WINVER        0x0A00
#define _WIN32_IE     0x0900
#define _WIN32_DCOM

// ── Windows ──────────────────────────────────────────────
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlobj_core.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <shobjidl_core.h>
#include <objbase.h>
#include <objidl.h>
#include <oleauto.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <thumbcache.h>
#include <docobj.h>
#include <exdisp.h>
#include <msxml6.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <wincodec.h>
#include <dwmapi.h>

// ── GDI+ ────────────────────────────────────────────────
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

// ── Standard Library ─────────────────────────────────────
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <list>
#include <queue>
#include <stack>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <thread>
#include <future>
#include <functional>
#include <algorithm>
#include <numeric>
#include <ranges>
#include <span>
#include <optional>
#include <variant>
#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <format>
#include <chrono>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cwchar>

// ── Pragma Comments ──────────────────────────────────────
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' \
  name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
  processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ── Namespace Aliases ────────────────────────────────────
namespace fs  = std::filesystem;
namespace chr = std::chrono;

// ── Smart COM Pointer Helper ─────────────────────────────
template<typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;
#include <wrl/client.h>

// ── HRESULT Helper ───────────────────────────────────────
#define RETURN_IF_FAILED(hr)  do { HRESULT _hr=(hr); if(FAILED(_hr)) return _hr; } while(0)
#define LOG_IF_FAILED(hr,msg) do { HRESULT _hr=(hr); if(FAILED(_hr)) \
    OutputDebugStringW(std::format(L"FAILED(0x{:08X}): {}\n",_hr,msg).c_str()); } while(0)

// ── Module State (defined in dllmain.cpp) ────────────────
extern HINSTANCE g_hDllInstance;
extern long      g_cDllRefCount;
extern long      g_cLockCount;
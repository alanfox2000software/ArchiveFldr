// stdafx.h — Precompiled Header (FIXED for SDK 10.0.28000.0 / VS 2026)
// Key fixes:
//   1. Define __WRL_NO_DEFAULT_LIB__ before ANY wrl include
//   2. Include <unknwn.h> before wrl to satisfy IUnknown dependency
//   3. ComPtr alias AFTER <wrl/client.h>
//   4. Remove <msxml6.h> (WinRT conflict in new SDK)
//   5. Remove <expected> (C++23 only; not needed here)
//   6. Guard all WinRT-pulling headers with WINRT exclusions
#pragma once

// ── Must define these BEFORE any Windows / WRL headers ───
#define STRICT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VC_EXTRA_LEAN

// ── Suppress WRL pulling in WinRT libs (fixes HSTRING errors) ──
#define __WRL_NO_DEFAULT_LIB__
#define __WRL_CLASSIC_COM__

// ── Target Windows 10+ ───────────────────────────────────
#define _WIN32_WINNT   0x0A00
#define WINVER         0x0A00
#define _WIN32_IE      0x0900
#define NTDDI_VERSION  NTDDI_WIN10_RS5

// ── Prevent WinRT type system from loading (we are classic COM) ──
// These stop activation.h / hstring.h from being auto-included
// by shobjidl.h in the new SDK 10.0.28000.0
#define RO_NO_TEMPLATE_NAME
#ifndef __cplusplus_winrt
#  define _HIDE_GLOBAL_ASYNC_STATUS
#endif

// ═════════════════════════════════════════════════════════
// STEP 1 — Core Windows headers (ORDER MATTERS)
// IUnknown must exist before WRL is included
// ═════════════════════════════════════════════════════════
#include <unknwn.h>          // IUnknown — MUST be first COM header
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>         // CoInitialize, CLSID helpers
#include <objidl.h>          // IStream, IStorage, IDataObject

// ═════════════════════════════════════════════════════════
// STEP 2 — WRL (after unknwn.h, before WinRT headers)
// ═════════════════════════════════════════════════════════
#include <wrl/client.h>      // Microsoft::WRL::ComPtr

// ── ComPtr alias (MUST come AFTER <wrl/client.h>) ────────
template<typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// ═════════════════════════════════════════════════════════
// STEP 3 — Shell / COM headers
// ═════════════════════════════════════════════════════════
#include <shlobj.h>
#include <shlobj_core.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shobjidl_core.h>
#include <oleauto.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <thumbcache.h>      // IThumbnailProvider
#include <docobj.h>

// ═════════════════════════════════════════════════════════
// STEP 4 — UI / GDI headers
// ═════════════════════════════════════════════════════════
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <dwmapi.h>
#include <gdi32.h>           // basic GDI

// GDI+ (must be after windows.h, before using Gdiplus::)
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

// ═════════════════════════════════════════════════════════
// STEP 5 — Standard C++20 Library
// (removed <expected> — requires C++23, not needed here)
// ═════════════════════════════════════════════════════════
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <list>
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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <format>            // C++20 — OK
#include <chrono>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cwchar>

// ═════════════════════════════════════════════════════════
// STEP 6 — Pragma lib links
// ═════════════════════════════════════════════════════════
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
#pragma comment(linker, \
    "\"/manifestdependency:type='win32' "                   \
    "name='Microsoft.Windows.Common-Controls' "             \
    "version='6.0.0.0' processorArchitecture='*' "          \
    "publicKeyToken='6595b64144ccf1df' language='*'\"")

// ═════════════════════════════════════════════════════════
// STEP 7 — Namespace aliases
// ═════════════════════════════════════════════════════════
namespace fs  = std::filesystem;
namespace chr = std::chrono;

// ═════════════════════════════════════════════════════════
// STEP 8 — HRESULT helpers
// ═════════════════════════════════════════════════════════
#define RETURN_IF_FAILED(hr)  \
    do { HRESULT _hr=(hr); if(FAILED(_hr)) return _hr; } while(0)

#define LOG_IF_FAILED(hr, msg)  \
    do { HRESULT _hr=(hr); if(FAILED(_hr))  \
        OutputDebugStringW(                 \
            std::format(L"FAILED(0x{:08X}): {}\n", \
                (unsigned)_hr, msg).c_str()); } while(0)

// ═════════════════════════════════════════════════════════
// STEP 9 — Module state (defined in dllmain.cpp)
// ═════════════════════════════════════════════════════════
extern HINSTANCE g_hDllInstance;
extern long      g_cDllRefCount;
extern long      g_cLockCount;
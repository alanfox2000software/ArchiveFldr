// stdafx.h — Precompiled Header
// Fixed: C4005 macro redefinitions + removed non-existent gdi32.h
#pragma once

// ═════════════════════════════════════════════════════════
// STEP 1 — WRL / WinRT conflict guards
// Use #ifndef so these are safe whether set by .vcxproj
// preprocessor definitions OR defined here directly.
// Both places are acceptable — guards prevent C4005.
// ═════════════════════════════════════════════════════════
#ifndef __WRL_NO_DEFAULT_LIB__
#  define __WRL_NO_DEFAULT_LIB__
#endif

#ifndef __WRL_CLASSIC_COM__
#  define __WRL_CLASSIC_COM__
#endif

#ifndef RO_NO_TEMPLATE_NAME
#  define RO_NO_TEMPLATE_NAME
#endif

#ifndef _HIDE_GLOBAL_ASYNC_STATUS
#  define _HIDE_GLOBAL_ASYNC_STATUS
#endif

// ═════════════════════════════════════════════════════════
// STEP 2 — Windows targeting macros
// All guarded with #ifndef — safe if already set by .vcxproj
// ═════════════════════════════════════════════════════════
#ifndef STRICT
#  define STRICT
#endif

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#  define NOMINMAX
#endif

#ifndef VC_EXTRA_LEAN
#  define VC_EXTRA_LEAN
#endif

// SHELLNSE_XP is set by the project when building with the XP toolset
// (see BUILD-XP.md). It pins the headers to XP so the STL picks the
// XP-compatible synchronisation primitives — with _WIN32_WINNT at 0x0A00
// std::mutex compiles down to SRW locks, which XP's kernel32 does not
// export, and the DLL would not load at all.
#ifdef SHELLNSE_XP
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0501
#  endif
#  ifndef WINVER
#    define WINVER       0x0501
#  endif
#  ifndef SHELLNSE_NO_VISTA_HANDLERS
#    define SHELLNSE_NO_VISTA_HANDLERS   // no thumbnail/preview pane on XP
#  endif
#else
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0A00
#  endif
#  ifndef WINVER
#    define WINVER       0x0A00
#  endif
#endif

#ifndef _WIN32_IE
#  define _WIN32_IE      0x0900
#endif

#ifndef NTDDI_VERSION
#  ifdef SHELLNSE_XP
#    define NTDDI_VERSION 0x05010300   // XP SP3
#  else
#    define NTDDI_VERSION 0x0A000006
#  endif
#endif

// ═════════════════════════════════════════════════════════
// STEP 3 — Core Windows headers (ORDER MATTERS)
// unknwn.h must come before any WRL / COM header
// windows.h must come before shell / GDI headers
// gdi32.h does NOT exist — GDI is in windows.h already
// ═════════════════════════════════════════════════════════
#include <unknwn.h>          // IUnknown — must be first COM header
#include <windows.h>         // also brings in GDI (gdi32)
#include <windowsx.h>
#include <objbase.h>
#include <objidl.h>          // IStream, IStorage, IDataObject

// ═════════════════════════════════════════════════════════
// STEP 4 — WRL (after unknwn.h, before WinRT-pulling headers)
// ═════════════════════════════════════════════════════════
#include <wrl/client.h>      // Microsoft::WRL::ComPtr

// ComPtr alias — MUST come AFTER <wrl/client.h>
template<typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// ═════════════════════════════════════════════════════════
// STEP 5 — Shell / COM headers
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
#include <thumbcache.h>
#include <docobj.h>

// ═════════════════════════════════════════════════════════
// STEP 6 — UI / GDI headers
// NOTE: Do NOT include gdi32.h — it does not exist.
//       All GDI declarations come from <windows.h> above.
// ═════════════════════════════════════════════════════════
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <vssym32.h>
// dwmapi.h is fine to include, but dwmapi.lib must NOT be linked: there
// is no dwmapi.dll on XP, and one unresolvable import stops the whole
// extension loading. Anything from DWM has to be late bound via SysInfo.
#include <dwmapi.h>

// GDI+ — must come after windows.h
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

// ═════════════════════════════════════════════════════════
// STEP 7 — Standard C++20 Library
// <expected> removed — it requires C++23, not C++20
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
#include <format>
#include <chrono>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cwchar>

// ═════════════════════════════════════════════════════════
// STEP 8 — Pragma lib links
// gdi32.lib is kept here (needed by linker even though
// gdi32.h does not exist as standalone — declarations
// come from windows.h)
// ═════════════════════════════════════════════════════════
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")       // lib exists, header does not
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(linker,                                          \
    "\"/manifestdependency:type='win32' "                        \
    "name='Microsoft.Windows.Common-Controls' "                  \
    "version='6.0.0.0' processorArchitecture='*' "               \
    "publicKeyToken='6595b64144ccf1df' language='*'\"")

// ═════════════════════════════════════════════════════════
// STEP 9 — Namespace aliases
// ═════════════════════════════════════════════════════════
namespace fs  = std::filesystem;
namespace chr = std::chrono;

// ═════════════════════════════════════════════════════════
// STEP 10 — HRESULT helpers
// ═════════════════════════════════════════════════════════
#define RETURN_IF_FAILED(hr)  \
    do { HRESULT _hr = (hr); if (FAILED(_hr)) return _hr; } while(0)

#define LOG_IF_FAILED(hr, msg)                                   \
    do { HRESULT _hr = (hr); if (FAILED(_hr))                    \
        OutputDebugStringW(                                       \
            std::format(L"FAILED(0x{:08X}): {}\n",               \
                (unsigned)_hr, msg).c_str()); } while(0)

// ═════════════════════════════════════════════════════════
// STEP 11 — Module state (defined in dllmain.cpp)
// ═════════════════════════════════════════════════════════
extern HINSTANCE g_hDllInstance;
extern long      g_cDllRefCount;
extern long      g_cLockCount;

// Lazy, on-demand subsystem start-up. NEVER call these from DllMain: both
// GdiplusStartup and InitCommonControlsEx run code that needs the loader
// lock DllMain already holds. Call them from the method that actually
// draws or creates a window.
bool EnsureGdiPlus();          // true when GDI+ is usable
void EnsureCommonControls();
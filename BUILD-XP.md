# Building ArchiveFldr for Windows XP through Windows 11

One source tree, two build flavours. The default produces a modern binary;
an opt-in switch produces one that loads on Windows XP.

```
msbuild ArchiveFldr.sln /p:Configuration=Release /p:Platform=x64       modern (default)
msbuild ArchiveFldr.sln /p:Configuration=Release /p:Platform=Win32 ^
                     /p:ArchiveFldrToolset=v141_xp                      XP-capable
```

## Why a switch is needed at all

There are two separate reasons a DLL refuses to load on XP, and they have
to be solved in different places.

**1. The import table.** Windows resolves every statically imported
function when it loads a module. One function that XP's `kernel32` or
`shell32` has never exported and the whole extension fails to load —
before any version check inside our code could run. This is solved in the
source, for both flavours, and it is the part that is done:

| What the code used to import | Why it broke XP | What it does now |
|---|---|---|
| `RegDeleteTreeW` | Vista+ | `SysInfo::DeleteRegTree` — late bound, with a hand-rolled recursive delete behind it |
| `SHCreateStreamOnFileEx` | XP SP2+ | `SysInfo::OpenFileStreamRead` — falls back to `SHCreateStreamOnFileW` |
| `SHGetNameFromIDList` | Vista+ | `SysInfo::GetNameFromIDList` — falls back to `SHGetPathFromIDList` / the desktop folder's `GetDisplayNameOf` |
| `dwmapi.lib` | no `dwmapi.dll` before Vista | not linked at all |

The rule for new code: anything newer than XP goes through
`SysInfo::Bind<>()` and degrades when it returns null. Never add a
`#pragma comment(lib, …)` or an `AdditionalDependencies` entry for a
library that XP does not ship.

You can check the result without a Windows machine by dumping the import
table (`dumpbin /imports ArchiveFldr.64.dll`) and looking for anything
Vista-only outside the CRT rows.

**2. The C runtime.** This is the part the switch exists for. The VS2026
toolset (`v145`) links against the UCRT, which imports SRW locks,
condition variables, `Fls*` and `InitOnceExecuteOnce` — none of which XP
has. No amount of care in our own code changes that, because the runtime
pulls them in on its own behalf. `/p:ArchiveFldrToolset=v141_xp` switches to
the last toolset Microsoft shipped with XP-compatible CRT support.

## What the XP flavour changes

Setting `ArchiveFldrToolset` to anything ending in `_xp` makes the project:

* select that toolset and let it bring its own Windows 7.1A SDK
  (`WindowsTargetPlatformVersion` is left unset in that case);
* define `ARCHIVEFLDR_XP`, `_WIN32_WINNT=0x0501`, `WINVER=0x0501`,
  `NTDDI_VERSION=0x05010300`.

The targeting macros matter as much as the toolset. With `_WIN32_WINNT`
left at `0x0A00`, the STL compiles `std::mutex` and `std::call_once` into
SRW locks and `InitOnceExecuteOnce` even under the XP toolset, and you get
a binary that still will not load. Pinned to `0x0501` they fall back to
critical sections.

`ARCHIVEFLDR_XP` also defines `ARCHIVEFLDR_NO_VISTA_HANDLERS`, which compiles
out `CThumbnailProvider` and `CPreviewHandler`. `IThumbnailProvider` and
`IPreviewHandler` are Vista-era interfaces; XP uses `IExtractImage` and
has no preview pane, so on XP those two classes are dead weight that would
not compile against the 7.1A SDK anyway. Registration skips their registry
keys at runtime (`SysInfo::IsVistaOrLater()`), so even the modern binary
leaves a clean registry if it is ever run somewhere old.

Both link configurations stamp a subsystem floor of 5.01 (Win32) and 5.02
(x64) through `MinimumRequiredVersion`. Without it the linker writes 6.00
into the PE header and XP rejects the file on sight, imports or no
imports.

## Prerequisites for the XP flavour

`v141_xp` is not part of VS2026. Install, from the Visual Studio Installer:

* **MSVC v141 — VS 2017 C++ x64/x86 build tools**, and
* **C++ Windows XP Support for VS 2017 (tools and libraries)**

Both appear under *Individual components*. The solution still opens and
builds normally in VS2026 — the switch only takes effect when you pass it.

## What runs where

| Feature | XP | Vista | 7 | 8.1 | 10 / 11 |
|---|---|---|---|---|---|
| Browse archives as folders | ✓ | ✓ | ✓ | ✓ | ✓ |
| Context menus, drag and drop | ✓ | ✓ | ✓ | ✓ | ✓ |
| Extract / test | ✓ | ✓ | ✓ | ✓ | ✓ |
| Thumbnail provider | — | ✓ | ✓ | ✓ | ✓ |
| Preview pane | — | ✓ | ✓ | ✓ | ✓ |

wimlib is the one engine with a hard version floor of its own: 1.13.0 or
newer (the `libwim-15` builds), checked at load time.

Third-party engine DLLs have their own floor. Several current builds of
zstd, brotli and wimlib are themselves compiled for Vista+; where a
project ships a separate XP build, drop it in as `*.xp.<bits>.dll` and the
loader prefers it automatically on XP (see `thirdparty/README.md`).

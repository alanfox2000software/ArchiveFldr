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
not compile against the 7.1A SDK anyway. Registration follows the same
switch: `kHasVistaHandlers` (`Registry.cpp`) is the compile-time inverse of
`ARCHIVEFLDR_NO_VISTA_HANDLERS`, and an `if constexpr` on it decides whether
`RegisterAll` writes the thumbnail and preview keys or removes any left
behind by an earlier install. A build without the handlers can therefore
never advertise them — the keys are not written and then skipped at run
time, they are not written at all.

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

## What gets built

Outputs land in `<Configuration>\x32` (Win32) or `<Configuration>\x64` (x64):

| File | What it is | Built by |
|---|---|---|
| `ArchiveFldr.64.dll` | the shell extension, for 64-bit hosts | x64 |
| `ArchiveFldr.32.dll` | the shell extension, for 32-bit hosts | Win32 |
| `ArchiveFldrSetting.exe` | the settings window, started by "ArchiveFldr settings..." | x64 / Win32 |
| `Lang\*.txt` | the language files, copied from `Lang\` | both |
| `thirdparty\**\*.dll` | the codec engines, copied from `thirdparty\` | both |

The settings program is named the same in both folders — `x32` and `x64`
each hold one `ArchiveFldrSetting.exe`, matching the bitness of the DLL
beside it. (Earlier builds tagged it `ArchiveFldrSetting.64.exe` /
`.32.exe`, from when both landed in one folder. Nothing looks for those
names any more except the upgrade paths that clean them up.) The DLLs
keep their tags, because both are registered on a 64-bit machine and the
registry has to name each one.

A Win32 build writes only `Release\x32` (or `Debug\x32`). An x64 build
writes only `Release\x64` (or `Debug\x64`) — it does not also build the
32-bit DLL. On 64-bit Windows both DLLs are still needed at runtime
(Explorer loads `ArchiveFldr.64.dll`; 32-bit hosts load
`ArchiveFldr.32.dll`); build each platform separately.

Only the DLLs are registered. The settings program is an ordinary
executable that the DLL starts on demand, which keeps the whole Options
window out of every process that loads a context menu. Its **Install
32-bit** and **Install 64-bit** buttons register whichever of the two
DLLs they find next to themselves, each with the matching `regsvr32`,
so keep them together.

Those two buttons install the *base* extension: browsing archives as
folders, thumbnails, preview, and the Default apps entry. They
deliberately leave the right-click menu alone, because that is a
separate switch per bitness — the two **Integrate to shell context
menu** ticks on the ArchiveFldr page, which are applied by registering
that DLL again. The same choice is available to an unattended install
through `regsvr32 /i`:

```
regsvr32 /s /i             ArchiveFldr.64.dll   rem everything
regsvr32 /s /i:base        ArchiveFldr.64.dll   rem no context menu
regsvr32 /s /i:contextmenu ArchiveFldr.64.dll   rem with context menu
```

Plain `regsvr32 ArchiveFldr.64.dll` with no `/i` follows whatever those
ticks already say, and on a machine that has never run the settings
program that means everything, exactly as it always did.

Everything the settings program writes is machine-wide —
`HKLM\SOFTWARE\ArchiveFldr` for preferences, `HKLM\SOFTWARE\Classes`
for registration — so it is manifested `requireAdministrator` and asks
for elevation once, at launch.

The build registers the DLL it just produced as a convenience. That
needs an elevated Visual Studio; without one the build still succeeds
and says `NOT registered` instead.

## What runs where

| Feature | XP | Vista | 7 | 8.1 | 10 / 11 |
|---|---|---|---|---|---|
| Browse archives as folders | ✓ | ✓ | ✓ | ✓ | ✓ |
| Context menus, drag and drop | ✓ | ✓ | ✓ | ✓ | ✓ |
| Extract / test | ✓ | ✓ | ✓ | ✓ | ✓ |
| Settings program | ✓ | ✓ | ✓ | ✓ | ✓ |
| Translated UI from `Lang\` | ✓ | ✓ | ✓ | ✓ | ✓ |
| Thumbnail provider | — | ✓ | ✓ | ✓ | ✓ |
| Preview pane | — | ✓ | ✓ | ✓ | ✓ |
| Listed in Default apps | — | ✓ | ✓ | ✓ | ✓ |

`RegisteredApplications` and `Capabilities\FileAssociations` are a
Vista-and-later mechanism. The keys are written on XP too; nothing there
reads them, so on XP the extension is reachable only through the file
types it was able to claim without displacing an existing owner.

wimlib is the one engine with a hard version floor of its own: 1.13.0 or
newer (the `libwim-15` builds), checked at load time.

Third-party engine DLLs have their own floor. Several current builds of
zstd, brotli and wimlib are themselves compiled for Vista+; where a
project ships a separate XP build, drop it in as `*.xp.<bits>.dll` and the
loader prefers it automatically on XP (see `thirdparty/README.md`).

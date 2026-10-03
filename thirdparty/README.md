# thirdparty\ — external engine DLLs

ArchiveFldr ships **no** archive codec of its own. Every format it can really
read is powered by a third-party DLL that you drop into this tree, loaded at
runtime and driven through that DLL's own public ABI. That keeps ArchiveFldr's
source free of other projects' licensed decoder code, and it means a new
format is an added DLL, not a rebuild.

All components use the **same layout**, so the rules below never change as
more engines are added.

## Layout

```
<ArchiveFldr dir>\
├─ ArchiveFldr.64.dll
├─ ArchiveFldr.32.dll
└─ thirdparty\
   ├─ 7z\
   │  ├─ 7z.64.dll                 ← used by ArchiveFldr.64.dll
   │  ├─ 7z.32.dll                 ← used by ArchiveFldr.32.dll
   │  └─ Codecs\                   ← optional 7-Zip codec plug-ins
   │     └─ another-codec-x64.dll  ← example external method decoder
   ├─ brotli\                      three DLLs, in a bitness subfolder
   │  ├─ 64\
   │  │  ├─ libbrotlidec.dll
   │  │  ├─ libbrotlienc.dll
   │  │  └─ libbrotlicommon.dll
   │  └─ 32\  (same three)
   ├─ lizard\
   │  ├─ liblizard.64.dll
   │  └─ liblizard.32.dll
   ├─ lz4\
   │  ├─ liblz4.64.dll
   │  └─ liblz4.32.dll
   ├─ lz5\
   │  ├─ liblz5.64.dll
   │  └─ liblz5.32.dll
   ├─ zstd\
   │  ├─ libzstd.64.dll
   │  ├─ libzstd.32.dll
   │  ├─ libzstd.xp.64.dll         ← preferred when running on Windows XP
   │  └─ libzstd.xp.32.dll
   ├─ WimLib\
   │  ├─ libwim-15.64.dll
   │  └─ libwim-15.32.dll
   └─ Unrar\
      ├─ unrar64.dll               ← used by ArchiveFldr.64.dll
      └─ unrar.dll                 ← used by ArchiveFldr.32.dll
```

**Bitness must match the host process**, not your CPU: 64-bit Explorer loads
`ArchiveFldr.64.dll`, which can only load a 64-bit engine. Keep both if 32-bit
applications also browse archives.

Both naming styles work everywhere. `thirdparty\zstd\64\libzstd.dll` and
`thirdparty\zstd\libzstd.64.dll` are equally valid — use whichever your
download already uses, and do not rename anything.

## Search order

For a component `id` with candidate file name `name.dll`, ArchiveFldr tries
every folder below, and within each folder every name variant, taking the
first file that exists. `<bits>` is `64` or `32`, following the running
build.

Folders, in order:

| # | Folder |
|---|--------|
| 1 | `<ArchiveFldr dir>\thirdparty\<id>\<bits>\` |
| 2 | `<ArchiveFldr dir>\thirdparty\<id>\` |
| 3 | `<ArchiveFldr dir>\thirdparty\<bits>\` |
| 4 | `<ArchiveFldr dir>\thirdparty\` |
| 5 | `<ArchiveFldr dir>\<id>\<bits>\` |
| 6 | `<ArchiveFldr dir>\<id>\` |
| 7 | `<ArchiveFldr dir>\` (next to ArchiveFldr) |

Name variants, in order:

| # | Variant | Example (`<bits>` = 64) |
|---|---------|-------------------------|
| 1 | `name.<bits>.dll` | `libzstd.64.dll` |
| 2 | `name<bits>.dll` | `unrar64.dll` |
| 3 | `name.dll` | `libzstd.dll` |
| 4 | `name.xp.<bits>.dll` | `libzstd.xp.64.dll` |

**On Windows XP variant 4 is tried first instead of last.** Some projects
publish a separate XP-compatible build alongside the current one; name it
`*.xp.<bits>.dll` and the same folder serves every Windows version, with
each picking the build that runs there. On Vista and later the XP build is
still reachable as a last resort, so a folder containing only the XP build
works everywhere.

After all of that, the component's registry install hint is consulted
(`HKLM` then `HKCU`, 64- and 32-bit views), which is how an existing 7-Zip
or WinRAR installation is found without copying anything.

The DLL is loaded with `LOAD_WITH_ALTERED_SEARCH_PATH`, and any companion
DLLs a component declares are loaded from the same folder first — that is
how brotli's `libbrotlicommon.dll` is found before `libbrotlidec.dll`
needs it.

Whenever an engine is missing, the error message lists exactly these paths —
drop the DLL at any one of them and retry.

## Components today

| id | folder | primary file(s) | extensions | status |
|----|--------|-----------------|------------|--------|
| `7z` | `thirdparty\7z\` | `7z.dll`, `7za.dll` | `.7z` `.zip` `.tar` `.wim` `.iso` `.cab` `.gz` `.xz` `.bz2` … | extract + test |
| `Unrar` | `thirdparty\Unrar\` | `unrar64.dll`, `unrar.dll` | `.rar` `.r00` `.cbr` | extract + test |
| `zstd` | `thirdparty\zstd\` | `libzstd.<bits>.dll` | `.zst` `.zstd` `.tzst` | extract + test |
| `brotli` | `thirdparty\brotli\<bits>\` | `libbrotlidec.dll` (+ common, enc) | `.br` | extract + test |
| `lz4` | `thirdparty\lz4\` | `liblz4.<bits>.dll` | `.lz4` `.tlz4` | extract + test |
| `lz5` | `thirdparty\lz5\` | `liblz5.<bits>.dll` | `.lz5` | extract + test |
| `lizard` | `thirdparty\lizard\` | `liblizard.<bits>.dll` | `.liz` | extract + test |
| `WimLib` | `thirdparty\WimLib\` | `libwim-15.<bits>.dll` | `.wim` `.swm` `.esd` | extract + verify |

Registry hints: `7z` → `HKLM\SOFTWARE\7-Zip\Path`, `Unrar` →
`HKLM\SOFTWARE\WinRAR\exe64`.

None of these engines is required. A format whose DLL is absent reports
which file it wanted and every path it looked in, and the rest of ArchiveFldr
keeps working.

### Notes per component

**Brotli, LZ4, LZ5, Lizard, Zstandard** are single-stream codecs, not
archive formats: the file holds one compressed stream and no file names.
ArchiveFldr shows exactly one entry, named by removing the suffix —
`notes.txt.zst` → `notes.txt`, `backup.tzst` → `backup.tar`. Only zstd
records the original size in its header, so the other four show their size
only after extraction. Writing is deliberately not offered.

**LZ5 and Lizard** must export the *frame* API (`LZ5F_…` / `LizardF_…`).
A build that exports only the raw block functions cannot read framed
files, and ArchiveFldr says so rather than producing garbage.

**UnRAR** needs version 4 or newer, for the Unicode entry points.
`unrar.dll` decodes only — creating or modifying RAR archives requires a
WinRAR licence. Encrypted archives are reported rather than prompted for,
because a modal password dialog on Explorer's UI thread would hang the
window.

**7-Zip** is the broadest engine by far, and the one to install first.
ArchiveFldr asks `7z.dll` which formats it supports (`GetNumberOfFormats` /
`GetHandlerProperty2`) instead of carrying a hard-coded list, so it reads
exactly what your copy of 7-Zip reads — including formats added after
this was written. When a file's extension does not match its contents,
every other handler is tried as well. ArchiveFldr publishes the raw library
under `thirdparty\zstd\` as a 7-Zip ZSTD decoder for method `04F71101` (and
`04015D`), so the same `libzstd.<bits>.dll` supports both `.zst` files and
ZSTD-compressed 7z archives. It also loads additional 7-Zip codec plug-ins
from the `Codecs` folder adjacent to the selected engine.

**WimLib** needs version **1.13.0 or newer** — in other words the
`libwim-15` builds. `struct wimlib_dir_entry` grew fields in 1.9.1 and
again later, and wimlib hands that struct straight to the caller, so an
older DLL would lay out memory differently from what ArchiveFldr expects.
The version is checked when the DLL loads and anything older is refused
with an explanation rather than read incorrectly.

A WIM holds one or more *images*, each a full directory tree. A
single-image file shows its root directly; a multi-image file shows one
folder per image (`1 - Windows Setup`, `2 - …`), the way DISM and 7-Zip
present them. Extracting such a file writes each image into its own
subfolder so they cannot collide.

For a split WIM, keep every `.swm` part in one folder. Opening part 1 is
enough to browse, but the file data lives across all the parts, so
ArchiveFldr finds the siblings and references them before extracting.

If libwim is absent, `.wim` falls back to `7z.dll`, which reads WIM too.

## Adding another engine

1. Add one row to `kComponents[]` in `src/ThirdParty.cpp`:

   ```cpp
   { L"unrar", L"UnRAR engine", L"unrar.dll", L"" /* companions */,
     L"SOFTWARE\\WinRAR", L"exe64", L"unrar.dll" },
   ```

   That alone gives the new DLL the whole search order, the bitness and XP
   naming, companion preloading and the "searched here" diagnostics.

2. Add the extensions to `kFormats[]` in `src/Formats.cpp`, with the right
   `EngineKind` and component id. That one table drives the file
   associations, the registry registration *and* its removal, so there is
   no second list to keep in step.

3. Implement `IArchiveEngine` for the format (`src/CodecEngine.*` is the
   compact example, `src/UnrarEngine.*` the full one) and resolve the DLL
   with `ThirdParty::LoadComponent(L"unrar")`.

4. Report honest capabilities from `GetCaps()` — `canExtract`, `canAdd`,
   `canDelete`, `canRename`, `canTest`, plus `unavailableReason` when the DLL
   is missing. The shell UI is driven by those flags: commands the engine
   cannot perform are hidden or explained instead of silently failing.

5. Dispatch to it in `CreateArchiveEngine()` (`src/ArchiveEngine.cpp`).

6. Drop the DLL in `thirdparty\<id>\`; the post-build step copies the whole
   `thirdparty\` tree next to the built ArchiveFldr DLLs.

Nothing in this folder is redistributed by this repository — each engine
comes from its own project under its own licence.

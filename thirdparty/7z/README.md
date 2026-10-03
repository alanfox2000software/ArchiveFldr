# thirdparty\7z — external 7-Zip engine (required for real .7z support)

ArchiveFldr does **not** ship any 7-Zip decoder code. Instead, at runtime it
loads a 7-Zip engine DLL from this folder and talks to it through the
stable, public 7-Zip "COM-lite" interface — the same contract 7-Zip's own
`Client7z` SDK sample uses. This keeps ArchiveFldr's own codebase free of
GPL/LGPL-licensed decoder code while still giving you full, real `.7z`
browsing and extraction.

## What you need to do

Drop the official 7-Zip engine DLL(s) into this folder, named by bitness:

```
thirdparty\7z\7z.64.dll     ← used by ArchiveFldr.64.dll (64-bit Explorer)
thirdparty\7z\7z.32.dll     ← used by ArchiveFldr.32.dll (32-bit Explorer / apps)
```

You only need the one matching the ArchiveFldr build(s) you actually use —
most people only need `7z.64.dll` on modern 64-bit Windows.

A plain, unrenamed `thirdparty\7z\7z.dll` is also accepted as a fallback
if you don't want to rename the file.

### Full runtime search order

`7z` is one component of the shared third-party layout documented in
[`..\README.md`](../README.md): ArchiveFldr looks in
`thirdparty\7z\`, then a flat `thirdparty\`, then `<ArchiveFldr dir>\7z\`,
then next to `ArchiveFldr.<bits>.dll` itself — trying `7z.64.dll`, `7z64.dll`
and plain `7z.dll` in each — and finally an installed 7-Zip registered under
`HKLM`/`HKCU\SOFTWARE\7-Zip` → `Path` (both registry views).

The bitness must match the ArchiveFldr build that loads it: a 64-bit
`ArchiveFldr.64.dll` inside 64-bit Explorer can only load a 64-bit `7z.dll`.

If **Open with ArchiveFldr** reports that no engine was found, the message box
lists exactly the paths that were probed — drop the DLL at any one of them.

### Where to get `7z.dll`

`7z.dll` ships inside the official 7-Zip installer/archive from
https://www.7-zip.org/download.html — after installing 7-Zip (or
extracting the portable `7z####-extra.7z` package), copy the matching
`7z.dll` (the 64-bit install's `7z.dll` is the 64-bit engine, the one in
the `x86`/`32-bit` folder of the extra package is the 32-bit engine) into
this folder and rename it as shown above.

> This repository's build/automation cannot download or redistribute
> `7z.dll` itself (it's a separate project with its own license/terms) —
> you must place it here yourself before building, or before copying a
> prebuilt ArchiveFldr to another machine.

## Build-time behavior

The project's post-build step copies every `*.dll` found in this folder
into the build output's `thirdparty\7z\` subfolder (next to
`ArchiveFldr.32.dll` / `ArchiveFldr.64.dll`), so ArchiveFldr can locate its engine
next to itself at runtime regardless of where it's ultimately installed.

If no usable engine DLL is found (here, or via a system-wide 7-Zip install
registered under `HKLM\SOFTWARE\7-Zip`), `.7z` archives will simply fail to
open in ArchiveFldr — there is no fake/placeholder data shown.

## Current limitations (v1 of the 7z engine integration)

- **Extraction only** — creating or modifying `.7z` archives isn't
  supported (7-Zip's own LZMA encoder has a separate, more complex SDK
  surface not yet wired up here). ArchiveFldr reports this through
  `IArchiveEngine::GetCaps()`, so the shell hides or explains the commands
  that would need to write: dropping files onto an open `.7z` says why it
  cannot be done instead of silently doing nothing, and Delete/Rename are
  never offered for items inside it.
- **No password-prompt UI** — unencrypted archives are unaffected, but an
  archive with encrypted headers will fail to open, and individual
  encrypted items inside an otherwise-open archive will fail to extract.

What *does* work with this engine: browsing, the Details columns (size,
packed, ratio, method, date, CRC), opening an item (extracted to a private
read-only temp copy), Extract/Extract-here for the whole archive or a
selection, Test, and copying or dragging items **out** of the archive into
Explorer.

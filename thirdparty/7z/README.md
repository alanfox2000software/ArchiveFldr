# thirdparty\7z — external 7-Zip engine (required for real .7z support)

ShellNSE does **not** ship any 7-Zip decoder code. Instead, at runtime it
loads a 7-Zip engine DLL from this folder and talks to it through the
stable, public 7-Zip "COM-lite" interface — the same contract 7-Zip's own
`Client7z` SDK sample uses. This keeps ShellNSE's own codebase free of
GPL/LGPL-licensed decoder code while still giving you full, real `.7z`
browsing and extraction.

## What you need to do

Drop the official 7-Zip engine DLL(s) into this folder, named by bitness:

```
thirdparty\7z\7z.64.dll     ← used by ShellNSE.64.dll (64-bit Explorer)
thirdparty\7z\7z.32.dll     ← used by ShellNSE.32.dll (32-bit Explorer / apps)
```

You only need the one matching the ShellNSE build(s) you actually use —
most people only need `7z.64.dll` on modern 64-bit Windows.

A plain, unrenamed `thirdparty\7z\7z.dll` is also accepted as a fallback
if you don't want to rename the file.

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
> prebuilt ShellNSE to another machine.

## Build-time behavior

The project's post-build step copies every `*.dll` found in this folder
into the build output's `thirdparty\7z\` subfolder (next to
`ShellNSE.32.dll` / `ShellNSE.64.dll`), so ShellNSE can locate its engine
next to itself at runtime regardless of where it's ultimately installed.

If no usable engine DLL is found (here, or via a system-wide 7-Zip install
registered under `HKLM\SOFTWARE\7-Zip`), `.7z` archives will simply fail to
open in ShellNSE — there is no fake/placeholder data shown.

## Current limitations (v1 of the 7z engine integration)

- **Extraction only** — creating or modifying `.7z` archives isn't
  supported (7-Zip's own LZMA encoder has a separate, more complex SDK
  surface not yet wired up here).
- **No password-prompt UI** — unencrypted archives are unaffected, but an
  archive with encrypted headers will fail to open, and individual
  encrypted items inside an otherwise-open archive will fail to extract.

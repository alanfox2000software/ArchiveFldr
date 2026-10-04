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

### Archives using external methods (ZSTD, Brotli, LZ4, …)

A 7z archive can refer to a compression method that is not built into the
selected `7z.dll`. For example, 7-Zip ZS uses method `04F71101` for ZSTD.
ArchiveFldr adapts the same raw ZSTD runtime already used for `.zst` files and
publishes it to the 7z handler through `ICompressCodecsInfo`:

```
thirdparty\zstd\
├─ libzstd.64.dll
└─ libzstd.32.dll
```

No separate 7-Zip ZSTD plug-in is required when the matching raw library is
present. The adapter creates and extracts standalone Zstandard streams and
encodes/decodes both the 7-Zip ZS method (`04F71101`) and official ZSTD coder
ID (`04015D`), including concatenated/skippable frames. ZIP creation uses
standardized ZSTD method 93.

The same adapter publishes Brotli (`04F71102`), LZ4 (`04F71104`), LZ5
(`04F71105`) and Lizard (`04F71106`) encoders and decoders from these raw
runtime libraries:

```
thirdparty\brotli\32\libbrotlicommon.dll
thirdparty\brotli\32\libbrotlidec.dll
thirdparty\brotli\32\libbrotlienc.dll
thirdparty\brotli\64\libbrotlicommon.dll
thirdparty\brotli\64\libbrotlidec.dll
thirdparty\brotli\64\libbrotlienc.dll
thirdparty\lz4\liblz4.32.dll
thirdparty\lz4\liblz4.64.dll
thirdparty\lz5\liblz5.32.dll
thirdparty\lz5\liblz5.64.dll
thirdparty\lizard\liblizard.32.dll
thirdparty\lizard\liblizard.64.dll
```

Only the matching process bitness is loaded. LZ4 and LZ5 are loaded from their
dedicated `thirdparty\lz4` and `thirdparty\lz5` folders, respectively.
Lizard creation and extraction support all four families: fastLZ4
(levels 10–19), LIZv1 (20–29), fastLZ4 + Huffman (30–39),
and LIZv1 + Huffman (40–49). The same choices are available for standalone
`.liz` streams and Lizard-compressed 7z archives. Official liblizard builds
that export only the raw block API are supported: ArchiveFldr supplies the
standard Lizard frame reader/writer when `LizardF_…` exports are absent.

ArchiveFldr additionally discovers genuine 7-Zip codec plug-ins from a
`Codecs` folder beside the selected engine. This remains useful for other
external methods:

```
thirdparty\7z\
├─ 7z.64.dll
└─ Codecs\
   └─ another-codec-x64.dll
```

All DLLs must match Explorer's bitness. When ArchiveFldr falls back to an
installed `C:\Program Files\7-Zip\7z.dll`, it automatically scans that
installation's adjacent `Codecs` folder.

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

## Capabilities and limitations

ArchiveFldr supports browsing, password prompts (including encrypted file
names), Details columns, opening an item, Extract/Extract-here, Test, copying
or dragging items out, and creating formats for which the selected `7z.dll` publishes a
writer. An archive opened as an Explorer folder is intentionally read-only:
Paste, Ctrl+V, and dragging files into it are not accepted.

The Add to Archive dialog exposes Dictionary size and Word size (`d` and
`fb` writer properties), plus a sized Solid Block setting (`s`) for solid 7z
archives. It can also split a completed archive at an exact byte count. Parts
are named `archive.ext.001`, `.002`, and so on; opening `.001` uses the
extension before that suffix to select the underlying handler and supplies
sibling volumes through 7-Zip's volume callback.

Per-item Delete and Rename are not currently offered. Capabilities are read
from the loaded handler, so commands that its particular `7z.dll` cannot
perform are hidden or explained instead of silently doing nothing.

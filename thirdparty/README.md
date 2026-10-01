# thirdparty\ — external engine DLLs

ShellNSE ships **no** archive codec of its own. Every format it can really
read is powered by a third-party DLL that you drop into this tree, loaded at
runtime and driven through that DLL's own public ABI. That keeps ShellNSE's
source free of other projects' licensed decoder code, and it means a new
format is an added DLL, not a rebuild.

All components use the **same layout**, so the rules below never change as
more engines are added.

## Layout

```
<ShellNSE dir>\
├─ ShellNSE.64.dll
├─ ShellNSE.32.dll
└─ thirdparty\
   ├─ 7z\
   │  ├─ 7z.64.dll        ← used by ShellNSE.64.dll
   │  └─ 7z.32.dll        ← used by ShellNSE.32.dll
   └─ <next engine>\
      ├─ <name>.64.dll
      └─ <name>.32.dll
```

**Bitness must match the host process**, not your CPU: 64-bit Explorer loads
`ShellNSE.64.dll`, which can only load a 64-bit engine. Keep both if 32-bit
applications also browse archives.

## Search order

For a component `id` with candidate file name `name.dll`, ShellNSE takes the
first of these that exists (`64`/`32` follows the running build):

| # | Path |
|---|------|
| 1 | `<ShellNSE dir>\thirdparty\<id>\name.64.dll` |
| 2 | `<ShellNSE dir>\thirdparty\<id>\name64.dll` |
| 3 | `<ShellNSE dir>\thirdparty\<id>\name.dll` |
| 4 | `<ShellNSE dir>\thirdparty\name.*` (flat) |
| 5 | `<ShellNSE dir>\<id>\name.*` |
| 6 | `<ShellNSE dir>\name.*` (next to ShellNSE) |
| 7 | the component's registry install hint, `HKLM` then `HKCU`, 64- and 32-bit views |

The DLL is loaded with `LOAD_WITH_ALTERED_SEARCH_PATH`, so an engine that
needs its own satellite DLLs can keep them in the same folder.

Whenever an engine is missing, the error message lists exactly these paths —
drop the DLL at any one of them and retry.

## Components today

| id | folder | file(s) | registry hint | status |
|----|--------|---------|---------------|--------|
| `7z` | `thirdparty\7z\` | `7z.dll`, `7za.dll` | `SOFTWARE\7-Zip` → `Path` | extract + test (no writing) |

See `thirdparty\7z\README.md` for where to get `7z.dll`.

## Adding another engine

1. Add one row to `kComponents[]` in `src/ThirdParty.cpp`:

   ```cpp
   { L"unrar", L"UnRAR engine", L"unrar.dll",
     L"SOFTWARE\\WinRAR", L"exe64", L"unrar.dll" },
   ```

   That alone gives the new DLL the whole search order, the bitness naming
   and the "searched here" diagnostics.

2. Implement `IArchiveEngine` for the format (see `src/SevenZipEngine.*` as
   the worked example) and resolve the DLL with
   `ThirdParty::Resolve(L"unrar")` / `ThirdParty::Load(...)`.

3. Report honest capabilities from `GetCaps()` — `canExtract`, `canAdd`,
   `canDelete`, `canRename`, `canTest`, plus `unavailableReason` when the DLL
   is missing. The shell UI is driven by those flags: commands the engine
   cannot perform are hidden or explained instead of silently failing.

4. Dispatch to it in `CreateArchiveEngine()` (`src/ArchiveEngine.cpp`) and add
   the extensions to `kSupportedExtensions[]` (`src/GUIDs.h`).

5. Drop the DLL in `thirdparty\<id>\`; the post-build step copies the whole
   `thirdparty\` tree next to the built ShellNSE DLLs.

Nothing in this folder is redistributed by this repository — each engine
comes from its own project under its own licence.

// Registry.h — COM + Shell registration helpers
#pragma once
#include "stdafx.h"

class CRegistry
{
public:
    static HRESULT RegisterAll  (const wchar_t* dllPath);
    static HRESULT UnregisterAll();

    // ── Legacy Explorer context menu cleanup ─────────────
    //
    // Earlier builds registered CLSID_ArchiveFldrContextMenu as an
    // Explorer-level context menu handler — on every file type, on "*"
    // and on "Directory". That feature is gone: the only context menu
    // ArchiveFldr still provides is the one on items inside an opened
    // archive, which belongs to the namespace extension and needs no
    // registration of its own. This sweeps out everything an older
    // build may have written for the Explorer menu. RegisterAll and
    // UnregisterAll both run it, so any registration pass cleans up.
    static HRESULT RemoveLegacyExplorerContextMenu();

    // Windows "Default apps" integration. Publishing a Capabilities key
    // under HKLM\SOFTWARE\RegisteredApplications is what puts ArchiveFldr
    // in the Default apps list, so the user can hand it a file type that
    // Windows' own archive handler currently owns.
    //
    // Driven by the "list in Default apps" checkbox in the settings
    // program, which applies it immediately rather than waiting for
    // the next registration.
    static HRESULT RegisterCapabilities  (const wchar_t* dllPath);
    static HRESULT UnregisterCapabilities();

    // Re-offer (or stop offering) each format's ProgID in its
    // extension's "Open with" list, following the ticks on the System
    // page. Independent of the Default apps registration above, so the
    // settings program can apply an association change on its own.
    static void    RefreshOpenWithProgids();

private:
    // Body of UnregisterAll. How far it reaches depends on the other
    // build: the keys under Software\Classes are one set that WOW64
    // shows to both, so they are only removed when nothing of the
    // other bitness is standing on them.
    static HRESULT UnregisterInternal();

    // Everything of ours that lives in one view of the registry: COM
    // servers, Approved entries, the preview handler list, the legacy
    // overlay entries.
    static void    UnregisterOwnServers();

    static HRESULT RegisterCOMServer   (const CLSID&, const wchar_t* name,
                                        const wchar_t* dllPath,
                                        const wchar_t* threadModel = L"Apartment");
    static HRESULT UnregisterCOMServer (const CLSID&);

    static HRESULT RegisterExtension   (const wchar_t* ext, const wchar_t* progId,
                                        const wchar_t* dllPath);
    static HRESULT UnregisterExtension (const wchar_t* ext, const wchar_t* progId);

    // Removes HKCR\Applications\<settings exe> and
    // HKCR\Applications\ArchiveFldrOpen.exe, which older builds wrote
    // when the open verb went through a program of ours instead of
    // Explorer plus the folder-open delegate.
    static void    UnregisterOpenWithApp();

    static HRESULT RegisterApproved    (const CLSID&, const wchar_t* name);
    static HRESULT UnregisterApproved  (const CLSID&);
    static HRESULT UnregisterOverlay   (const CLSID&, const wchar_t* name);

    // Drops our entry from the shell's global PreviewHandlers list.
    static void    UnregisterPreviewHandlerEntry();

    // The handlers that hang off one Software\Classes base: drop,
    // thumbnail, preview. Always deletes the ContextMenuHandlers entry
    // an older build may have written, so a re-register really does
    // take the retired Explorer menu away rather than leaving the old
    // key behind.
    static HRESULT RegisterShellExOnBase  (const std::wstring& base);
    static void    UnregisterShellExOnBase(const std::wstring& base);

    // Removes the Explorer context menu entry older builds put under
    // one Software\Classes base ("*", "Directory", the per-type keys).
    static void    UnregisterContextMenuOnBase(const std::wstring& base);

    // Namespace-extension (browsable folder object) registration:
    // ShellFolder\Attributes + CATID_BrowsableShellExt + DefaultIcon.
    static HRESULT RegisterNamespaceFolder(const wchar_t* dllPath);

    // "File as folder" junction plumbing — the default value of `keyPath`
    // holds the CLSID of the namespace extension that owns the file type.
    static HRESULT TakeOverJunction(const std::wstring& keyPath,
                                    const std::wstring& ourClsid);
    static void    ReleaseJunction (const std::wstring& keyPath,
                                    const std::wstring& ourClsid);

    static HRESULT SetRegStr(HKEY root, const wchar_t* path,
                             const wchar_t* name, const wchar_t* value);
    static HRESULT SetRegDword(HKEY root, const wchar_t* path,
                               const wchar_t* name, DWORD value);
    static HRESULT DelRegKey(HKEY root, const wchar_t* path);

    static std::wstring ClsidToStr(const CLSID& clsid);
};
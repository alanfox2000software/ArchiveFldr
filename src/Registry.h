// Registry.h — COM + Shell registration helpers
#pragma once
#include "stdafx.h"

class CRegistry
{
public:
    static HRESULT RegisterAll  (const wchar_t* dllPath);
    static HRESULT UnregisterAll();

    // ── The two halves, separately ───────────────────────
    //
    // The browsing extension and the right-click menu are installed
    // and removed independently, per bitness: the Settings page has an
    // Install and an Uninstall button for each build of the DLL, and
    // the ArchiveFldr page has a tick for each build's context menu.
    // regsvr32 /n /i:base and /n /i:contextmenu reach the same code.
    //
    // RegisterBase and UnregisterBase preserve the context menu's
    // actual registration state. This is what makes the Settings page's
    // Install/Uninstall buttons change the browsing half and nothing else.
    static HRESULT RegisterBase  (const wchar_t* dllPath);
    static HRESULT UnregisterBase();

    // Just the context menu handler: its COM registration in this
    // build's view of the registry, and the shellex keys that name it.
    // Nothing here touches the namespace extension, the file type
    // junctions or the Default apps entry.
    static HRESULT RegisterContextMenuOnly  (const wchar_t* dllPath);
    static HRESULT UnregisterContextMenuOnly();

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
    // extension's "Open with" list, following the stored per-format
    // choices. Independent of the Default apps registration above.
    static void    RefreshOpenWithProgids();

private:
    // Shared body of RegisterAll and RegisterBase. preserveContextMenu
    // takes the menu decision from the current COM registration instead
    // of from Settings::CtxMenuHere, keeping a base-only operation from
    // changing it.
    static HRESULT RegisterInternal(const wchar_t* dllPath,
                                    bool preserveContextMenu);

    // Shared body of UnregisterAll and UnregisterBase.
    //
    // keepContextMenu keeps this build's context menu COM registration,
    // and the shellex keys that name it, out of the sweep. What else
    // comes out depends on the other build: the keys under
    // Software\Classes are one set that WOW64 shows to both, so they
    // are only removed when nothing of the other bitness is standing
    // on them.
    static HRESULT UnregisterInternal(bool keepContextMenu);

    // Everything of ours that lives in one view of the registry: COM
    // servers, Approved entries, the preview handler list, the legacy
    // overlay entries. keepContextMenu spares the one handler that is
    // allowed to outlive the rest.
    static void    UnregisterOwnServers(bool keepContextMenu);

    static HRESULT RegisterCOMServer   (const CLSID&, const wchar_t* name,
                                        const wchar_t* dllPath,
                                        const wchar_t* threadModel = L"Apartment");
    static HRESULT UnregisterCOMServer (const CLSID&);

    // withContextMenu == false registers the file type without the
    // right-click handler — the "base" install the Settings page's
    // Install buttons perform. See RegisterAll.
    static HRESULT RegisterExtension   (const wchar_t* ext, const wchar_t* progId,
                                        const wchar_t* dllPath,
                                        bool withContextMenu);
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

    // The handlers that hang off one Software\Classes base: context
    // menu, drop, thumbnail, preview. withContextMenu == false deletes
    // the ContextMenuHandlers entry instead of writing it, so switching
    // an install from "with menu" to "base only" really does take the
    // menu away rather than leaving the old key behind.
    static HRESULT RegisterShellExOnBase  (const std::wstring& base,
                                           bool withContextMenu);
    static void    UnregisterShellExOnBase(const std::wstring& base);

    // Context menu only — no drop handler, no thumbnail, no preview.
    // Used for the "*" and "Directory" keys, where ArchiveFldr has a
    // compress command to offer but nothing to say about the file's
    // contents.
    static HRESULT RegisterContextMenuOnBase  (const std::wstring& base);
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
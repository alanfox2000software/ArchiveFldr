// Registry.h — COM + Shell registration helpers
#pragma once
#include "stdafx.h"

class CRegistry
{
public:
    static HRESULT RegisterAll  (const wchar_t* dllPath);
    static HRESULT UnregisterAll();

    // Driven by the "list in Default apps" checkbox in the settings
    // program, which applies it immediately rather than waiting for
    // the next registration.
    static HRESULT RegisterCapabilities  (const wchar_t* dllPath);
    static HRESULT UnregisterCapabilities();

private:
    static HRESULT RegisterCOMServer   (const CLSID&, const wchar_t* name,
                                        const wchar_t* dllPath,
                                        const wchar_t* threadModel = L"Apartment");
    static HRESULT UnregisterCOMServer (const CLSID&);
    // Identity for the open helper (legacy; removed from build). Was named by every
    // open verb. Private: still cleans up Applications key on uninstall. Not a
    // thing callers arrange for themselves.
    static HRESULT RegisterOpenHelper  (const wchar_t* dllPath);

    static HRESULT RegisterExtension   (const wchar_t* ext, const wchar_t* progId,
                                        const wchar_t* dllPath);
    static HRESULT UnregisterExtension (const wchar_t* ext, const wchar_t* progId);

    // Removes HKCR\Applications\<settings exe>, which older builds wrote
    // when the open verb went through that program instead of Explorer.
    static void    UnregisterOpenWithApp();
    // Windows "Default apps" integration. Publishing a Capabilities key
    // under HKLM\SOFTWARE\RegisteredApplications is what puts ArchiveFldr
    // in the Default apps list, so the user can hand it a file type that
    // Windows' own archive handler currently owns.

    static HRESULT RegisterApproved    (const CLSID&, const wchar_t* name);
    static HRESULT UnregisterApproved  (const CLSID&);
    static HRESULT UnregisterOverlay   (const CLSID&, const wchar_t* name);

    // NEW
    static HRESULT RegisterShellExOnBase  (const std::wstring& base);
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
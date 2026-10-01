// Registry.h — COM + Shell registration helpers
#pragma once
#include "stdafx.h"

class CRegistry
{
public:
    static HRESULT RegisterAll  (const wchar_t* dllPath);
    static HRESULT UnregisterAll();

private:
    static HRESULT RegisterCOMServer   (const CLSID&, const wchar_t* name,
                                        const wchar_t* dllPath,
                                        const wchar_t* threadModel = L"Apartment");
    static HRESULT UnregisterCOMServer (const CLSID&);
    static HRESULT RegisterExtension   (const wchar_t* ext, const wchar_t* progId,
                                        const wchar_t* dllPath);
    static HRESULT UnregisterExtension (const wchar_t* ext);
    static HRESULT RegisterApproved    (const CLSID&, const wchar_t* name);
    static HRESULT UnregisterApproved  (const CLSID&);
    static HRESULT RegisterOverlay     (const CLSID&, const wchar_t* name);
    static HRESULT UnregisterOverlay   (const CLSID&, const wchar_t* name);

    // NEW
    static HRESULT RegisterShellExOnBase  (const std::wstring& base);
    static void    UnregisterShellExOnBase(const std::wstring& base);

    static HRESULT SetRegStr(HKEY root, const wchar_t* path,
                             const wchar_t* name, const wchar_t* value);
    static HRESULT DelRegKey(HKEY root, const wchar_t* path);

    static std::wstring ClsidToStr(const CLSID& clsid);
};
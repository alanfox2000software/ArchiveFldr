// GUIDs.h
#pragma once
// Ensure WinRT is not pulled in before classic COM
#ifndef __WRL_NO_DEFAULT_LIB__
#  define __WRL_NO_DEFAULT_LIB__
#endif
#ifndef __WRL_CLASSIC_COM__
#  define __WRL_CLASSIC_COM__
#endif
#include "stdafx.h"

// ── Shell Namespace Extension ────────────────────────────
// CLSID_ShellFolder  {B1A2C3D4-E5F6-7890-ABCD-111111111111}
DEFINE_GUID(CLSID_ShellNSEFolder,
    0xB1A2C3D4, 0xE5F6, 0x7890,
    0xAB, 0xCD, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11);

// ── Context Menu Handler ─────────────────────────────────
// CLSID_ContextMenu  {B1A2C3D4-E5F6-7890-ABCD-222222222222}
DEFINE_GUID(CLSID_ShellNSEContextMenu,
    0xB1A2C3D4, 0xE5F6, 0x7890,
    0xAB, 0xCD, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22);

// ── Icon Overlay Handler ─────────────────────────────────
// CLSID_IconOverlay  {B1A2C3D4-E5F6-7890-ABCD-333333333333}
DEFINE_GUID(CLSID_ShellNSEIconOverlay,
    0xB1A2C3D4, 0xE5F6, 0x7890,
    0xAB, 0xCD, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33);

// ── Drop Target Handler ──────────────────────────────────
// CLSID_DropTarget   {B1A2C3D4-E5F6-7890-ABCD-444444444444}
DEFINE_GUID(CLSID_ShellNSEDropTarget,
    0xB1A2C3D4, 0xE5F6, 0x7890,
    0xAB, 0xCD, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44);

// ── Thumbnail Provider ───────────────────────────────────
// CLSID_Thumbnail    {B1A2C3D4-E5F6-7890-ABCD-555555555555}
DEFINE_GUID(CLSID_ShellNSEThumbnail,
    0xB1A2C3D4, 0xE5F6, 0x7890,
    0xAB, 0xCD, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55);

// ── Preview Handler ──────────────────────────────────────
// CLSID_Preview      {B1A2C3D4-E5F6-7890-ABCD-666666666666}
DEFINE_GUID(CLSID_ShellNSEPreview,
    0xB1A2C3D4, 0xE5F6, 0x7890,
    0xAB, 0xCD, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66);

// ── Property Sheet ───────────────────────────────────────
// CLSID_PropSheet    {B1A2C3D4-E5F6-7890-ABCD-777777777777}
DEFINE_GUID(CLSID_ShellNSEPropSheet,
    0xB1A2C3D4, 0xE5F6, 0x7890,
    0xAB, 0xCD, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77);

// ── Supported Archive Extensions ─────────────────────────
static constexpr const wchar_t* kSupportedExtensions[] = {
    L".zip", L".zipx", L".jar", L".war", L".ear", L".apk", L".ipa",
    L".docx",L".xlsx", L".pptx",L".odt", L".ods", L".odp",
    L".7z",  L".7zip",
    L".rar", L".r00", L".r01", L".r02",
    L".tar", L".tgz", L".tbz2",L".txz", L".tlz",
    L".gz",  L".gzip",
    L".bz2", L".bzip2",
    L".xz",
    L".lz",  L".lzma",
    L".zst", L".zstd",
    L".lzh", L".lha",
    L".arj",
    L".cab",
    L".iso", L".img", L".nrg", L".mdf",
    L".wim", L".swm", L".esd",
    L".msi", L".msm", L".msp",
    L".rpm", L".deb",
    L".cpio",
    L".dmg",
    L".vhd", L".vhdx",
    nullptr
};

static inline bool IsArchiveExtension(const wchar_t* ext) {
    if (!ext) return false;
    for (int i = 0; kSupportedExtensions[i]; i++)
        if (_wcsicmp(ext, kSupportedExtensions[i]) == 0) return true;
    return false;
}

// ── Registry Key Paths ────────────────────────────────────
static constexpr wchar_t kRegKeySettings[]  = L"Software\\ShellNSE\\Settings";
static constexpr wchar_t kRegKeyClasses[]   = L"Software\\Classes";
static constexpr wchar_t kRegKeyCLSID[]     = L"Software\\Classes\\CLSID";
static constexpr wchar_t kRegKeyApproved[]  =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";
static constexpr wchar_t kRegKeyOverlays[]  =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers";
static constexpr wchar_t kShellNSEName[]    = L"ShellNSE - Archive Shell Extension";
static constexpr wchar_t kShellNSEVersion[] = L"1.0.0";
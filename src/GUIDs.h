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
// The overlay handler was removed; this id is kept only so registration
// can delete what older builds left in the registry.
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
// The "Archive" property page was removed; this id is kept only so
// registration can delete what older builds left in the registry.
DEFINE_GUID(CLSID_ShellNSEPropSheet,
    0xB1A2C3D4, 0xE5F6, 0x7890,
    0xAB, 0xCD, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77);

// ── Supported Archive Extensions ─────────────────────────
// The list itself lives in Formats.cpp — one table that also carries the
// display name, the engine and the third-party DLL each format needs.
#include "Formats.h"

static inline bool IsArchiveExtension(const wchar_t* ext) {
    return Formats::IsArchiveExtension(ext);
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
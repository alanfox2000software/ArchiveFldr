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
DEFINE_GUID(CLSID_ArchiveFldrFolder,
    0xB1A2C3D4, 0xE5F6, 0x7890,
    0xAB, 0xCD, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11);

// ── Context Menu Handler (retired) ───────────────────────
// CLSID_ContextMenu  {B1A2C3D4-E5F6-7890-ABCD-222222222222}
// The Explorer-level context menu handler was removed; this id is kept
// only so registration can delete what older builds left in the
// registry. The context menu inside an opened archive does not use it.
DEFINE_GUID(CLSID_ArchiveFldrContextMenu,
    0xB1A2C3D4, 0xE5F6, 0x7890,
    0xAB, 0xCD, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22);

// ── Icon Overlay Handler ─────────────────────────────────
// CLSID_IconOverlay  {B1A2C3D4-E5F6-7890-ABCD-333333333333}
// The overlay handler was removed; this id is kept only so registration
// can delete what older builds left in the registry.
DEFINE_GUID(CLSID_ArchiveFldrIconOverlay,
    0xB1A2C3D4, 0xE5F6, 0x7890,
    0xAB, 0xCD, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33);

// ── Drop Target Handler ──────────────────────────────────
// CLSID_DropTarget   {B1A2C3D4-E5F6-7890-ABCD-444444444444}
DEFINE_GUID(CLSID_ArchiveFldrDropTarget,
    0xB1A2C3D4, 0xE5F6, 0x7890,
    0xAB, 0xCD, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44);

// ── Thumbnail Provider ───────────────────────────────────
// CLSID_Thumbnail    {B1A2C3D4-E5F6-7890-ABCD-555555555555}
DEFINE_GUID(CLSID_ArchiveFldrThumbnail,
    0xB1A2C3D4, 0xE5F6, 0x7890,
    0xAB, 0xCD, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55);

// ── Preview Handler ──────────────────────────────────────
// CLSID_Preview      {B1A2C3D4-E5F6-7890-ABCD-666666666666}
DEFINE_GUID(CLSID_ArchiveFldrPreview,
    0xB1A2C3D4, 0xE5F6, 0x7890,
    0xAB, 0xCD, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66);

// ── Property Sheet ───────────────────────────────────────
// CLSID_PropSheet    {B1A2C3D4-E5F6-7890-ABCD-777777777777}
// The "Archive" property page was removed; this id is kept only so
// registration can delete what older builds left in the registry.
DEFINE_GUID(CLSID_ArchiveFldrPropSheet,
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
// Machine-wide, per the project's design: one configuration for every
// account, which is also where the shell extension's own registration
// lives. Writing it needs administrator rights, so ArchiveFldrSetting.exe
// asks for them in its manifest.
static constexpr wchar_t kRegKeySettings[]  = L"SOFTWARE\\ArchiveFldr";
static constexpr wchar_t kRegKeyClasses[]   = L"Software\\Classes";
static constexpr wchar_t kRegKeyCLSID[]     = L"Software\\Classes\\CLSID";
static constexpr wchar_t kRegKeyApproved[]  =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";
static constexpr wchar_t kRegKeyOverlays[]  =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers";
static constexpr wchar_t kArchiveFldrName[]    = L"ArchiveFldr - Archive Shell Extension";
static constexpr wchar_t kArchiveFldrVersion[] = L"1.0.0";
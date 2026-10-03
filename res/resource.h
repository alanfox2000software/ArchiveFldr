//{{NO_DEPENDENCIES}}
// Microsoft Visual C++ generated include file.
// Used by resource.rc
//
#pragma once

#ifndef IDC_STATIC
#define IDC_STATIC (-1)
#endif

// ── Icons ─────────────────────────────────────────────────
// Lowest-numbered icon in the module: Explorer picks that one as the
// executable's icon, and ",0" in a DefaultIcon value resolves to it too.
#define IDI_ARCHIVEFLDR             1

// ── Dialogs ───────────────────────────────────────────────
// The Options window and its four tabs, laid out after 7-Zip's File
// Manager > Tools > Options. Every id below doubles as a language-file
// key: Lang.cpp retitles a control by looking its id up in Lang\<x>.txt,
// so these numbers are part of the file format and must not be
// renumbered casually.
//
// 102 was IDD_PAGE_ARCHIVEFLDR, the Explorer context menu integration
// page. The feature was removed; the id is retired, never reused.
#define IDD_OPTIONS                 100
#define IDD_PAGE_SYSTEM             101
#define IDD_PAGE_FOLDERS            103
#define IDD_PAGE_SETTINGS           104
#define IDD_PAGE_LANGUAGE           105
#define IDD_PROGRESS                107
#define IDD_PASSWORD                108

// ── String Table ──────────────────────────────────────────
#define IDS_APP_NAME                400
#define IDS_APP_VERSION             401
#define IDS_APP_DESCRIPTION         402
#define IDS_EXTRACT_TO              403
#define IDS_ADD_FILES               404
#define IDS_COMPRESS_AND_EMAIL      405
#define IDS_OPEN_WITH               406
#define IDS_TEST_ARCHIVE            407
#define IDS_ARCHIVE_INFO            408
#define IDS_SETTINGS                409
#define IDS_EXTRACTING              410
#define IDS_COMPRESSING             411
#define IDS_TESTING                 412
#define IDS_DONE                    413
#define IDS_ERROR_OPEN              414
#define IDS_ERROR_EXTRACT           415
#define IDS_ERROR_CREATE            416
#define IDS_CONFIRM_DELETE          417
#define IDS_ENTER_PASSWORD          418

// ── Options window frame ──────────────────────────────────
#define IDC_TAB_PAGES               1000
#define IDC_BTN_OK                  1001
#define IDC_BTN_CANCEL              1002
#define IDC_BTN_APPLY               1003

// ── System page ───────────────────────────────────────────
#define IDC_LBL_ASSOCIATE           1010
#define IDC_LIST_ASSOC              1011
#define IDC_LBL_BITS32              1012
#define IDC_BTN_ASSOC_ALL_32        1013
#define IDC_BTN_ASSOC_NONE_32       1014
#define IDC_LBL_BITS64              1015
#define IDC_BTN_ASSOC_ALL_64        1016
#define IDC_BTN_ASSOC_NONE_64       1017

// ── ArchiveFldr page (removed) ────────────────────────────
// 1020–1026 were the controls of the Explorer context menu integration
// page (IDC_CHK_INTEGRATE, IDC_CHK_CASCADED, IDC_CHK_MENUICONS,
// IDC_LBL_CTXITEMS, IDC_LIST_CTXITEMS, IDC_CHK_INTEGRATE_64,
// IDC_CHK_INTEGRATE_32). The feature was removed; the ids are retired,
// never reused — each is a key in every Lang\<x>.txt on disk.

// ── Folders page ──────────────────────────────────────────
#define IDC_GRP_WORKFOLDER          1030
#define IDC_RAD_TEMP_SYSTEM         1031
#define IDC_RAD_TEMP_SPEC           1032
#define IDC_EDIT_WORKDIR            1033
#define IDC_BTN_WORKDIR             1034

// ── Settings page ─────────────────────────────────────────
// Install and Uninstall are both per bitness, and each acts on the
// whole shell extension: browsing archives as folders, the context
// menu inside an opened archive, thumbnails, preview, Default apps.
//
// 1042 and 1043 were a single "Install" and a single "Uninstall"
// button covering both bitnesses. They are retired rather than reused:
// each number is a key in every Lang\<x>.txt on disk, and giving one to
// another control would put the old caption on the new button in every
// translation that has not been updated.
#define IDC_GRP_INSTALL             1040
#define IDC_LBL_INSTALL_STATE       1041
#define IDC_LBL_INSTALL_NOTE        1044
#define IDC_BTN_INSTALL_32          1045
#define IDC_BTN_INSTALL_64          1046
#define IDC_BTN_UNINSTALL_32        1047
#define IDC_BTN_UNINSTALL_64        1048

// ── Language page ─────────────────────────────────────────
#define IDC_LBL_LANG                1050
#define IDC_LIST_LANG               1051

// ── Progress Dialog ───────────────────────────────────────
#define IDC_PROGRESS_BAR            1700
#define IDC_STATIC_OPERATION        1701
#define IDC_STATIC_FILE             1702
#define IDC_STATIC_PERCENT          1703
#define IDC_BTN_CANCEL_OP           1704

// ── Password Dialog ───────────────────────────────────────
#define IDC_EDIT_PASSWORD           1800
#define IDC_EDIT_CONFIRM_PW         1801
#define IDC_CHK_SHOW_PASSWORD       1802
#define IDC_CHK_ENCRYPT_HEADER      1803
#define IDC_STATIC_STRENGTH         1804
#define IDC_PROGRESS_STRENGTH       1805

// Next default values for new objects
//
#ifdef APSTUDIO_INVOKED
#ifndef APSTUDIO_READONLY_SYMBOLS
#define _APS_NEXT_RESOURCE_VALUE        2000
#define _APS_NEXT_COMMAND_VALUE         32771
#define _APS_NEXT_CONTROL_VALUE         1900
#define _APS_NEXT_SYMED_VALUE           110
#endif
#endif

// PasswordDialog.h
// ─────────────────────────────────────────────────────────────────────────
// The password prompt, built on the IDD_PASSWORD template.
//
// Two situations ask for a password, and they need different dialogs:
//
//   Ask     — an existing archive wants one: its headers are encrypted
//             (nothing can even be listed), or items in it are encrypted
//             and about to be extracted/tested. One box, no confirm —
//             a typo just fails and asks again.
//
//   AskNew  — a password for an archive about to be WRITTEN. Typed twice,
//             because a typo here is silent data loss. (The Add to
//             Archive dialog embeds its own fields instead of calling
//             this, but the mode exists for any other writer UI.)
//
// Both run modal on the caller's window and return false on cancel.
// ─────────────────────────────────────────────────────────────────────────
#pragma once
#include "stdafx.h"

namespace PasswordDialog {

// `what` names the archive the prompt is about (file name is enough);
// `reason` is one short line shown above the box — why it is being asked
// ("This archive's headers are encrypted...", "3 items are encrypted...").
// Empty reason keeps the template's default wording.
bool Ask(HWND parent, const std::wstring& what, const std::wstring& reason,
         std::wstring& passwordOut);

// Password for something about to be written: adds the Confirm box and
// refuses to close until both match.
bool AskNew(HWND parent, const std::wstring& what, std::wstring& passwordOut);

} // namespace PasswordDialog

// Lang.h
// ─────────────────────────────────────────────────────────────────────────
// Localisation, in the shape 7-Zip uses: a Lang folder of plain .txt files
// sitting next to the binary, one per language, each a list of
//
//     <id> = "text"
//
// where <id> is the control or string resource id the text belongs to.
// Nothing is compiled in but English, and even that is only the fallback
// baked into the dialog templates — a language file that omits an id
// simply leaves that control saying what the resource script says.
//
// Ids 0 and 1 are reserved for the language's own name: 0 in the language
// itself ("Deutsch"), 1 in English ("German"). The Language page lists
// both, which is how a user who cannot read the native name still finds
// their language.
// ─────────────────────────────────────────────────────────────────────────
#pragma once
#include "stdafx.h"

namespace Lang {

struct Entry
{
    std::wstring code;      // file stem: "en", "de", "zh-cn"
    std::wstring native;    // id 0
    std::wstring english;   // id 1
};

// <directory of the running module>\Lang
std::wstring Dir();

// Load Dir()\<code>.txt. Returns false when the file is absent or
// unreadable, in which case the table is emptied and every Str() call
// falls through to its fallback — the UI stays in English rather than
// going blank.
bool Load(const std::wstring& code);

// Language code currently loaded ("en" when none).
const std::wstring& Current();

// Text for a resource id, or `fallback` when this language does not
// carry it. `fallback` is what the resource script already says, so an
// incomplete translation degrades one control at a time.
std::wstring Str(UINT id, const wchar_t* fallback);

// Text for a resource id whose translation carries one "%s", with that
// placeholder replaced by `arg`. Done by substring replacement rather
// than a printf, so a translation that drops or mistypes the
// placeholder produces odd wording instead of a crash.
std::wstring Format1(UINT id, const wchar_t* fallback, const std::wstring& arg);

// Retitle a dialog and every child control whose id the table knows.
// Controls absent from the table keep the text the template gave them.
void Apply(HWND hDlg, UINT dlgId);

// Every .txt in Dir(), sorted by English name, with "en" first.
std::vector<Entry> Available();

} // namespace Lang

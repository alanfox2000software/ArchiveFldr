// BrowseTo.h — open a folder view on a shell item
#pragma once
#include "stdafx.h"

// Navigate into `pidlAbs` the way double-clicking a folder does: one
// Explorer view showing the folder's contents, obeying the user's "open
// each folder in the same window" preference.
//
// This exists because the obvious call is the wrong one. See BrowseTo.cpp.
bool ShellBrowseToFolder(HWND hwnd, PCIDLIST_ABSOLUTE pidlAbs);

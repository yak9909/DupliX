// DupliX 2: what the editor's window (dx2_editor_win.cpp) and its link to After Effects (dx2_editor.cpp) share.
// Main thread only (the window, the idle hook and the UI commands all run on it), except where noted.
#pragma once

#include "v2/dx2.h"

namespace dx2 {
namespace ed {

// The instance the editor edits, and its graph as the editor shows it.
struct Session {
    AEGP_EffectRefH eff = nullptr;  // kept while the editor is open; never used after its instance went away
    A_u_longlong instance = 0;
    dx5::Graph graph;               // the editor's copy (written to AE by Commit)
    HWND wnd = nullptr;
    bool closing = false;           // the window is gone: the idle hook disposes the reference after the writes
};
Session* Current();  // nullptr if no editor is open

// The editor's change to `graph` is done: a new revision, queued for the idle hook (one undo step with `macros`).
void Commit(const char* why, const std::vector<dx5::MacroWrite>& macros = {});
// AE's Undo / Redo, from the editor's Ctrl+Z / Ctrl+Shift+Z (run by the idle hook).
void RequestUndo(bool redo);
// The window of `s` was destroyed (WM_DESTROY).
void WindowGone(Session* s);

// ---- the window (dx2_editor_win.cpp) ----
HWND CreateEditorWindow(Session* s);
// The graph changed outside the editor (AE's undo): redraw it as it is now.
void GraphReloaded(HWND wnd);

}  // namespace ed
}  // namespace dx2

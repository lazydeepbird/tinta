#ifndef TINTA_IME_H
#define TINTA_IME_H

// Inline IME composition for the Markdown source editor (Korean, Japanese,
// Chinese).
//
// Design: while the user composes, the composition string is *spliced into*
// App::editorText at [ime.start, ime.start + ime.length). The editor's normal
// DirectWrite line layout, wrapping, caret and scrolling therefore apply to the
// composing text without a second rendering path, and the glyphs after the
// caret reflow instead of being overdrawn. Invariants:
//
//   * A composition is ONE transaction. Interim strings (a Hangul jamo, then the
//     syllable, then its final form) only rewrite the spliced range; nothing
//     touches the Undo stack, the dirty flag or the preview until text commits.
//   * A commit records exactly one edit: an Insert that coalesces like typed
//     characters, or - when the composition overwrote a selection - a single
//     Replace whose Undo brings the selection back.
//   * Cancel removes the spliced text and restores the overwritten selection.
//   * The IME's committed text arrives ONLY through WM_IME_COMPOSITION
//     (GCS_RESULTSTR). We consume that message instead of forwarding it to
//     DefWindowProc, so no WM_IME_CHAR/WM_CHAR duplicate is generated. Input
//     targets that do not draw their own composition (find bar, table cell
//     editor) return "not handled" and keep the default Windows behaviour.
//   * Anything that reads or replaces the buffer (save, tab switch, mouse click,
//     minimize, close, focus loss, Ctrl-chords) first calls imeCommitPending().

#include "app.h"

#include <string>

// Result of asking the IME for a string or the cursor position. Empty is a
// valid zero-length answer (the composition was cleared); Failed is an API
// error and must never be mistaken for it.
enum class ImeQuery { Ok, Empty, Failed };

// Thin seam over the Imm* calls so the state machine is testable
// deterministically. Win32ImeSource is the production implementation.
class ImeSource {
public:
    virtual ~ImeSource() = default;
    // index is GCS_COMPSTR or GCS_RESULTSTR; out holds UTF-16 code units.
    virtual ImeQuery string(DWORD index, std::wstring& out) = 0;
    // GCS_CURSORPOS: a character index (not a byte count).
    virtual ImeQuery cursor(size_t& position) = 0;
    // ImmNotifyIME(NI_COMPOSITIONSTR, action): CPS_COMPLETE or CPS_CANCEL.
    // Returns whether the IME accepted the request.
    virtual bool notify(DWORD action) = 0;
};

class Win32ImeSource : public ImeSource {
public:
    explicit Win32ImeSource(HWND hwnd) : hwnd_(hwnd) {}
    ImeQuery string(DWORD index, std::wstring& out) override;
    ImeQuery cursor(size_t& position) override;
    bool notify(DWORD action) override;
private:
    HWND hwnd_;
};

// True when the source editor is the input target and draws its own
// composition. Find/replace fields, the table cell editor, dialogs and the
// reading preview are not.
bool imeOwnsInput(const App& app);

// --- State machine (return true = consumed, do not call DefWindowProc) ---
bool imeStartComposition(App& app, ImeSource& source);
bool imeComposition(App& app, LPARAM flags, ImeSource& source);
bool imeEndComposition(App& app, ImeSource& source);

// Finish any open composition as committed text. Safe to call when idle and
// from within IME messages' aftermath; it never inserts text twice.
void imeCommitPending(App& app, ImeSource& source);
void imeCommitPending(App& app);  // Win32 source on app.hwnd
// Abandon the open composition: the spliced text goes away and an overwritten
// selection returns. False if there was nothing to cancel.
bool imeCancelComposition(App& app, ImeSource& source);

// Called for a real (non-VK_PROCESSKEY) key press. Escape cancels a live
// composition (true = consumed); any other non-modifier key commits it first.
bool imeKeyDown(App& app, HWND hwnd, WPARAM virtualKey);

// Window-message entry used by WndProc: handles WM_IME_SETCONTEXT,
// WM_IME_STARTCOMPOSITION, WM_IME_COMPOSITION and WM_IME_ENDCOMPOSITION.
// Returns true when it produced `result` and the caller must return it.
bool imeWindowMessage(App& app, HWND hwnd, UINT message, WPARAM wParam,
                      LPARAM lParam, LRESULT& result);
bool imeWindowMessage(App& app, HWND hwnd, UINT message, WPARAM wParam,
                      LPARAM lParam, ImeSource& source, LRESULT& result);

// Anchor the IME candidate window under the start of the composition.
void imePositionWindows(App& app, HWND hwnd);

#endif // TINTA_IME_H

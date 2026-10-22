#include "ime.h"
#include "editor.h"
#include "search.h"

#include <imm.h>
#include <algorithm>

#ifndef ISC_SHOWUIALL
#define ISC_SHOWUIALL 0xC000000FL
#endif

// See ime.h for the design and its invariants.

namespace {

// IME strings are a handful of characters; anything huge is a broken driver
constexpr LONG kMaxImeBytes = 1 << 16;

// Reset the transaction, keeping the state that outlives it
void closeTransaction(App& app) {
    bool hidden = app.ime.uiHidden;
    app.ime = App::ImeComposition{};
    app.ime.uiHidden = hidden;
}

void beginTransaction(App& app) {
    closeTransaction(app);
    // A composition happens at the primary caret only: the extra carets
    // (#251) go, like on a plain click
    app.editorExtraCarets.clear();
    auto& ime = app.ime;
    ime.active = true;
    const size_t size = app.editorText.size();
    ime.cursorBefore = std::min(app.editorCursorPos, size);
    ime.selStartBefore = app.editorSelStart;
    ime.selEndBefore = app.editorSelEnd;
    if (app.editorHasSelection && app.editorSelStart != app.editorSelEnd) {
        size_t lo = std::min(std::min(app.editorSelStart, app.editorSelEnd), size);
        size_t hi = std::min(std::max(app.editorSelStart, app.editorSelEnd), size);
        ime.replacedSelection = true;
        ime.replaced = app.editorText.substr(lo, hi - lo);
        ime.start = lo;
    } else {
        ime.start = ime.cursorBefore;
    }
}

// The selection leaves the buffer the first time real composition text has
// to occupy its place, so Escape before that changes nothing at all
void removeSelectionIfNeeded(App& app) {
    auto& ime = app.ime;
    if (ime.replacedSelection && !ime.selectionRemoved) {
        editorImeSplice(app, ime.start, ime.replaced.size(), L"");
        ime.selectionRemoved = true;
        app.editorHasSelection = false;
    }
}

// Rewrite the composition range with the IME's current string
void setCompositionText(App& app, const std::wstring& text, size_t caret) {
    auto& ime = app.ime;
    if (!text.empty()) removeSelectionIfNeeded(app);
    const bool same = ime.length == text.size() &&
        app.editorText.compare(ime.start, ime.length, text) == 0;
    if (!same) {
        editorImeSplice(app, ime.start, ime.length, text);
        ime.length = text.size();
    }
    ime.caret = std::min(caret, text.size());
    app.editorCursorPos = ime.start + ime.caret;
    app.editorHasSelection = false;
    app.editorCaretUpstreamPos = std::wstring::npos;
}

// The IME's final string replaces the composition and becomes ONE edit. The
// transaction then continues empty at the new position: a single message may
// carry the next syllable's composition right behind the result.
void commitResult(App& app, const std::wstring& result) {
    auto& ime = app.ime;
    removeSelectionIfNeeded(app);
    editorImeSplice(app, ime.start, ime.length, result);
    const std::wstring removed = ime.replacedSelection ? ime.replaced : L"";
    editorImeRecordCommit(app, ime.start, removed, result, ime.cursorBefore,
                          ime.selStartBefore, ime.selEndBefore,
                          ime.replacedSelection);
    const size_t next = ime.start + result.size();
    closeTransaction(app);
    ime.active = true;
    ime.start = ime.cursorBefore = next;
    ime.selStartBefore = ime.selEndBefore = next;
    app.editorCursorPos = next;
    app.editorHasSelection = false;
    app.editorCaretUpstreamPos = std::wstring::npos;
}

// Drop the composition: its text disappears and an overwritten selection is
// back exactly as it was
void cancelTransaction(App& app) {
    auto& ime = app.ime;
    if (ime.length > 0) editorImeSplice(app, ime.start, ime.length, L"");
    if (ime.replacedSelection) {
        if (ime.selectionRemoved) editorImeSplice(app, ime.start, 0, ime.replaced);
        app.editorSelStart = ime.selStartBefore;
        app.editorSelEnd = ime.selEndBefore;
        app.editorHasSelection = true;
        app.editorCursorPos = ime.cursorBefore;
    } else {
        app.editorCursorPos = ime.start;
    }
    closeTransaction(app);
    editorImeRefresh(app);
}

// The IME ended without reporting a result: what it displayed becomes text.
// Losing typed characters is worse than keeping an unconfirmed one.
void keepTransaction(App& app) {
    auto& ime = app.ime;
    if (ime.length == 0) {
        cancelTransaction(app);
        return;
    }
    const std::wstring text = app.editorText.substr(ime.start, ime.length);
    const std::wstring removed = ime.replacedSelection ? ime.replaced : L"";
    editorImeRecordCommit(app, ime.start, removed, text, ime.cursorBefore,
                          ime.selStartBefore, ime.selEndBefore,
                          ime.replacedSelection);
    app.editorCursorPos = ime.start + ime.length;
    app.editorHasSelection = false;
    closeTransaction(app);
    editorImeRefresh(app);
}

bool isModifierOrModeKey(WPARAM vk) {
    switch (vk) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
        case VK_LWIN: case VK_RWIN:
        case VK_HANGUL: case VK_JUNJA: case VK_FINAL: case VK_HANJA:
        case VK_CONVERT: case VK_NONCONVERT: case VK_PROCESSKEY:
            return true;
    }
    return false;
}

// Re-send WM_IME_SETCONTEXT when the input target changed kind since the
// last one, so the default composition UI is hidden for the source editor
// and available again for fields that rely on it. Our own handler decides
// the flags each time.
void syncUiFlags(App& app, HWND hwnd, bool owns) {
    if (!hwnd || owns == app.ime.uiHidden) return;
    SendMessageW(hwnd, WM_IME_SETCONTEXT, TRUE, ISC_SHOWUIALL);
}

}  // namespace

// --- Win32 source ---

ImeQuery Win32ImeSource::string(DWORD index, std::wstring& out) {
    out.clear();
    HIMC himc = ImmGetContext(hwnd_);
    if (!himc) return ImeQuery::Failed;
    ImeQuery outcome = ImeQuery::Failed;
    // The return value is a BYTE count; negative means IMM_ERROR_*
    LONG bytes = ImmGetCompositionStringW(himc, index, nullptr, 0);
    if (bytes == 0) {
        outcome = ImeQuery::Empty;
    } else if (bytes > 0 && bytes <= kMaxImeBytes) {
        std::wstring buffer((size_t)bytes / sizeof(wchar_t), L'\0');
        LONG got = ImmGetCompositionStringW(himc, index, &buffer[0],
                                            (DWORD)(buffer.size() * sizeof(wchar_t)));
        if (got >= 0) {
            buffer.resize(std::min((size_t)got / sizeof(wchar_t), buffer.size()));
            out = std::move(buffer);
            outcome = out.empty() ? ImeQuery::Empty : ImeQuery::Ok;
        }
    }
    ImmReleaseContext(hwnd_, himc);
    return outcome;
}

ImeQuery Win32ImeSource::cursor(size_t& position) {
    HIMC himc = ImmGetContext(hwnd_);
    if (!himc) return ImeQuery::Failed;
    // GCS_CURSORPOS returns a character index directly (0 is a valid answer),
    // NOT a byte length like the string queries
    LONG at = ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
    ImmReleaseContext(hwnd_, himc);
    if (at < 0) return ImeQuery::Failed;
    position = (size_t)at;
    return ImeQuery::Ok;
}

bool Win32ImeSource::notify(DWORD action) {
    HIMC himc = ImmGetContext(hwnd_);
    if (!himc) return false;
    // May synchronously send WM_IME_COMPOSITION/ENDCOMPOSITION to hwnd_
    BOOL ok = ImmNotifyIME(himc, NI_COMPOSITIONSTR, action, 0);
    ImmReleaseContext(hwnd_, himc);
    return ok != FALSE;
}

// --- Ownership ---

bool imeOwnsInput(const App& app) {
    return sourceEditorHasFocus(app) && !app.confirmExitPending &&
           !app.showContextMenu && !app.showSettings && !app.showThemeChooser &&
           !app.showHelp && !app.showShortcutEditor && !app.showThemeEditor &&
           !app.showPrintPreview && !app.annotEditorOpen;
}

// --- State machine ---

bool imeStartComposition(App& app, ImeSource&) {
    if (app.ime.detaching) return true;
    if (app.ime.active) return true;  // START without END: keep the transaction
    if (!imeOwnsInput(app)) return false;
    beginTransaction(app);
    return true;
}

bool imeComposition(App& app, LPARAM flags, ImeSource& source) {
    if (app.ime.detaching) return true;  // the flush's own messages
    if (!app.ime.active) {
        if (!imeOwnsInput(app)) return false;  // default WM_IME_CHAR/WM_CHAR path
        beginTransaction(app);                 // some IMEs skip STARTCOMPOSITION
    }

    std::wstring result, composition;
    bool haveResult = false, haveComposition = false;

    if (flags & GCS_RESULTSTR) {
        // Ok: final text. Empty: the IME committed nothing. Failed: unknown,
        // so leave the buffer alone rather than guess.
        ImeQuery q = source.string(GCS_RESULTSTR, result);
        haveResult = q != ImeQuery::Failed;
    }
    // lParam == 0 is how an IME reports "the composition string was cleared"
    if ((flags & GCS_COMPSTR) || flags == 0) {
        ImeQuery q = source.string(GCS_COMPSTR, composition);
        // A failed query keeps the current composition; only a real empty
        // answer clears it
        haveComposition = q != ImeQuery::Failed;
        if (q == ImeQuery::Empty) composition.clear();
    }
    size_t caret = composition.size();
    if (haveComposition && !composition.empty() && (flags & GCS_CURSORPOS)) {
        size_t at = 0;
        if (source.cursor(at) == ImeQuery::Ok) caret = std::min(at, composition.size());
    }

    // Result first, then any composition that follows it in the same message
    if (haveResult) {
        if (!result.empty()) commitResult(app, result);
        else setCompositionText(app, L"", 0);
    }
    if (haveComposition) {
        setCompositionText(app, composition, caret);
    } else if ((flags & GCS_CURSORPOS) && app.ime.length > 0) {
        size_t at = 0;
        if (source.cursor(at) == ImeQuery::Ok) {
            app.ime.caret = std::min(at, app.ime.length);
            app.editorCursorPos = app.ime.start + app.ime.caret;
        }
    }
    editorImeRefresh(app);
    return true;
}

bool imeEndComposition(App& app, ImeSource&) {
    if (app.ime.detaching) return true;
    if (!app.ime.active) return imeOwnsInput(app);
    // By now a commit or a cancel has normally emptied the range. Residual
    // text with no reported result is kept; an empty, uncommitted transaction
    // hands back the selection it borrowed.
    if (app.ime.length > 0) keepTransaction(app);
    else cancelTransaction(app);
    return true;
}

void imeCommitPending(App& app, ImeSource& source) {
    if (!app.ime.active || app.ime.detaching) return;
    // Ask the IME to finish: it answers with WM_IME_COMPOSITION(GCS_RESULTSTR),
    // re-entering imeComposition on this same thread
    source.notify(CPS_COMPLETE);
    if (!app.ime.active) return;
    if (app.ime.length == 0 && !app.ime.replacedSelection) {
        closeTransaction(app);  // nothing was ever composed
        return;
    }
    // The IME never answered. Adopt what is displayed, then cancel the IME's
    // own copy (its flush messages are ignored) so it cannot re-send the same
    // text later and duplicate it.
    app.ime.detaching = true;
    source.notify(CPS_CANCEL);
    app.ime.detaching = false;
    keepTransaction(app);
}

void imeCommitPending(App& app) {
    if (!app.ime.active) return;
    Win32ImeSource source(app.hwnd);
    imeCommitPending(app, source);
}

bool imeCancelComposition(App& app, ImeSource& source) {
    if (!app.ime.active || app.ime.detaching) return false;
    // The IME answers with an empty composition then ENDCOMPOSITION
    source.notify(CPS_CANCEL);
    if (app.ime.active) {
        app.ime.detaching = true;
        source.notify(CPS_CANCEL);
        app.ime.detaching = false;
        cancelTransaction(app);
    }
    return true;
}

bool imeKeyDown(App& app, HWND hwnd, WPARAM virtualKey) {
    if (!app.ime.active) return false;
    if (virtualKey == VK_ESCAPE) {
        Win32ImeSource source(hwnd);
        return imeCancelComposition(app, source);
    }
    if (isModifierOrModeKey(virtualKey)) return false;
    Win32ImeSource source(hwnd);
    imeCommitPending(app, source);
    return false;  // the key itself still runs, now on the committed buffer
}

// --- Window messages ---

void imePositionWindows(App& app, HWND hwnd) {
    if (!app.ime.active || !hwnd) return;
    D2D1_POINT_2F point{};
    if (!editorPointForPos(app, app.ime.start, point)) return;
    HIMC himc = ImmGetContext(hwnd);
    if (!himc) return;
    // Just below the line the composition starts on, so the candidate list
    // never covers the text being composed
    POINT below = {(LONG)point.x, (LONG)(point.y + editorLineHeightPx(app))};
    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = below;
    ImmSetCompositionWindow(himc, &composition);
    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos = below;
    ImmSetCandidateWindow(himc, &candidate);
    ImmReleaseContext(hwnd, himc);
}

bool imeWindowMessage(App& app, HWND hwnd, UINT message, WPARAM wParam,
                      LPARAM lParam, LRESULT& result) {
    Win32ImeSource source(hwnd);
    return imeWindowMessage(app, hwnd, message, wParam, lParam, source, result);
}

bool imeWindowMessage(App& app, HWND hwnd, UINT message, WPARAM wParam,
                      LPARAM lParam, ImeSource& source, LRESULT& result) {
    switch (message) {
        case WM_IME_SETCONTEXT: {
            // Only while the source editor draws its own composition is the
            // default composition window suppressed; the candidate list and
            // every other IME UI part stay untouched
            const bool owns = wParam && imeOwnsInput(app);
            app.ime.uiHidden = owns;
            if (!owns) return false;
            result = DefWindowProcW(hwnd, message, wParam,
                                    lParam & ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW);
            return true;
        }
        case WM_IME_STARTCOMPOSITION:
            syncUiFlags(app, hwnd, imeOwnsInput(app));
            if (imeStartComposition(app, source)) {
                imePositionWindows(app, hwnd);
                result = 0;
                return true;
            }
            editorPositionImeWindow(app, hwnd);  // find bar / table cell
            return false;
        case WM_IME_COMPOSITION:
            if (imeComposition(app, lParam, source)) {
                imePositionWindows(app, hwnd);
                result = 0;
                return true;
            }
            editorPositionImeWindow(app, hwnd);
            return false;
        case WM_IME_ENDCOMPOSITION:
            if (imeEndComposition(app, source)) {
                result = 0;
                return true;
            }
            return false;
    }
    return false;
}

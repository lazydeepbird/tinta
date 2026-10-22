// ime_composition: the inline IME composition state machine (src/ime.cpp)
// driven through the same functions the window procedure calls, with the Imm*
// queries replaced by a scripted source so every sequence is deterministic.
//
// Scope note: this exercises the production state machine and the real editor
// layout. It is NOT a substitute for typing with the Microsoft Korean IME; see
// tinta-codex-kit/MANUAL_TESTS.ko.md for that.

#include "d2d_init.h"
#include "editor.h"
#include "ime.h"
#include "input.h"
#include "render.h"
#include "search.h"
#include "settings.h"
#include "tabs.h"
#include "utils.h"

#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

namespace {

int failures = 0, assertions = 0;
std::string currentCase;
std::vector<std::pair<std::string, int>> caseAssertions;

void check(bool ok, const char* what) {
    ++assertions;
    ++caseAssertions.back().second;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL [" << currentCase << "]: " << what << '\n';
    }
}
void beginCase(const char* name) {
    currentCase = name;
    caseAssertions.push_back({name, 0});
}

// Scripted stand-in for ImmGetCompositionStringW / ImmNotifyIME
struct FakeIme : ImeSource {
    std::wstring composition, result;
    ImeQuery compositionStatus = ImeQuery::Ok;  // Ok/Empty derive from the text
    ImeQuery resultStatus = ImeQuery::Ok;
    ImeQuery cursorStatus = ImeQuery::Failed;
    size_t cursorAt = 0;
    std::vector<DWORD> notifications;
    std::function<void(DWORD)> onNotify;

    ImeQuery string(DWORD index, std::wstring& out) override {
        out.clear();
        const bool isResult = index == GCS_RESULTSTR;
        const ImeQuery status = isResult ? resultStatus : compositionStatus;
        const std::wstring& text = isResult ? result : composition;
        if (status == ImeQuery::Failed) return ImeQuery::Failed;
        if (text.empty()) return ImeQuery::Empty;
        out = text;
        return ImeQuery::Ok;
    }
    ImeQuery cursor(size_t& position) override {
        if (cursorStatus == ImeQuery::Ok) position = cursorAt;
        return cursorStatus;
    }
    bool notify(DWORD action) override {
        notifications.push_back(action);
        if (onNotify) onNotify(action);
        return true;
    }
    int count(DWORD action) const {
        int n = 0;
        for (DWORD a : notifications) n += a == action;
        return n;
    }
};

// One WM_IME_COMPOSITION as the window procedure would see it
bool send(App& app, FakeIme& ime, LPARAM flags, const wchar_t* composition,
          const wchar_t* result = L"") {
    ime.composition = composition;
    ime.result = result;
    ime.compositionStatus = ime.resultStatus = ImeQuery::Ok;
    return imeComposition(app, flags, ime);
}

std::unique_ptr<App> makeApp(const std::string& file, int width = 1050) {
    auto state = std::make_unique<App>();
    App& app = *state;
    app.hwnd = CreateWindowExW(0, L"STATIC", L"IME composition tests", WS_POPUP,
                               0, 0, width, 900, nullptr, nullptr, nullptr, nullptr);
    if (!app.hwnd || !initD2D(app) || !createRenderTarget(app)) return nullptr;
    app.width = width;
    app.height = 900;
    updateTextFormats(app);
    Settings settings;
    settings.keyProfile = "windows";
    applyKeymap(app, settings);
    app.currentFile = file;
    enterEditMode(app);
    editorReparse(app);
    ensureLayoutComplete(app);
    return state;
}

// A small deterministic buffer for the state-machine cases
void reset(App& app, const std::wstring& text, size_t cursor,
           size_t selStart = 0, size_t selEnd = 0) {
    app.ime = App::ImeComposition{};
    app.editorText = text;
    rebuildLineStarts(app);
    app.editorCursorPos = cursor;
    app.editorHasSelection = selStart != selEnd;
    app.editorSelStart = selStart;
    app.editorSelEnd = selEnd;
    app.editorExtraCarets.clear();
    app.undoStack.clear();
    app.redoStack.clear();
    app.editorDirty = false;
    app.showSearch = app.searchActive = false;
    app.tableEditActive = false;
    app.confirmExitPending = false;
    app.editorReadingPreview = false;
}

// Korean typing as an IME delivers it: START, then COMPSTR updates, then a
// RESULTSTR, then END
const LPARAM COMP = GCS_COMPSTR | GCS_CURSORPOS;
const LPARAM RESULT = GCS_RESULTSTR;

// ---------------------------------------------------------------------------

void extraCarets(App& app) {
    beginCase("a composition drops the extra carets (#251) and commits once");
    FakeIme ime;
    reset(app, L"ab\ncd\nef", 1);
    for (size_t pos : {4u, 7u}) {
        App::EditorCaret caret;
        caret.pos = caret.anchor = pos;
        app.editorExtraCarets.push_back(caret);
    }
    check(imeStartComposition(app, ime), "START consumed with extra carets");
    check(app.editorExtraCarets.empty(), "only the primary caret composes");
    send(app, ime, COMP, L"\u314E");
    send(app, ime, COMP, L"\uD55C");
    send(app, ime, RESULT, L"", L"\uD55C");
    imeEndComposition(app, ime);
    check(app.editorText == L"a\uD55Cb\ncd\nef", "syllable lands at the primary caret only");
    check(app.editorCursorPos == 2, "caret follows the syllable");
    check(app.undoStack.size() == 1 && app.undoStack.back().group == 0,
          "one ungrouped undo entry");
    editorUndoForTest(app);
    check(app.editorText == L"ab\ncd\nef", "undo restores the original text");
}

void hangulSyllable(App& app) {
    beginCase("one syllable (jamo -> syllable -> final) commits once");
    FakeIme ime;
    reset(app, L"AB", 1);
    check(imeOwnsInput(app), "the source editor owns IME input");
    check(imeStartComposition(app, ime), "START is consumed for the source editor");
    check(app.ime.active && app.ime.start == 1, "transaction opens at the caret");

    const size_t undoBefore = app.undoStack.size();
    for (const wchar_t* step : {L"\u314E", L"\uD558", L"\uD55C"}) {  // jamo, syllable, final
        check(send(app, ime, COMP, step), "COMPSTR update is consumed");
        check(app.editorText == std::wstring(L"A") + step + L"B",
              "each interim string REPLACES the previous one (no accumulation)");
        check(app.ime.length == 1 && app.ime.start == 1, "composition range stays one char");
        check(app.editorCursorPos == 2, "caret sits after the composing char");
        check(app.undoStack.size() == undoBefore, "interim strings add no Undo entry");
        check(!app.editorDirty, "an uncommitted composition does not dirty the buffer");
    }
    check(send(app, ime, RESULT, L"", L"\uD55C"), "RESULTSTR is consumed (no WM_IME_CHAR follows)");
    check(app.editorText == L"A\uD55CB", "final buffer holds \uD55C exactly once");
    check(app.editorDirty, "the commit marks the buffer dirty");
    check(app.undoStack.size() == undoBefore + 1, "the commit is ONE undo entry");
    check(app.editorCursorPos == 2, "caret after the committed syllable");
    check(imeEndComposition(app, ime), "END is consumed");
    check(!app.ime.active, "END closes the transaction");
    check(app.editorText == L"A\uD55CB", "END never re-inserts or removes the result");
}

bool isHangul(wchar_t c) { return c >= 0xAC00 && c <= 0xD7A3; }

// Compatibility jamo of a syllable's initial consonant (the first step of typing it)
wchar_t initialJamo(wchar_t syllable) {
    static const wchar_t kInitial[19] = {
        0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141, 0x3142, 0x3143, 0x3145,
        0x3146, 0x3147, 0x3148, 0x3149, 0x314A, 0x314B, 0x314C, 0x314D, 0x314E};
    return kInitial[(syllable - 0xAC00) / 588];
}
// The syllable without its final consonant (the second step of typing it)
wchar_t withoutFinal(wchar_t syllable) {
    return (wchar_t)(0xAC00 + ((syllable - 0xAC00) / 28) * 28);
}

// Type `sentence` the way a two-set Korean IME delivers it: every syllable goes
// jamo -> syllable -> final through COMPSTR updates, the next syllable's first jamo rides in
// the same message as the previous RESULTSTR, and anything that is not Hangul
// (space, Latin, digits) arrives as an ordinary WM_CHAR after the IME committed.
void typeSentence(App& app, const std::wstring& sentence) {
    FakeIme ime;
    reset(app, L"", 0);
    bool composing = false;
    wchar_t pending = 0;  // the syllable currently composing (final form)
    auto interim = [&](wchar_t syllable, bool startsAfterResult) {
        const std::wstring prefix = app.editorText.substr(0, app.ime.start);
        std::wstring steps[3] = {std::wstring(1, initialJamo(syllable)),
                                 std::wstring(1, withoutFinal(syllable)),
                                 std::wstring(1, syllable)};
        const int n = withoutFinal(syllable) == syllable ? 2 : 3;
        for (int i = 0; i < n; i++) {
            const size_t undoBefore = app.undoStack.size();
            const LPARAM flags = (i == 0 && startsAfterResult) ? (RESULT | COMP) : COMP;
            const std::wstring result =
                (i == 0 && startsAfterResult) ? std::wstring(1, pending) : L"";
            send(app, ime, flags, steps[i].c_str(), result.c_str());
            if (i > 0) {
                check(app.editorText.size() == app.ime.start + steps[i].size(),
                      "an interim string replaces the previous one, never appends");
                check(app.undoStack.size() == undoBefore, "interim strings write no Undo entry");
            }
        }
    };
    for (wchar_t ch : sentence) {
        if (isHangul(ch)) {
            if (!composing) {
                imeStartComposition(app, ime);
                composing = true;
                interim(ch, false);
            } else {
                interim(ch, true);
            }
            pending = ch;
        } else {
            if (composing) {
                send(app, ime, RESULT, L"", std::wstring(1, pending).c_str());
                imeEndComposition(app, ime);
                composing = false;
            }
            handleCharInput(app, app.hwnd, ch);
        }
    }
    if (composing) {
        send(app, ime, RESULT, L"", std::wstring(1, pending).c_str());
        imeEndComposition(app, ime);
    }
}

void sentenceSequence(App& app) {
    beginCase("sentences: mixed Hangul / Latin / digits / spaces typed like a two-set IME");
    for (const wchar_t* sentence : {L"\uD55C\uAE00 \uC785\uB825 \uD14C\uC2A4\uD2B8",
                                    L"GS\uB9AC\uD14C\uC77C \uD611\uB825\uC0AC \uD3EC\uD138",
                                    L"ABC\uD55C\uAE00123",
                                    L"\uAC12\uC774 \uC77D\uACE0 \uC559\uC544"}) {
        typeSentence(app, sentence);
        check(app.editorText == sentence, "the buffer equals what was typed: nothing lost, nothing doubled");
        check(!app.ime.active || app.ime.length == 0, "no composition text left in the buffer");
        check(app.editorCursorPos == std::wstring(sentence).size(), "caret ends after the last character");
        check(app.editorDirty, "typing marked the buffer dirty");
        // Undo peels the typed runs; nothing residual or duplicated remains
        for (int i = 0; i < 64 && !app.undoStack.empty(); i++) editorUndoForTest(app);
        check(app.editorText.empty(), "undoing every entry returns the empty buffer");
    }
}

void resultPlusNewComposition(App& app) {
    beginCase("RESULTSTR + next COMPSTR in one message");
    FakeIme ime;
    reset(app, L"x|y", 2);
    imeStartComposition(app, ime);
    send(app, ime, COMP, L"\uD55C");
    // the first syllable is final; the next keystroke already begins the next one
    check(send(app, ime, RESULT | COMP, L"\u3131", L"\uD55C"), "combined message is consumed");
    check(app.editorText == L"x|\uD55C\u3131y", "committed \uD55C stays and \u3131 composes after it");
    check(app.ime.start == 3 && app.ime.length == 1, "new composition starts at the NEW position");
    check(app.editorCursorPos == 4, "caret follows the new composition");
    send(app, ime, COMP, L"\uAE00");  // the second syllable
    send(app, ime, RESULT, L"", L"\uAE00");
    imeEndComposition(app, ime);
    check(app.editorText == L"x|\uD55C\uAE00y", "\uD55C\uAE00 with both syllables exactly once");
    check(app.undoStack.size() == 1, "consecutive syllables coalesce like typed characters");
    editorUndoForTest(app);
    check(app.editorText == L"x|y", "one Undo removes the whole typed run");
}

void emptyVersusFailed(App& app) {
    beginCase("empty composition string vs API query failure");
    FakeIme ime;
    reset(app, L"ab", 1);
    imeStartComposition(app, ime);
    send(app, ime, COMP, L"\uD558");
    check(app.editorText == L"a\uD558b", "composition present");

    // A FAILED query must not be read as "cleared": the text stays put
    ime.composition = L"";
    ime.compositionStatus = ImeQuery::Failed;
    check(imeComposition(app, COMP, ime), "failed query still consumes the message");
    check(app.editorText == L"a\uD558b" && app.ime.length == 1,
          "a failed COMPSTR query leaves the composition untouched");

    // A VALID zero-length answer clears it
    ime.compositionStatus = ImeQuery::Ok;
    check(imeComposition(app, COMP, ime), "empty query is consumed");
    check(app.editorText == L"ab" && app.ime.length == 0,
          "a real empty COMPSTR removes the composition text");

    // lParam == 0 is how some IMEs report a cleared composition
    send(app, ime, COMP, L"\uD55C");
    ime.composition = L"";
    check(imeComposition(app, 0, ime), "lParam 0 is consumed");
    check(app.editorText == L"ab", "lParam 0 clears the composition");

    // Failed RESULTSTR: never guess at text
    send(app, ime, COMP, L"\uD55C");
    ime.resultStatus = ImeQuery::Failed;
    ime.compositionStatus = ImeQuery::Ok;
    ime.composition = L"\uD55C";
    imeComposition(app, RESULT | COMP, ime);
    check(app.editorText == L"a\uD55Cb", "a failed RESULTSTR neither inserts nor deletes");
    imeEndComposition(app, ime);
    check(app.editorText == L"a\uD55Cb", "END keeps the displayed text when no result arrived");
}

void cursorPosition(App& app) {
    beginCase("GCS_CURSORPOS is a character index");
    FakeIme ime;
    reset(app, L"__", 1);
    imeStartComposition(app, ime);
    ime.cursorStatus = ImeQuery::Ok;
    ime.cursorAt = 1;
    send(app, ime, COMP, L"\uD55C\uAE00");
    check(app.editorCursorPos == 2 && app.ime.caret == 1, "cursor 1 = between the two syllables");
    ime.cursorAt = 0;
    send(app, ime, COMP, L"\uD55C\uAE00");
    check(app.editorCursorPos == 1 && app.ime.caret == 0, "cursor 0 is valid, not an error");
    ime.cursorAt = 99;
    send(app, ime, COMP, L"\uD55C\uAE00");
    check(app.ime.caret == 2, "an out-of-range cursor is clamped to the string");
    ime.cursorStatus = ImeQuery::Failed;
    send(app, ime, COMP, L"\uD55C\uAE00");
    check(app.ime.caret == 2, "a failed cursor query falls back to the end");
    imeCancelComposition(app, ime);
}

void selectionReplacement(App& app) {
    beginCase("selection replacement: commit / Undo / Redo / cancel");
    // --- commit ---
    {
        FakeIme ime;
        reset(app, L"pre TARGET post", 8, 4, 10);  // "TARGET" selected, caret at its end
        check(app.editorHasSelection, "selection is set up");
        imeStartComposition(app, ime);
        check(app.editorText == L"pre TARGET post", "START alone leaves the selection in place");
        send(app, ime, COMP, L"\uD558");
        check(app.editorText == L"pre \uD558 post", "first composition char replaces the selection");
        check(!app.editorHasSelection, "selection highlight is gone while composing");
        send(app, ime, COMP, L"\uD55C");
        send(app, ime, RESULT, L"", L"\uD55C");
        imeEndComposition(app, ime);
        check(app.editorText == L"pre \uD55C post", "committed \uD55C replaced the selection");
        check(app.undoStack.size() == 1 &&
              app.undoStack.back().type == App::EditAction::Replace,
              "the replacement is one atomic Replace");
        check(app.editorDirty, "dirty after commit");

        editorUndoForTest(app);
        check(app.editorText == L"pre TARGET post", "Undo restores the original text");
        check(app.editorHasSelection && app.editorSelStart == 4 && app.editorSelEnd == 10,
              "Undo restores the selection too");
        check(app.editorCursorPos == 8, "Undo restores the caret");
        editorRedoForTest(app);
        check(app.editorText == L"pre \uD55C post", "Redo reapplies the replacement");
        check(app.editorCursorPos == 5 && !app.editorHasSelection, "Redo caret after the syllable");
    }
    // --- cancel ---
    {
        FakeIme ime;
        reset(app, L"pre TARGET post", 8, 4, 10);
        imeStartComposition(app, ime);
        send(app, ime, COMP, L"\uD558");
        check(imeCancelComposition(app, ime), "cancel reports something to cancel");
        check(ime.count(CPS_CANCEL) >= 1, "cancel asks the IME to drop its composition");
        // The IME answers a cancel with an empty COMPSTR and ENDCOMPOSITION
        send(app, ime, COMP, L"");
        imeEndComposition(app, ime);
        check(app.editorText == L"pre TARGET post", "cancel restores the original text");
        check(app.editorHasSelection && app.editorSelStart == 4 && app.editorSelEnd == 10,
              "cancel restores the selection");
        check(!app.editorDirty && app.undoStack.empty(),
              "cancel leaves no dirty flag and no Undo entry");
        check(!app.ime.active, "transaction is closed after cancel");
    }
    // --- Escape before any character: nothing changes at all ---
    {
        FakeIme ime;
        reset(app, L"pre TARGET post", 8, 4, 10);
        imeStartComposition(app, ime);
        imeCancelComposition(app, ime);
        imeEndComposition(app, ime);
        check(app.editorText == L"pre TARGET post" && app.editorHasSelection,
              "START then cancel with no text is a no-op");
    }
    // --- Backspace all the way: composition empties, borrowed selection returns ---
    {
        FakeIme ime;
        reset(app, L"pre TARGET post", 8, 4, 10);
        imeStartComposition(app, ime);
        send(app, ime, COMP, L"\u314E");
        send(app, ime, COMP, L"");
        imeEndComposition(app, ime);
        check(app.editorText == L"pre TARGET post" && app.editorSelStart == 4,
              "an emptied uncommitted composition hands the selection back");
    }
}

void repeatedEndAndCancel(App& app) {
    beginCase("repeated END and cancel messages never duplicate or delete");
    FakeIme ime;
    reset(app, L"AB", 1);
    imeStartComposition(app, ime);
    send(app, ime, COMP, L"\uD55C");
    send(app, ime, RESULT, L"", L"\uD55C");
    for (int i = 0; i < 4; i++) imeEndComposition(app, ime);
    check(app.editorText == L"A\uD55CB", "repeated END leaves the committed text alone");
    for (int i = 0; i < 3; i++) imeCancelComposition(app, ime);
    check(app.editorText == L"A\uD55CB", "cancel after commit does not undo the committed text");
    check(app.undoStack.size() == 1, "still exactly one undo entry");
    // END with no transaction at all
    reset(app, L"AB", 1);
    check(imeEndComposition(app, ime), "END without a transaction is consumed silently");
    check(app.editorText == L"AB", "and changes nothing");
}

void commitPendingBoundaries(App& app) {
    beginCase("save / tab switch / minimize commit exactly once");
    // The IME answers CPS_COMPLETE with the final RESULTSTR
    {
        FakeIme ime;
        reset(app, L"AB", 1);
        imeStartComposition(app, ime);
        send(app, ime, COMP, L"\uD55C");
        ime.onNotify = [&](DWORD action) {
            if (action == CPS_COMPLETE) {
                send(app, ime, RESULT, L"", L"\uD55C");
                imeEndComposition(app, ime);
            }
        };
        imeCommitPending(app, ime);
        check(app.editorText == L"A\uD55CB", "IME-answered COMPLETE commits \uD55C once");
        check(!app.ime.active, "transaction closed");
        check(ime.count(CPS_CANCEL) == 0, "no needless cancel when the IME answered");
        check(app.undoStack.size() == 1 && app.editorDirty, "one undo entry, dirty");
    }
    // The IME never answers: adopt what is on screen, flush the IME's copy
    {
        FakeIme ime;
        reset(app, L"AB", 1);
        imeStartComposition(app, ime);
        send(app, ime, COMP, L"\uD55C");
        // While flushing, the IME emits its own empty COMPSTR/END: they must
        // not delete the text we are keeping
        ime.onNotify = [&](DWORD action) {
            if (action == CPS_CANCEL) {
                send(app, ime, COMP, L"");
                imeEndComposition(app, ime);
            }
        };
        imeCommitPending(app, ime);
        check(app.editorText == L"A\uD55CB", "unanswered COMPLETE still keeps \uD55C once");
        check(ime.count(CPS_COMPLETE) == 1 && ime.count(CPS_CANCEL) == 1,
              "IME is asked to complete, then its own copy is cancelled");
        check(!app.ime.active && !app.ime.detaching, "no transaction and no flush flag left");
        check(app.undoStack.size() == 1, "committed as one edit");
        // A late duplicate result from a confused IME is a fresh transaction,
        // which is why the flush above cancels the IME's copy
        imeCommitPending(app, ime);
        check(app.editorText == L"A\uD55CB", "commit with nothing pending is a no-op");
    }
    // Commit over a selection through the fallback path
    {
        FakeIme ime;
        reset(app, L"pre TARGET post", 8, 4, 10);
        imeStartComposition(app, ime);
        send(app, ime, COMP, L"\uD55C");
        imeCommitPending(app, ime);
        check(app.editorText == L"pre \uD55C post", "fallback commit replaced the selection");
        editorUndoForTest(app);
        check(app.editorText == L"pre TARGET post" && app.editorHasSelection,
              "and Undo brings the selection back");
    }
    // Idle: nothing to do
    {
        FakeIme ime;
        reset(app, L"AB", 1);
        imeCommitPending(app, ime);
        check(ime.notifications.empty(), "idle commit never touches the IME");
    }
}

void keyPolicy(App& app) {
    beginCase("Escape / ordinary keys / modifier keys");
    auto begin = [&](const wchar_t* comp) {
        FakeIme ime;
        reset(app, L"AB", 1);
        imeStartComposition(app, ime);
        send(app, ime, COMP, comp);
    };
    // Escape cancels the composition first; the editor stays in edit mode
    begin(L"\uD558");
    check(imeKeyDown(app, app.hwnd, VK_ESCAPE), "Escape is consumed while composing");
    check(app.editorText == L"AB" && !app.ime.active, "Escape removed only the composition");
    check(app.editMode && !app.editorReadingPreview,
          "that one Escape did not leave edit mode or toggle the reading preview");
    check(!imeKeyDown(app, app.hwnd, VK_ESCAPE), "Escape with no composition is left to the editor");
    // Ctrl+Shift alone must not disturb it (Shift+key types double consonants etc.)
    begin(L"\uD558");
    for (WPARAM vk : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_HANGUL, VK_CAPITAL}) {
        check(!imeKeyDown(app, app.hwnd, vk), "modifier / mode key passes through");
        check(app.ime.active && app.ime.length == 1, "modifier key keeps the composition alive");
    }
    // A real command key commits first
    check(!imeKeyDown(app, app.hwnd, 'S'), "Ctrl+S's key is not consumed");
    check(app.editorText == L"A\uD558B" && !app.ime.active,
          "the command key found the composition already committed");
    check(app.undoStack.size() == 1, "one edit for the run");
}

void otherInputTargets(App& app) {
    beginCase("find bar and table cell input never touch the source document");
    FakeIme ime;
    // Find bar focused
    reset(app, L"source text", 6);
    app.showSearch = true;
    app.searchActive = true;
    check(!imeOwnsInput(app), "the find field does not use the source composition");
    check(!imeStartComposition(app, ime), "START falls through to the default IME UI");
    check(!send(app, ime, RESULT, L"", L"\uD55C"),
          "RESULTSTR is NOT consumed: DefWindowProc's WM_IME_CHAR->WM_CHAR path stays in charge");
    check(!imeEndComposition(app, ime), "END falls through too");
    check(app.editorText == L"source text" && !app.editorDirty && !app.ime.active,
          "the source document is untouched");
    // ...and the WM_CHAR that path produces lands in the find field, not the source
    app.searchQuery.clear();
    handleCharInput(app, app.hwnd, L'\uD55C');
    check(app.editorText == L"source text", "WM_CHAR for the find field never edits the source");
    app.showSearch = app.searchActive = false;

    // Table cell editor focused
    reset(app, L"source text", 6);
    app.tableEditActive = true;
    check(!imeOwnsInput(app) && !imeStartComposition(app, ime),
          "the table cell editor keeps its own IME handling");
    check(!send(app, ime, COMP | RESULT, L"\uD55C", L"\uD55C"), "composition not consumed for a table cell");
    check(app.editorText == L"source text", "source untouched by a table cell composition");
    app.tableEditActive = false;

    // Dialogs, reading preview
    reset(app, L"source text", 6);
    app.confirmExitPending = true;
    check(!imeOwnsInput(app), "the unsaved-changes dialog is not a composition target");
    app.confirmExitPending = false;
    app.editorReadingPreview = true;
    check(!imeOwnsInput(app), "the reading preview is not a composition target");
    app.editorReadingPreview = false;
    check(imeOwnsInput(app), "ownership returns with the source editor");
}

void windowMessageRouting(App& app) {
    beginCase("WM_IME_* message routing");
    FakeIme ime;
    LRESULT result = 99;
    reset(app, L"AB", 1);
    check(imeWindowMessage(app, nullptr, WM_IME_STARTCOMPOSITION, 0, 0, ime, result) && result == 0,
          "STARTCOMPOSITION handled with result 0");
    ime.composition = L"\uD55C";
    check(imeWindowMessage(app, nullptr, WM_IME_COMPOSITION, 0, COMP, ime, result) && result == 0,
          "COMPOSITION handled with result 0 (not forwarded to DefWindowProc)");
    check(app.editorText == L"A\uD55CB", "routed message updated the buffer");
    ime.composition.clear();
    ime.result = L"\uD55C";
    imeWindowMessage(app, nullptr, WM_IME_COMPOSITION, 0, RESULT, ime, result);
    check(imeWindowMessage(app, nullptr, WM_IME_ENDCOMPOSITION, 0, 0, ime, result),
          "ENDCOMPOSITION handled");
    check(app.editorText == L"A\uD55CB", "routed commit inserted once");

    // SETCONTEXT hides only the composition window, and only for the source editor
    result = 0;
    const LPARAM all = ISC_SHOWUIALL;
    bool handled = imeWindowMessage(app, app.hwnd, WM_IME_SETCONTEXT, TRUE, all, ime, result);
    check(handled && app.ime.uiHidden, "source editor active: default composition UI suppressed");
    app.showSearch = app.searchActive = true;
    handled = imeWindowMessage(app, app.hwnd, WM_IME_SETCONTEXT, TRUE, all, ime, result);
    check(!handled && !app.ime.uiHidden,
          "find bar active: SETCONTEXT untouched, default UI available again");
    app.showSearch = app.searchActive = false;
    handled = imeWindowMessage(app, app.hwnd, WM_IME_SETCONTEXT, FALSE, all, ime, result);
    check(!handled, "deactivation is never intercepted");
    // The candidate list bit is never cleared by us
    check((ISC_SHOWUICOMPOSITIONWINDOW & ~(LPARAM)ISC_SHOWUIALL) == 0, "sanity: flag is part of ALL");
}

void surrogateSafety(App& app) {
    beginCase("emoji / surrogate pairs survive");
    FakeIme ime;
    const std::wstring smile = L"\U0001F600";
    reset(app, L"a" + smile + L"b", 3);  // caret between the emoji and b
    imeStartComposition(app, ime);
    send(app, ime, COMP, L"\uD55C");
    check(app.editorText == L"a" + smile + L"\uD55Cb", "composition after an emoji");
    send(app, ime, RESULT, L"", L"\uD55C");
    imeEndComposition(app, ime);
    check(app.editorText == L"a" + smile + L"\uD55Cb", "emoji pair intact after commit");

    // Selecting the whole emoji and replacing it
    reset(app, L"a" + smile + L"b", 3, 1, 3);
    imeStartComposition(app, ime);
    send(app, ime, COMP, L"\uD558");
    check(app.editorText == L"a\uD558b", "the selected emoji (2 UTF-16 units) is replaced whole");
    imeCancelComposition(app, ime);
    send(app, ime, COMP, L"");
    imeEndComposition(app, ime);
    check(app.editorText == L"a" + smile + L"b", "cancel puts the complete pair back");
    editorUndoForTest(app);  // nothing to undo: buffer unchanged
    check(app.editorText == L"a" + smile + L"b", "no stray undo entry from a cancelled composition");
}

// ---- Layout: composition lives in the real DirectWrite line layout ----

float wrapWidth(App& app) {
    return editorPaneWidth(app) - (dpi(app, 48) + editorGutterWidth(app)) - dpi(app, 16);
}

struct Expected { float x = 0, y = 0; bool ok = false; };

// Independent hit test of `column` within the logical line containing `pos`
Expected independentPoint(App& app, size_t pos) {
    Expected e;
    size_t lineStart = pos;
    while (lineStart > 0 && app.editorText[lineStart - 1] != L'\n') --lineStart;
    size_t lineEnd = app.editorText.find(L'\n', lineStart);
    if (lineEnd == std::wstring::npos) lineEnd = app.editorText.size();
    size_t line = 0;
    for (size_t i = 0; i < app.editorLineStarts.size(); ++i)
        if (app.editorLineStarts[i] == lineStart) line = i;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    const bool wrap = app.editorWordWrap;
    app.dwriteFactory->CreateTextLayout(app.editorText.data() + lineStart,
        (UINT32)(lineEnd - lineStart), app.editorTextFormat,
        wrap ? wrapWidth(app) : 1e7f, 1e7f, layout.GetAddressOf());
    if (!layout) return e;
    if (wrap) layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    if (app.fontFallback) {
        Microsoft::WRL::ComPtr<IDWriteTextLayout2> layout2;
        if (SUCCEEDED(layout.As(&layout2))) layout2->SetFontFallback(app.fontFallback);
    }
    FLOAT hx = 0, hy = 0;
    DWRITE_HIT_TEST_METRICS m{};
    layout->HitTestTextPosition((UINT32)(pos - lineStart), FALSE, &hx, &hy, &m);
    const float height = app.editorTextFormat->GetFontSize() * 1.5f;
    const float row = wrap ? (float)app.editorRowStarts[line] : (float)line;
    e.x = dpi(app, 48) + editorGutterWidth(app) + hx - (wrap ? 0.0f : app.editorScrollX);
    e.y = chromeTopHeight(app) + dpi(app, 8) + row * height - app.editorScrollY + (wrap ? hy : 0.0f);
    e.ok = true;
    return e;
}

void layoutAndDisplay(App& app) {
    beginCase("layout: caret and composition anchor match DirectWrite under wrap/scroll/zoom/DPI/theme");
    const auto original = app.editorText;
    size_t paragraph = app.editorText.find(L"\uCC3D\uC744 \uC881\uAC8C");  // the long wrapping paragraph
    check(paragraph != std::wstring::npos, "fixture has the long wrapping paragraph");
    if (paragraph == std::wstring::npos) return;
    size_t paragraphEnd = app.editorText.find(L'\n', paragraph);
    int positions = 0;

    for (int theme : {0, 5}) for (float scale : {1.0f, 1.5f, 2.0f}) {
        applyTheme(app, theme);
        app.contentScale = scale;
        updateTextFormats(app);
        for (int width : {650, 1050}) for (bool preview : {true, false}) {
            app.width = width;
            app.editorShowPreview = preview;
            for (bool wrap : {true, false}) {
                app.editorWordWrap = wrap;
                app.editorText = original;
                rebuildLineStarts(app);
                // start of line, middle (mid word), and the last chars of the paragraph
                for (size_t at : {paragraph, paragraph + 33, paragraphEnd - 2}) {
                    FakeIme ime;
                    app.ime = App::ImeComposition{};
                    app.editorText = original;
                    rebuildLineStarts(app);
                    app.editorHasSelection = false;
                    app.editorCursorPos = at;
                    app.editorScrollX = 0;
                    app.undoStack.clear();
                    app.redoStack.clear();
                    imeStartComposition(app, ime);
                    send(app, ime, COMP, L"\uD55C\uAE00");
                    ++positions;

                    D2D1_POINT_2F caret{}, start{};
                    check(editorCaretPoint(app, caret), "caret geometry available while composing");
                    check(editorPointForPos(app, app.ime.start, start), "composition start geometry available");
                    Expected caretExpected = independentPoint(app, app.ime.start + app.ime.caret);
                    Expected startExpected = independentPoint(app, app.ime.start);
                    check(caretExpected.ok && std::fabs(caret.x - caretExpected.x) < 0.75f &&
                          std::fabs(caret.y - caretExpected.y) < 0.75f,
                          "caret matches an independent DirectWrite hit test of the composed line");
                    check(startExpected.ok && std::fabs(start.x - startExpected.x) < 0.75f &&
                          std::fabs(start.y - startExpected.y) < 0.75f,
                          "composition start (candidate anchor) matches the layout");
                    check(caret.y >= start.y - 0.5f, "caret is never above the composition start");

                    // What follows the composition must be pushed aside, not overdrawn
                    if (app.ime.start + app.ime.length < app.editorText.size() &&
                        app.editorText[app.ime.start + app.ime.length] != L'\n') {
                        Expected after = independentPoint(app, app.ime.start + app.ime.length);
                        Expected before = independentPoint(app, app.ime.start);
                        if (after.ok && before.ok && std::fabs(after.y - before.y) < 0.5f) {
                            check(after.x > before.x + 1.0f,
                                  "text after the composition starts to its right (no overlap)");
                        }
                    }
                    // The editor renders (underline included) without error
                    app.cursorBlinkOn = true;
                    app.renderTarget->BeginDraw();
                    renderEditor(app, editorPaneWidth(app));
                    check(SUCCEEDED(app.renderTarget->EndDraw()), "editor paints the composition + underline");

                    // Commit, then the buffer is exactly the original plus the syllables
                    send(app, ime, RESULT, L"", L"\uD55C\uAE00");
                    imeEndComposition(app, ime);
                    check(app.editorText == original.substr(0, at) + L"\uD55C\uAE00" + original.substr(at),
                          "commit inserts at the composition position");
                }
            }
        }
    }
    check(positions == 2 * 3 * 2 * 2 * 2 * 3, "every theme/scale/width/preview/wrap/position combination ran");
    // restore
    app.contentScale = 1.0f;
    app.width = 1050;
    app.editorWordWrap = false;
    app.editorShowPreview = true;
    updateTextFormats(app);
    app.editorText = original;
    rebuildLineStarts(app);
    app.ime = App::ImeComposition{};
}

// ---- Save and tab switch through the real editor/tab functions ----

std::wstring readFile(const std::string& path) {
    std::ifstream in(std::wstring(path.begin(), path.end()), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string bytes = ss.str();
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring wide(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), &wide[0], n);
    return wide;
}
void writeFile(const std::string& path, const std::string& bytes) {
    std::ofstream out(std::wstring(path.begin(), path.end()), std::ios::binary);
    out << bytes;
}
size_t occurrences(const std::wstring& haystack, const std::wstring& needle) {
    size_t n = 0, at = 0;
    while ((at = haystack.find(needle, at)) != std::wstring::npos) { ++n; at += needle.size(); }
    return n;
}

void saveAndTabSwitch(const std::string& dir) {
    beginCase("save and tab switch commit the composition to the previous target exactly once");
    CreateDirectoryW(std::wstring(dir.begin(), dir.end()).c_str(), nullptr);
    const std::string first = dir + "\\ime-first.md", second = dir + "\\ime-second.md";
    writeFile(first, "# First\n\nbody\n");
    writeFile(second, "# Second\n\nother\n");

    // -- save with a composition in flight (the production Win32 source: an
    //    offscreen STATIC window has no IME, so this exercises the fallback) --
    {
        auto state = makeApp(first);
        if (!state) { check(false, "app initializes"); return; }
        App& app = *state;
        app.editorCursorPos = app.editorText.size();
        FakeIme ime;
        imeStartComposition(app, ime);
        send(app, ime, COMP, L"\uD55C");
        check(app.editorText.find(L"\uD55C") != std::wstring::npos, "composing \uD55C before saving");
        saveEditorFile(app, app.hwnd);
        std::wstring onDisk = readFile(first);
        check(occurrences(onDisk, L"\uD55C") == 1, "Ctrl+S wrote the composing syllable exactly once");
        check(!app.ime.active && occurrences(app.editorText, L"\uD55C") == 1,
              "no leftover composition and no duplicate in the buffer");
        check(!app.editorDirty, "the saved buffer is clean");
    }
    // -- tab switch: the syllable belongs to the tab being left --
    {
        writeFile(first, "# First\n\nbody\n");
        auto state = makeApp(first);
        if (!state) { check(false, "app initializes"); return; }
        App& app = *state;
        app.editorCursorPos = app.editorText.size();
        FakeIme ime;
        imeStartComposition(app, ime);
        send(app, ime, COMP, L"\uAE00");
        tabOpenPath(app, app.hwnd, second, true, -1);
        check(!app.ime.active, "switching tabs leaves no composition behind");
        check(app.currentFile == second, "the second document is now active");
        check(app.editorText.find(L"\uAE00") == std::wstring::npos,
              "the new tab's buffer did not receive the syllable");
        bool parked = false;
        for (const auto& tab : app.tabs) {
            if (tab.path == first) parked = occurrences(tab.editorText, L"\uAE00") == 1;
        }
        check(parked, "the parked first tab holds \uAE00 exactly once");
    }
}

}  // namespace

int main() {
    const std::string dir = TINTA_IME_TEST_DIR;
    auto state = makeApp(TINTA_IME_FIXTURE);
    if (!state) {
        std::cerr << "cannot initialize the native renderer\n";
        return 2;
    }
    App& app = *state;
    const std::wstring fixtureText = app.editorText;

    hangulSyllable(app);
    sentenceSequence(app);
    resultPlusNewComposition(app);
    emptyVersusFailed(app);
    cursorPosition(app);
    selectionReplacement(app);
    repeatedEndAndCancel(app);
    commitPendingBoundaries(app);
    keyPolicy(app);
    otherInputTargets(app);
    windowMessageRouting(app);
    surrogateSafety(app);
    extraCarets(app);
    // the layout case needs the fixture document back
    app.editorText = fixtureText;
    rebuildLineStarts(app);
    layoutAndDisplay(app);
    state.reset();
    saveAndTabSwitch(dir);

    for (const auto& c : caseAssertions)
        std::cout << "  case: " << c.first << " (" << c.second << " assertions)\n";
    std::cout << "IME composition: " << caseAssertions.size() << " cases, "
              << assertions << " assertions, " << failures << " failures\n";
    CoUninitialize();
    return failures ? 1 : 0;
}

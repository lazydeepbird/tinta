#ifndef TINTA_EDITOR_H
#define TINTA_EDITOR_H

#include "app.h"
#include <windows.h>

// Mode transitions
void enterEditMode(App& app);
// Ctrl+N quick note: edit mode on an untitled empty buffer with no backing
// file; the first save routes through the classic Save As dialog
void enterQuickNoteMode(App& app);
void enterRecoveredDraft(App& app, HWND hwnd, const std::string& content,
                         const std::string& origPath);
void exitEditMode(App& app);
void setEditorReadingPreview(App& app, bool reading);
// The Read button (#236) fades in for a hint session's intro and while the
// pointer rests on it, and is hidden and click-through otherwise (#245)
D2D1_RECT_F editorReadingButtonRect(const App& app);
void renderEditorReadingButton(App& app);
bool editorReadingButtonShown(const App& app);
bool editorReadingButtonNeedsTicks(const App& app);
void editorReadingButtonHover(App& app, float x, float y);
// Word and character count beside the Read button (#240)
void renderEditorWordCount(App& app);
// Alt+Up/Down move lines, Ctrl+Alt+Up/Down add carets (#251); false when
// the key is not one of them or the source editor is not in front
bool editorAltArrowKey(App& app, HWND hwnd, WPARAM wParam);
void editorClearExtraCarets(App& app);
// Multi-caret clipboard text (#251): every selection, joined by line
// breaks; pasting a text with one line per caret hands each its own line
std::wstring editorCaretSelectionsText(const App& app);
void editorPasteAtCarets(App& app, HWND hwnd, const std::wstring& paste);

// Editor input handlers
void handleEditorKeyDown(App& app, HWND hwnd, WPARAM wParam);
void handleEditorCharInput(App& app, HWND hwnd, WPARAM wParam);
void editorPositionImeWindow(App& app, HWND hwnd);
void handleEditorMouseDown(App& app, HWND hwnd, int x, int y);
void handleEditorMouseUp(App& app, HWND hwnd, int x, int y);
void handleEditorMouseMove(App& app, HWND hwnd, int x, int y);
void handleEditorMouseWheel(App& app, HWND hwnd, float delta);
// A dragged selection (#289): TIMER_EDITOR_DRAG_SCROLL scrolls the editor
// while the pointer is past its top or bottom edge; Esc, a lost capture or
// a cancelled mode put the drag away and leave the text as it was
void editorTextDragScroll(App& app, HWND hwnd);
void editorTextDragCancel(App& app, HWND hwnd);

// Editor rendering
void renderEditor(App& app, float editorWidth);
// Screen-space caret origin shared by rendering and IME anchoring.
bool editorCaretPoint(App& app, D2D1_POINT_2F& point);
// Screen-space origin of an arbitrary buffer offset (same coordinate space as
// editorCaretPoint). Used to anchor the IME candidate window at the start of
// the composition rather than at its caret.
bool editorPointForPos(App& app, size_t pos, D2D1_POINT_2F& point);
// Height of one source line (row) in pixels, 0 when there is no editor font.
float editorLineHeightPx(const App& app);

// --- IME composition primitives (ime.cpp drives these) ---
// Splice text into the buffer with no undo, dirty, or cursor side effects;
// rebuilds the line index and invalidates the layout cache.
void editorImeSplice(App& app, size_t pos, size_t oldLen,
                     const std::wstring& text);
// Record a committed composition as ONE edit: an Insert (coalescing like
// typed characters) or, when it replaced a selection, a Replace that Undo
// turns back into that selection. Marks the buffer dirty.
void editorImeRecordCommit(App& app, size_t pos, const std::wstring& removed,
                           const std::wstring& inserted, size_t cursorBefore,
                           size_t selStartBefore, size_t selEndBefore,
                           bool hadSelection);
// Scroll the caret into view and repaint after a composition change.
void editorImeRefresh(App& app);
// Undo/redo entry points for tests and IME-aware callers
void editorUndoForTest(App& app);
void editorRedoForTest(App& app);

// Re-enter edit mode from a parked tab buffer (tabs.cpp), restoring the
// dirty flag, scroll, and caret the switch-away captured
void restoreEditBuffer(App& app, const std::wstring& text, bool dirty,
                       float scrollY, size_t cursor);

// Quick-note empty state (design 4a): an untitled buffer with no text yet
// shows a centered "Open a file" button in the preview pane until typing
// starts; the button (or Ctrl+O) opens the classic file picker
bool quickNoteEmptyStateActive(const App& app);
// Open in a tab, preserving existing/unsaved documents in either mode.
void openFileDialog(App& app, HWND hwnd);
void renderQuickNoteEmptyState(App& app);

// File save; Save As (Ctrl+Shift+S) re-prompts for the path and moves the
// tab's identity to the chosen file
void saveEditorFile(App& app, HWND hwnd);
void saveEditorFileAs(App& app, HWND hwnd);
// Ctrl+Shift+S anywhere: editor writes the buffer to a new path, viewer
// copies the viewed file; the tab follows the new name either way
void saveFileAs(App& app, HWND hwnd);
// Unsaved-changes dialog outcome: 1 save+exit, 2 discard, 3 keep editing
void confirmExitAction(App& app, HWND hwnd, int action);

// Editor reparse (called from timer)
// Exports need the current buffer even when the preview pane is hidden.
void editorReparse(App& app, bool force = false);
// External edits: undo-capable range replace plus dirty + immediate
// reparse, used by the preview table cell editor (#148)
void editorReplaceRangeExternal(App& app, size_t start, size_t end,
                                const std::wstring& repl);
void editorMarkDirtyAndReparse(App& app);

// Editor search
void performEditorSearch(App& app);
void scrollEditorToMatch(App& app);
// Find & replace (#121): replace the current match / every match with
// App::replaceText; each hunk is one undo entry
void editorReplaceCurrent(App& app, HWND hwnd);
void editorReplaceAll(App& app, HWND hwnd);

// Utility
// Navigate without changing the source buffer, including when preview is hidden.
void editorScrollToSourceOffset(App& app, size_t byteOffset);
void rebuildLineStarts(App& app);
size_t editorTopVisibleLine(App& app);
// Scrolls the edit-mode preview to follow the editor's top line
void syncPreviewToEditor(App& app);
// The other way: the editor scrolls to the source line at the preview's
// top, for a drag on the preview's scrollbar (#280)
void editorFollowPreview(App& app);

// Ctrl+G, go to line (#276): a prompt over the editor or the reading view.
// While open it takes every key; Enter jumps, Esc closes. The key-down
// consumes only those and Backspace, so the message loop turns the other
// keys into the characters gotoLineChar types (#282).
void gotoLineOpenPrompt(App& app);
bool gotoLineKeyDown(App& app, HWND hwnd, WPARAM key);
void gotoLineChar(App& app, wchar_t ch);
// The prompt's box, and whether a point in client pixels falls on it
D2D1_RECT_F gotoLinePromptBox(const App& app);
bool gotoLinePromptHit(const App& app, float x, float y);
// Lines in the document being edited or read
size_t gotoLineCount(const App& app);
// 1-based, clamped to the document: the editor's caret moves to the line's
// start, centred; the reading view scrolls to where the line is drawn
void gotoLineJump(App& app, size_t line);
void renderGotoLinePrompt(App& app);
std::string toUtf8(const std::wstring& wstr);

#endif // TINTA_EDITOR_H

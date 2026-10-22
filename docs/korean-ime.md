# Inline IME composition

The Markdown **source editor** draws the IME composition itself (Korean,
Japanese, Chinese). Previously the editor only moved the system composition
window near the caret and left the rest to `DefWindowProc`, so the syllable
being composed lived in a separate floating window below the line.

The state machine lives in `src/ime.cpp` / `include/ime.h`; the window
procedure only forwards the `WM_IME_*` messages to it.

## Design

While the user composes, the composition string is **spliced into
`App::editorText`** at `[ime.start, ime.start + ime.length)`. The normal
DirectWrite line layout, soft wrapping, caret, scrolling and horizontal scroll
therefore apply to it unchanged, and the text after the caret reflows instead
of being overdrawn. The range is underlined in both render paths.

Invariants (details in `ime.h`):

- One composition is one transaction. Interim strings (jamo, syllable, final
  form) only rewrite the spliced range: no Undo entry, no dirty flag, no
  preview reparse.
- A commit is one edit: an `Insert` that coalesces like typed characters, or,
  if the composition overwrote a selection, one `Replace` whose Undo restores
  that selection (`EditAction::hadSelection`).
- Cancel removes the spliced text and restores the overwritten selection.
- The IME's final text arrives only via `WM_IME_COMPOSITION` +
  `GCS_RESULTSTR`. The message is consumed rather than forwarded, so
  `DefWindowProc` never produces a second `WM_IME_CHAR`/`WM_CHAR`.
- `ImmGetCompositionStringW` returns bytes for strings and a character index
  for `GCS_CURSORPOS`; a valid empty string (`ImeQuery::Empty`) is distinct
  from an API failure (`ImeQuery::Failed`), which never clears anything.
- A composition happens at the primary caret only. Starting one drops the
  extra carets (#251), as a plain click does, so a syllable is never typed
  into several places with a single Undo step that cannot follow it.

## Who owns the composition

`imeOwnsInput()` is true only when the source editor is the input target. The
find/replace fields, the table cell editor, dialogs and the reading preview
return "not handled" and keep the default Windows IME UI and the
`WM_IME_CHAR` -> `WM_CHAR` path they always used. `WM_IME_SETCONTEXT` clears
`ISC_SHOWUICOMPOSITIONWINDOW` (and only that flag; the candidate list stays)
while the editor owns input, and is re-sent when the target kind changes.

## Commit points

`imeCommitPending()` runs before anything reads or replaces the buffer: save,
tab switch, mouse press, context menu, focus loss (which covers minimizing),
close, leaving edit mode, an external-change prompt, and any non-modifier key.
It asks the IME to finish (`CPS_COMPLETE`); if the IME does not answer, what
is displayed is kept and the IME's own copy is cancelled (its flush messages
are ignored) so it cannot re-send and duplicate the text. Escape cancels a
live composition first. `VK_PROCESSKEY` key-downs are left to the IME and
never reach shortcut code.

## Tests and fixture

- `ime_composition` (`tests/ime_tests.cpp`): a scripted IME source drives the
  production state machine and the real editor layout (wrap, zoom, DPI, theme,
  preview on/off, extra carets). It runs from an isolated copy with its own
  `settings.ini` (`tests/run_isolated_test.cmake`).
- `tests/fixtures/korean-ime.md` is the mixed Korean/English/table/code/
  Mermaid document for manual checks.

These prove the logic, ordering and failure handling. They do not replace
typing with the Microsoft Korean IME in the built app.

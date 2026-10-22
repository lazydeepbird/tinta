# Inline IME composition

Validated on Windows 11 on 2026-10-23, on top of upstream v3.9.0.

## Request

Korean input in the source editor showed the syllable being composed in a
separate floating IME window below the line. It should compose in place, at
the caret, like other editors.

## Change

- The composition is spliced into the editor text and underlined, so wrapping,
  scrolling and zoom apply to it and the text after the caret moves aside.
- A committed syllable is one undoable edit. Escape cancels a composition and
  brings back a selection it replaced; Undo of a replacement does the same.
- A pending composition is committed before save, tab switch, click, context
  menu, focus loss (and so minimizing), close, leaving edit mode and the
  external-change prompt, so it is never lost or inserted twice.
- A composition starts at the primary caret and drops the extra carets
  (#251).
- The find bar and table cell editor keep the default Windows IME window.

Design notes: `docs/korean-ime.md`.

## Fixtures and automated checks

`fixtures/korean-ime.md` mixes Korean, English, digits and emoji with
emphasis, inline code, a nested list, a quote, a long wrapping paragraph, a
table, a JSON code block, a Mermaid flowchart, an anchor link and a
multi-caret section.

`ime_composition` (`ime_tests.cpp`) drives the production state machine with
a scripted IME source: jamo to syllable to commit, result plus a following
composition in one message, empty versus failed queries, cursor position,
selection replacement and its Undo, repeated end and cancel, commit before
save and tab switch, key policy, other input targets, message routing,
surrogate pairs, extra carets, and caret and candidate anchors against an
independent DirectWrite hit test under wrap, zoom, DPI, theme and preview
on/off.

## Results

- MinGW Debug build (no MSVC on the test PC): `ime_composition` passes. The
  rest of CTest matches an upstream v3.9.0 baseline built the same way;
  `ui_font_language`, `wrapped_cursor_navigation`, `wide_table_layout` and
  `plantuml_parser` fail on both for environmental reasons.
  `editor_context_selection` and `table_and_modifier_input` share the
  clipboard and can fail under `ctest -j`; run serially they pass on both.
- Built app with the Microsoft Korean IME (2-beolsik), isolated portable
  folder, on the fixture: the maintainer reported inline composition,
  Undo/Escape, commit on save/click/focus/tab switch, wrapped lines and extra
  carets working.

## Limitations

- Not yet built or tested with MSVC.
- Japanese and Chinese IMEs were not typed with by hand; they go through the
  same messages.

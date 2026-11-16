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

## Live (upstream v4.0.1)

- Rebased on upstream v4.0.1 and extended `imeOwnsInput()` to Live, which
  edits `editorText` directly. `ime_composition` gained the case "Live:
  composition on the page, one commit per syllable, menus keep the default
  IME": a mixed document (heading, bold, link, nested list, quote with code,
  table) with the caret after a list item. It checks the splice, that Live's
  text and caret follow it, the candidate anchor (right of the rail, on the
  composition's line), one commit per syllable, Undo, and that Live's menu
  and the go-to-line prompt leave the IME to the default path.
- Frames `ime-live-composing.png` (the syllable underlined in the list item,
  the block's markdown shown as typed) and `ime-live-committed.png` (the
  word, no underline) were inspected in the dark theme at 1050 px.
- MinGW Debug harness: 51/59 pass. The 8 failures fail on pure upstream
  v4.0.1 (or are known on this PC): `ui_font_language`,
  `wrapped_cursor_navigation`, `wide_table_layout`, `highlight_layout`,
  `plantuml_parser`, `image_cache_and_decode` (network share paths, the same
  7 failures on a master build), and `editor_context_selection` /
  `code_block_layout`, which pass when run serially.
- Not checked: the Microsoft IME typed by hand in Live in the built app,
  narrow widths and the light theme in Live, and sheet cards (they keep the
  floating composition window).

## File watch (upstream v4.0.2)

- Upstream v4.0.2 reloads a clean Source or Live buffer when another program
  changes the file (#311). A composing syllable is spliced into `editorText`
  without marking it dirty, so a watch tick during a composition diffed the
  syllable away and left the IME range pointing past the new text. The watch
  now waits while a composition is open, and `editorReloadFromDisk()` commits
  any composition first.
- `ime_composition` gained the case "an external file change waits for the
  composition to commit": compose on a clean buffer, change the file on disk,
  run the watch; the syllable and its range stay and nothing reloads. After
  the commit the buffer is unsaved and the next tick asks, Reload or Keep
  mine. With the fix removed, three of its checks fail.
- Rebased the flat tab style onto pinned tabs (#312): the dividers run
  between the pinned tabs and the tabs in view, placed by `tabLeftAt()`.
- MinGW Debug harness: 54/62 pass; the 8 failures are the known ones
  (`editor_context_selection` and `code_block_layout` pass serially).

## Open mode and find bar (upstream v4.1.1)

- Upstream v4.1.1 can open a Markdown file straight into Live or Source
  (#322, `openMode=`). `ime_composition` gained the case "a file opened
  straight into Live or Source composes inline": a syllable composing in one
  tab, a second file opened with `openMode=live` and then `source`. The tab
  left keeps its syllable exactly once, nothing crosses into the new tab, the
  new tab is in the chosen view at once, and its first syllable composes at
  the caret (in Live with a place on the page) and commits once.
- Release build in an isolated folder: `openMode=live` with
  `fixtures/korean-ime.md` opens on Live's page with the rail's pill on Live.
  Hangul posted as WM_CHAR into the reading view's find bar (vim profile, `/`)
  stands on the bar's line beside the count (#319). Screenshots under
  `build/tinta-codex/smoke411-*/out/`.
- Not checked: the Microsoft IME typed by hand into a file opened in Live, and
  IME composition inside the find bar (it keeps the floating window).
- MinGW Debug harness: 81/86 pass; the 5 failures are the known ones
  (`ui_font_language`, `wrapped_cursor_navigation`, `image_cache_and_decode`,
  `link_opening`, `plantuml_parser`).

## Limitations

- Not yet built or tested with MSVC.
- Japanese and Chinese IMEs were not typed with by hand; they go through the
  same messages.

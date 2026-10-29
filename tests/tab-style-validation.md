# Flat tab style

Validated on Windows 11 with the MinGW Release build on 2026-10-29.

## Change

`renderTabStrip` (`src/tabs.cpp`) draws the tab row in a flatter, sharper
style:

- every tab starts 5 DIPs below the top of the 40 DIP strip; the active tab
  no longer rises 4 DIPs above the others
- tab bodies are plain rectangles (the 8 DIP rounded tops are gone)
- the active tab fills with the page colour and has a 1 DIP outline on its
  left, top and right, open at the bottom where it meets the page; the
  three-ring shadow around it is gone
- a 1 DIP divider, inset 8 DIPs from the tab's top and bottom, separates
  neighbouring tabs unless one of them is active, hovered or being dragged
- hovered tabs fill with the same faint tint as before, now square

The strip height, tab widths, gaps, close buttons, status dots and hit
rectangles are unchanged, so dragging, closing and the tab wheel behave as
before. Colours come from the theme as before.

The clip around the row now reaches one pixel past its ends (it reached past
for the shadow before, #271), so the outline of an active first or last tab
is not cut.

## Automated checks

`tab_hover_card` (`endTabsKeepTheirOutline`, which replaces the halo check
from #271): with the first and then the last of four fixture tabs active, in
two themes at 100 % and 150 %, the pixels on the hairline just inside the
tab's outer edge must differ from the same pixels when a middle tab is
active, and match the inner edge within 3 levels.

## Built-app checks

The Release exe ran from an isolated portable folder. Tabs were added to its
own window with `WM_COPYDATA` and the window was captured with
`PrintWindow`, before and after the change:

- Paper (light) and Midnight (dark) at 1100 px: dividers between inactive
  tabs, none beside the active tab or after the last tab; outline visible on
  the active tab in both themes
- Arctic (light) after raising the light-theme outline from 0.18 to 0.28
  alpha
- Arctic at 560 px: a single visible tab with the overflow chevron; its left
  outline is not clipped

Not checked by eye: hover over an inactive tab, tab drag and tear-off, 150 %
display scaling (covered only by the automated check).

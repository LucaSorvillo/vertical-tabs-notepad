# Changelog

## 1.1.0 — 2026-11-06

- The app follows the Windows display language: Italian if Windows is in Italian, English otherwise. This covers
  the tray menu, the "Tab color" menu and its colors, tooltips and accessibility names.
- The setup chooses its language the same way, without asking.
- Documentation and website with English screenshots. The website picks its language from the browser, with
  English as the fallback.

## 1.0.0 — 2026-11-06

First public release.

- Vertical tabs in a column from the top of the window, with the title row hidden and the menu bar next to the
  window buttons. Click, middle-click, drag to reorder, Notepad's own tab menu, collapse to icons, resizable
  width.
- Tab colors (the colors of Chrome's tab groups), saved across restarts.
- "Back to top" button in the editor.
- <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>T</kbd> reopens closed tabs in their place, up to 25, also after
  Notepad restarts.
- Mouse wheel without smooth scrolling (optional), scrollbars that expand at once on hover, fixes for two
  Notepad scrollbar glitches.
- The <kbd>Ctrl</kbd>+<kbd>F</kbd> find bar moves with the editor when the column is shown.
- Per-user installer (English and Italian), no administrator rights needed.

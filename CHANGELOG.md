# Changelog

## 1.3.0 — 2026-11-07

- **Exit** now removes the app from Notepad completely: the module leaves the open Notepad windows, and nothing of
  the app stays inside Notepad (1.2.3 only switched it off). Starting the app again brings it back into the windows
  already open.
- Updates take effect without closing Notepad: the old module leaves and the new one comes in.
- The module finds Notepad's tab bar, menu bar and scrollbars without XAML diagnostics (the API of Visual Studio's
  Live Visual Tree), which kept the module loaded for good. It now hooks a few public WinUI methods, and puts them
  back when it leaves.

## 1.2.3 — 2026-11-07

- **Exit** in the tray menu now turns everything off in the open Notepad windows right away (the vertical tabs used
  to stay until Notepad was closed). Everything comes back when you start the app again, and the closed-tabs
  history stays. Uninstalling the app does the same.

## 1.2.2 — 2026-11-07

- The active tab in the column shows the unsaved-changes dot instead of the X, like Notepad's own tabs: the X
  appears when the mouse is over the tab, and stays on the active tab when it has no unsaved changes.

## 1.2.1 — 2026-11-07

- Fixed: clicking a line to switch back to Notepad from another app could make the text jump and put the caret,
  or a selection, on another line. Notepad puts its windows back in their original place every time the window
  is activated; the layout now keeps them where they are.
- A plain click on the empty part of the title row activates the window without moving the text. Dragging,
  double-click and right-click work as before.

## 1.2.0 — 2026-11-06

- <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>T</kbd>: the reopened tab gets its color back too, like in VTabsExplorer.
- App icon: the tray icon on a dark tile, for the executable, the setup, the website and the README.
- The tray icon tooltip shows only the app name.

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

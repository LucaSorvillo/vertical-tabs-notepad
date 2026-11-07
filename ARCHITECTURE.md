# Architecture

A technical overview of VTabsNotepad. The complete development notes, with the reasons behind each choice, are
in [DESIGN-NOTES.it.md](DESIGN-NOTES.it.md) (Italian). Code comments are in Italian too.

## Overview

```
VTabsNotepad.exe (tray launcher)                    Notepad.exe (one process, all windows)
├─ settings.ini, tray menu, autostart               ├─ Win32 frame + XAML islands (tabs, menu bar,
├─ finds Notepad processes (Toolhelp)               │  status bar, scrollbars) + RichEdit editor
└─ injects ──────── CreateRemoteThread ──────────►  └─ VTabsNotepadMod.dll
                    LoadLibraryW                       ├─ XAML diagnostics (TAP) → visual tree events
                                                       ├─ per window: WindowLayout + features
                                                       └─ own XAML islands (column, back-to-top)
```

The modern Notepad is a Win32 window hosting several WinUI 3 XAML islands (tab bar, menu bar, status bar,
scrollbars) and a `RichEdit` editor, which runs on its own thread for each tab. A single process serves all
windows.

## Launcher (`src/launcher`)

- `main.cpp`: tray icon and menu, settings, "start with Windows" (`HKCU\...\Run`), command line
  (`--exit`, `--inject`, `--inject-pid <pid>`, `--reopen-tab`, `--dump`).
- `Injector.cpp`: finds Notepad processes (Toolhelp snapshots, checked again when the shell hook reports a new
  window) and skips those that already have the module. The DLL is copied to
  `%LOCALAPPDATA%\VTabsNotepad\runtime\<stamp>\` (stamp = time and size of the file) and that copy is loaded
  with `CreateRemoteThread` + `LoadLibraryW`. The installed DLL is therefore never locked, and a new build can
  be installed while Notepad runs. Old copies are deleted when no longer in use.
- `TrayIcon.cpp`: the monochrome icon, drawn with GDI+ at the exact size for the current DPI and theme.

## Shared code (`src/common`)

`Settings` reads and writes `settings.ini`. The module reads it too: it watches the data folder
(`FindFirstChangeNotificationW`) and applies the new settings as soon as the tray menu changes them. `Shared`
holds paths and names shared by launcher and module.

## Module (`src/mod`)

### Getting into the XAML tree

`dllmain.cpp` starts a bootstrap thread. `ModCore` calls `InitializeXamlDiagnosticsEx` (exported by
`Microsoft.Internal.FrameworkUdk.dll`), the API that Visual Studio's Live Visual Tree uses, passing its own
DLL as the "TAP". The XAML runtime calls `Tap.cpp` (`IObjectWithSite::SetSite`) with an
`IXamlDiagnostics`. `VisualTreeWatcher` subscribes to `IVisualTreeServiceCallback2`: every element added or
removed in any window is reported, so each window's `TabsBar` and the other islands are found as soon as they
exist.

The C++/WinRT projections of `NotepadXamlUI.winmd` (public metadata shipped with Notepad) give typed access
to `TabsBar`, `TabItemViewModel` and their commands (`ReorderTab`, `SelectTabByTabId`, ...).

### Per-window layout (`WindowLayout.cpp`)

- Keeps the rectangle Notepad gives to each child window and translates it out of the column. It
  re-aligns on `WM_SIZE`, right after the activation messages, on WinEvents and with a periodic check.
- **Activation.** Notepad re-applies its own rectangles every time the window is activated, without a size
  change. `PositionPin.cpp` keeps the windows the layout moves (editor, text surface, scrollbar islands, menu
  bar) where they are: a subclass on each window's own thread turns a `WM_WINDOWPOSCHANGING` that asks again for
  the same natural rectangle into "no move". Otherwise the editor was resized in the middle of the click that
  activated the window: the click landed on another line and `RichEdit` scrolled the caret into view.
- In the hidden title row a plain click activates the window without entering the move loop (`DragDetect`).
  Leaving that loop makes the input stack ask the editor for the caret position (`IMR_QUERYCHARPOSITION`) and
  `RichEdit` scrolls it into view, as Notepad itself does on its own title bar. Dragging, double-click and
  right-click are unchanged.
- Hides the title row: the menu bar moves up next to the window buttons, and the editor and its scrollbars
  grow upwards.
- **Floating islands** (the <kbd>Ctrl</kbd>+<kbd>F</kbd> find bar) are moved with the editor at their natural size,
  never resized, because their content is laid out for that size. They are never raised in Z-order either:
  the editor clips its siblings, and raising them would leave a black band.
- Window **regions** with holes make the empty parts of the column and of the menu bar behave as the title bar
  (drag, double-click, system menu). XAML islands don't use `WM_NCHITTEST`, so `HTTRANSPARENT` isn't enough.
- Notepad's main window has no `WS_CLIPCHILDREN`. After its own `WM_PAINT`, the part of the editor it painted
  over is invalidated.

### Window chrome

- `Glass.cpp`: Notepad extends the frame into the client area (`DwmExtendFrameIntoClientArea`) down to the
  menu bar. With the title row hidden, that would turn the top of the editor lighter: an import-table hook
  caps the top margin at the editor.
- `CaptionButtons.cpp`: the DWM caption buttons are hidden with the undocumented `0x10000` flag of
  `SetWindowThemeAttribute(WTA_NONCLIENT)`, the one the Windows App SDK uses. They are replaced by a layered
  child window drawn with Direct2D: same glyphs and colors as Windows 11, `HTMINBUTTON`/`HTMAXBUTTON`/`HTCLOSE`
  hit-testing, system menu and tooltips. Its alpha is 1/255 where it looks transparent, otherwise clicks would
  pass through.

### Features (`src/mod/features`)

| File | Feature |
| --- | --- |
| `VerticalTabs.cpp` | The column: a `ListView` of rows bound to Notepad's tab view models, in its own `DesktopWindowXamlSource` with a `MicaBackdrop`. Switch, close, new tab, drag reordering (`TabsBar.ReorderTab`), Notepad's context menu shown on the real tab plus the "Tab color" submenu, collapse, width. |
| `BackToTop.cpp` | Listens to the `ViewChanged` event of Notepad's vertical `ScrollViewer`. The button lives in its own layered island; it is hidden with opacity and `WS_EX_TRANSPARENT` instead of `ShowWindow`, because showing or hiding a window makes Windows synthesize a `WM_MOUSEMOVE` that changes Notepad's scrollbar. |
| `ReopenTab.cpp` | Detects closed tabs from `TabItems` changes, with a 0.5 s grace period because a reorder also removes and re-adds a tab. Stores path, position and color in `closed-tabs.txt`, behind a named mutex. Reopens a file by launching Notepad with it, then moves the new tab to its old place, gives it back its color (through the column, so the row updates at once) and selects it after Notepad's own selection change. <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>T</kbd> is registered with `RegisterHotKey` only while the window is active. |
| `WheelScroll.cpp` | A per-thread `WH_GETMESSAGE` hook on each editor thread turns `WM_MOUSEWHEEL` into `EM_LINESCROLL`, with a pixel fallback clamped to the end of the document. |
| `ScrollBarFit.cpp` | Notepad sizes its scrollbars for its own layout and sets them again at every layout pass. A synchronous property-changed callback adds the difference to both the viewport and the content. |
| `ScrollBarHover.cpp` | Sets WinUI's 0.4 s scrollbar expand delay to zero by editing the template's storyboards. |
| `ScrollRangeFix.cpp` | Works around two native `RichEdit` issues: a phantom vertical range when the text fits (fixed with `SetScrollInfo`), and XAML scrollbars that only re-read ranges on `EN_UPDATE` (so one is posted when the ranges change). |

### Robustness

- Every event handler toward the XAML runtime is guarded: no exception leaves the module's code.
- The module is never unloaded, because its hooks stay installed. Turning a feature off restores Notepad's
  layout. A new build takes effect when Notepad restarts.
- `TreeDump.cpp` exports the XAML tree (tray menu › "Esporta albero XAML") to diagnose a Notepad update.

## Languages

The texts the user sees (tray menu, the "Tab color" menu and the color names, tooltips, accessibility names) exist
in English and Italian. `UiItalian()` in `Shared.h` reads `GetUserDefaultUILanguage()` once per process: Italian if
the Windows display language is Italian, English otherwise (changing it requires signing out). `Tr(english,
italian)` picks the text. The XAML markup is written in Italian; in English, the user-facing attribute values are
replaced before `XamlReader::Load`. Logs stay in Italian. The installer chooses its language the same way (Inno
Setup detects the UI language; `ShowLanguageDialog=no`, English first). The test kit finds elements with
`UiName 'english' 'italian'`.

## Data files

All in `%LOCALAPPDATA%\VTabsNotepad`: `settings.ini`, `tab-colors.ini`, `closed-tabs.txt`, `logs\`,
`runtime\`. The [README](README.md#privacy) lists what each one contains.

## Build and tests

- `build.ps1` loads the Visual Studio x64 environment, generates the projections and builds with CMake + Ninja
  into `build\bin`.
- `tools\Make-Icon.ps1` draws the app icon, the tray icon on a dark tile, into `src\launcher\VTabsNotepad.ico` (used by the
  executable and the setup) and into the PNGs of the website and the README.
- `tools\Generate-Projections.ps1` runs `cppwinrt.exe` on the Windows SDK, on the Windows App SDK runtime that
  Notepad depends on, and on `NotepadXamlUI.winmd`. It rebuilds the projections when one of them changes.
- `installer\VTabsNotepad.iss` is the Inno Setup script: per-user install, no admin rights, English and Italian.
- `tools\test\Run-Tests.ps1` contains the end-to-end tests: real Notepad windows, UI Automation, and
  `VTabsNotepadUiaTool.exe` for context menus.

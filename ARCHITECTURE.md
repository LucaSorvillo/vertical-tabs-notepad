# Architecture

A technical overview of VTabsNotepad. The complete development notes, with the reasons behind each choice, are
in [DESIGN-NOTES.it.md](DESIGN-NOTES.it.md) (Italian). Code comments are in Italian too.

## Overview

```
VTabsNotepad.exe (tray launcher)                    Notepad.exe (one process, all windows)
├─ settings.ini, tray menu, autostart               ├─ Win32 frame + XAML islands (tabs, menu bar,
├─ finds Notepad processes (Toolhelp)               │  status bar, scrollbars) + RichEdit editor
└─ injects ──────── CreateRemoteThread ──────────►  └─ VTabsNotepadMod.dll
                    LoadLibraryW                       ├─ Discovery: vtable hooks + UI Automation search
                                                       ├─ per window: WindowLayout + features
                                                       └─ own XAML islands (column, back-to-top)
```

The modern Notepad is a Win32 window hosting several WinUI 3 XAML islands (tab bar, menu bar, status bar,
scrollbars) and a `RichEdit` editor, which runs on its own thread for each tab. A single process serves all
windows.

## Launcher (`src/launcher`)

- `main.cpp`: tray icon and menu, settings, "start with Windows" (`HKCU\...\Run`), command line
  (`--exit`, `--exit --keep-mods`, `--inject`, `--inject-pid <pid>`, `--reopen-tab`, `--dump`).
- `Injector.cpp`: finds Notepad processes (Toolhelp snapshots, checked again when the shell hook reports a new
  window) and skips those that already have the module. The DLL is copied to
  `%LOCALAPPDATA%\VTabsNotepad\runtime\<stamp>\` (stamp = time and size of the file) and that copy is loaded
  with `CreateRemoteThread` + `LoadLibraryW`. The installed DLL is therefore never locked, and a new build can
  be installed while Notepad runs. Old copies are deleted when no longer in use. A per-process mutex
  (`Local\VTabsNotepad.Inject.<pid>`) guards "already loaded? then inject" (two injections at once would leave two
  references to the DLL), and every 2 s the tray checks that the processes it handled still have the module (it
  injects it again, at most 3 times per process).
- `TrayIcon.cpp`: the monochrome icon, drawn with GDI+ at the exact size for the current DPI and theme.

## Shared code (`src/common`)

`Settings` reads and writes `settings.ini`. The module reads it too: it watches the data folder
(`FindFirstChangeNotificationW`) and applies the new settings as soon as the tray menu changes them. `Shared`
holds paths and names shared by launcher and module.

## Module (`src/mod`)

### Finding the windows (`Discovery.cpp`)

`dllmain.cpp` starts a bootstrap thread. It waits for the WinUI runtime; then `Discovery` replaces a few entries in
the method tables (vtables) of public WinUI interfaces, once per process, on a window's UI thread:

- `IFrameworkElementOverrides::OnApplyTemplate` of `TabView` and `MenuBar` (WinUI controls built by composition, so
  XAML calls them through the vtable): every new tab bar and menu bar presents itself.
- `IFrameworkElementOverrides::MeasureOverride` of `NotepadXamlUI.ScrollBar` (a Notepad `UserControl`; XAML calls
  the layout of custom types through the vtable): the editor's scrollbars.
- For the windows already open, a UI Automation search of each XAML island of the window makes XAML call
  `GetAutomationControlTypeCore` of the `TabViewItem` and `MenuBarItem` peers and `OnCreateAutomationPeer` of
  `NotepadXamlUI.ScrollBar` (it has no peer of its own, so XAML asks again at every visit). Outside our searches
  these hooks do nothing.

Every hook calls the original first and keeps only a weak reference; the rest happens on the window's thread. An
island that appears or becomes visible later (the vertical scrollbar, the find bar) is searched once. Until 1.2.3
the module used XAML diagnostics (`InitializeXamlDiagnosticsEx` with its own DLL as the "TAP"): the XAML runtime
loaded the DLL again and kept its object for good, so the module could never leave Notepad.

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
- **Exit** (tray menu or `--exit`, which the installer uses too) leaves `tray-exited.txt` in the data folder.
  The module watches the folder: every feature turns off (the closed-tabs history stays), then
  `ModCore::TryUnload` takes everything out and the DLL leaves with `FreeLibraryAndExitThread`:
  1. On each window's thread (`TeardownThread`): every feature detached (`Detach(restore)`), windows and parts
     forgotten.
  2. `discovery::Uninstall` and `glass::Uninstall` put the original vtable and import entries back (only if they
     are still ours); `pin::Uninstall` removes the subclasses from the editor windows, each from its own thread.
  3. Nobody inside our code: `lifetime::Calls()` counts the calls in progress in our subclasses, hooks and window
     procedures (a modal loop of Notepad, such as dragging the window, keeps one on the stack); then one turn of
     each window thread's message loop and a margin.
  4. The C++/WinRT module lock (XAML delegates, queued work) must reach zero, and the window class of the caption
     buttons is unregistered.

  A step that cannot complete is retried at most every 2 s; if the tray starts again first, the module resumes.
  The thread that reopens a closed tab holds its own reference to the DLL until it ends. A missing `settings.ini`
  counts as Exit too (data deleted when uninstalling). The launcher deletes `tray-exited.txt` when it starts;
  `--exit --keep-mods` (build and test scripts) exits without it. Updates take effect without restarting Notepad.
- **Work never nested in our own code** (`ThreadWork` in `ModCore.cpp`). The dispatcher can run a queued job while
  the window thread waits inside our code (for example in `SetWindowPos` on an editor window, which belongs to
  another thread). Detaching a window there destroyed the layout under `RestoreChildren` (a crash found while
  testing the unload). Before running, each job of the module walks its own stack (`RtlCaptureStackBackTrace`): if
  the module appears below the current call, the job runs again 100 ms later from the message loop.
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
`runtime\`, and `tray-exited.txt` while the app is closed with Exit. The [README](README.md#privacy) lists what each one contains.

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

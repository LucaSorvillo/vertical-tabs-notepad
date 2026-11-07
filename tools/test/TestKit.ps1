<#
.SYNOPSIS
    Kit di test di VTabsNotepad (da includere: . .\tools\test\TestKit.ps1).

.DESCRIPTION
    Helper per provare la mod dal vivo senza toccare il lavoro dell'utente, che nel Blocco note tiene
    anche note NON salvate:
      - i test agiscono su una finestra creata apposta (File > Nuova finestra) con file di prova in
        %TEMP%\npx-test; i titoli delle altre schede non vengono mai stampati (Safe-Name);
      - il Blocco note si chiude solo normalmente (WM_CLOSE), dopo un backup della sessione;
      - l'input reale (mouse) si interrompe da solo se il mouse viene mosso dall'esterno.

    Note sull'input sintetico: SetCursorPos non genera gli eventi puntatore di WinUI (WM_POINTER*),
    quindi i movimenti si inviano come input assoluto (mouse_event) aspettando che il cursore arrivi:
    l'input iniettato può essere elaborato in ritardo e non va "corretto" con SetCursorPos.
#>

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing
$AE = [Windows.Automation.AutomationElement]
$TS = [Windows.Automation.TreeScope]
$CT = [Windows.Automation.ControlType]

$NpxRepo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$NpxData = Join-Path $env:LOCALAPPDATA 'VTabsNotepad'
$NpxTestDir = Join-Path $env:TEMP 'npx-test'
$NpxTestPrefix = 'npx-test-'
$NpxLocalState = Join-Path $env:LOCALAPPDATA 'Packages\Microsoft.WindowsNotepad_8wekyb3d8bbwe\LocalState'
$NpxUiaTool = Join-Path $NpxRepo 'build\test\VTabsNotepadUiaTool.exe'
# Nomi dell'interfaccia nella lingua di Windows, la stessa del Blocco note e della mod: italiano, altrimenti inglese.
$NpxItalian = [Globalization.CultureInfo]::CurrentUICulture.TwoLetterISOLanguageName -eq 'it'
function UiName([string]$English, [string]$Italian) { if ($NpxItalian) { $Italian } else { $English } }

if (-not ('NpxKit' -as [type])) {
    Add-Type -Namespace '' -Name NpxKit -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
[StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize; public int flags; public IntPtr hCursor; public POINT pt; }
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] public static extern IntPtr SendMessageText(IntPtr h, uint m, IntPtr w, string l);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
[DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr dlg, int id);
[DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
[DllImport("user32.dll")] public static extern IntPtr GetDesktopWindow();
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
[DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
[DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO ci);
[DllImport("user32.dll")] public static extern IntPtr LoadCursor(IntPtr inst, IntPtr id);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, int data, IntPtr extra);
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
[DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
[DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
[DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr GetProp(IntPtr h, string name);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
[StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO { public int cbSize; public int flags; public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret; public RECT rcCaret; }
[DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint tid, ref GUITHREADINFO info);
[DllImport("user32.dll", SetLastError = true)] public static extern bool RegisterHotKey(IntPtr h, int id, uint mods, uint vk);
[DllImport("user32.dll")] public static extern bool UnregisterHotKey(IntPtr h, int id);
'@
}
# Coordinate fisiche ovunque (UI Automation, cursore, rettangoli)
[NpxKit]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null

# ------------------------------------------------------------------------------------------------
# Processo, finestre e schede
# ------------------------------------------------------------------------------------------------

# Processo del Blocco note con le finestre: riaprendo una scheda si lancia notepad.exe con il file, e
# per un istante esiste un secondo processo (che passa il file all'istanza vera e muore). Senza questo
# filtro gli helper che cercano per PID (voci di menu, log della mod) guarderebbero il processo sbagliato.
function Get-NpProcess {
    $all = @(Get-Process Notepad -ErrorAction SilentlyContinue)
    if ($all.Count -le 1) { return $all | Select-Object -First 1 }
    $withWindows = @($all | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero })
    if ($withWindows.Count -gt 0) { return $withWindows[0] }
    $all[0]
}

# Finestre principali (classe "Notepad") del processo del Blocco note
function Get-NpWindows {
    $p = Get-NpProcess
    if (-not $p) { return @() }
    $c = New-Object Windows.Automation.AndCondition(
        (New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)),
        (New-Object Windows.Automation.PropertyCondition($AE::ClassNameProperty, 'Notepad')))
    @($AE::RootElement.FindAll($TS::Children, $c))
}
function Get-Hwnd($Window) { [int64]$Window.Current.NativeWindowHandle }
function Win([int64]$Hwnd) { $AE::FromHandle([IntPtr]$Hwnd) }
function Find-ByName($Root, [string]$Name) {
    $Root.FindFirst($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::NameProperty, $Name)))
}
function Invoke-Element($Element) { $Element.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke() }
function Select-Element($Element) { $Element.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select() }
function Test-Selected($Element) { $Element.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected }

# Schede native (TabItem della striscia orizzontale nascosta) e righe del pannello
function Get-NativeTabs([int64]$Hwnd) {
    @((Win $Hwnd).FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::TabItem))))
}
function Get-PanelList([int64]$Hwnd) { Find-ByName (Win $Hwnd) (UiName 'Tabs' 'Schede') }
function Get-PanelRows([int64]$Hwnd) {
    $list = Get-PanelList $Hwnd
    if (-not $list) { return @() }
    @($list.FindAll($TS::Children, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::ListItem))))
}
# Titolo stampabile: solo i nostri file di prova; il resto diventa un segnaposto
function Safe-Name([string]$Name) {
    if ($Name.StartsWith($NpxTestPrefix)) { return ($Name -split '\.')[0] }
    if ($Name.StartsWith('Senza titolo') -or $Name.StartsWith('Untitled')) { return (UiName 'Untitled' 'Senza titolo') }
    return '<altra>'
}
function Get-NativeOrder([int64]$Hwnd) { @(Get-NativeTabs $Hwnd | ForEach-Object { Safe-Name $_.Current.Name }) }
function Get-PanelOrder([int64]$Hwnd) { @(Get-PanelRows $Hwnd | ForEach-Object { Safe-Name $_.Current.Name }) }
function Get-SelectedNative([int64]$Hwnd) { Get-NativeTabs $Hwnd | Where-Object { Test-Selected $_ } | ForEach-Object { Safe-Name $_.Current.Name } }
function Get-SelectedPanel([int64]$Hwnd) { Get-PanelRows $Hwnd | Where-Object { Test-Selected $_ } | ForEach-Object { Safe-Name $_.Current.Name } }
function Find-PanelRow([int64]$Hwnd, [string]$Prefix) { Get-PanelRows $Hwnd | Where-Object { $_.Current.Name.StartsWith($Prefix) } | Select-Object -First 1 }
function Find-Control([int64]$Hwnd, [string]$Name, $Type) {
    (Win $Hwnd).FindFirst($TS::Descendants, (New-Object Windows.Automation.AndCondition(
                (New-Object Windows.Automation.PropertyCondition($AE::NameProperty, $Name)),
                (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $Type)))))
}
# Esegue un elemento con il pattern che offre (Invoke, Toggle o SelectionItem)
function Invoke-Any($Element) {
    $patterns = $Element.GetSupportedPatterns()
    if ($patterns -contains [Windows.Automation.InvokePattern]::Pattern) { Invoke-Element $Element }
    elseif ($patterns -contains [Windows.Automation.TogglePattern]::Pattern) { $Element.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Toggle() }
    else { Select-Element $Element }
}
# Elemento del processo con quel nome (es. voci dei sottomenu), di qualunque tipo
function Find-InProcess([string]$Name) {
    $p = Get-NpProcess
    $AE::RootElement.FindFirst($TS::Descendants, (New-Object Windows.Automation.AndCondition(
                (New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)),
                (New-Object Windows.Automation.PropertyCondition($AE::NameProperty, $Name)))))
}
function Find-NativeTab([int64]$Hwnd, [string]$Prefix) { Get-NativeTabs $Hwnd | Where-Object { $_.Current.Name.StartsWith($Prefix) } | Select-Object -First 1 }
# Carattere del primo testo dentro un elemento (scheda nativa o riga del pannello), da UI Automation
function Get-TextFont($Element) {
    if (-not $Element) { return $null }
    $text = $Element.FindFirst($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::Text)))
    if (-not $text) { return $null }
    try { $range = $text.GetCurrentPattern([Windows.Automation.TextPattern]::Pattern).DocumentRange } catch { return $null }
    [pscustomobject]@{
        Size   = $range.GetAttributeValue([Windows.Automation.TextPattern]::FontSizeAttribute)
        Name   = $range.GetAttributeValue([Windows.Automation.TextPattern]::FontNameAttribute)
        Weight = $range.GetAttributeValue([Windows.Automation.TextPattern]::FontWeightAttribute)
        Color  = $range.GetAttributeValue([Windows.Automation.TextPattern]::ForegroundColorAttribute)
    }
}
# Colore "pieno" del testo in un rettangolo dello schermo: il pixel più chiaro (tema scuro) o più scuro
# (tema chiaro), cioè il centro dei tratti, dove il colore del testo non è sfumato col fondo
function Get-TextColor([int]$X, [int]$Y, [int]$W, [int]$H, [bool]$Dark = $true) {
    $bmp = New-Object Drawing.Bitmap $W, $H
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($X, $Y, 0, 0, $bmp.Size)
    $best = $null; $bestL = if ($Dark) { -1 } else { 999 }
    for ($i = 0; $i -lt $W; $i++) {
        for ($j = 0; $j -lt $H; $j++) {
            $c = $bmp.GetPixel($i, $j); $l = 0.299 * $c.R + 0.587 * $c.G + 0.114 * $c.B
            if (($Dark -and $l -gt $bestL) -or (-not $Dark -and $l -lt $bestL)) { $bestL = $l; $best = $c }
        }
    }
    $g.Dispose(); $bmp.Dispose()
    $best
}
# ...del titolo di una riga del pannello (esclusi stanghetta e X) o di una scheda orizzontale (esclusa la X)
function Get-RowTextColor([int64]$Hwnd, [string]$Prefix, [bool]$Dark = $true) {
    $r = (Find-PanelRow $Hwnd $Prefix).Current.BoundingRectangle
    Get-TextColor ([int]($r.X + $r.Width * 0.06)) ([int]($r.Y + $r.Height * 0.25)) ([int]($r.Width * 0.6)) ([int]($r.Height * 0.5)) $Dark
}
function Get-NativeTabTextColor([int64]$Hwnd, [string]$Prefix, [bool]$Dark = $true) {
    $r = (Find-NativeTab $Hwnd $Prefix).Current.BoundingRectangle
    Get-TextColor ([int]($r.X + $r.Width * 0.08)) ([int]($r.Y + $r.Height * 0.25)) ([int]($r.Width * 0.5)) ([int]($r.Height * 0.5)) $Dark
}

# Rettangoli e finestre figlie (coordinate fisiche dello schermo)
function Get-WinRect([int64]$Hwnd) { $r = New-Object NpxKit+RECT; [NpxKit]::GetWindowRect([IntPtr]$Hwnd, [ref]$r) | Out-Null; $r }
function Get-ChildWindows([int64]$Hwnd) {
    $out = @()
    for ($c = [NpxKit]::GetWindow([IntPtr]$Hwnd, 5); $c -ne [IntPtr]::Zero; $c = [NpxKit]::GetWindow($c, 2)) {
        $sb = New-Object System.Text.StringBuilder 256; [NpxKit]::GetClassName($c, $sb, 256) | Out-Null
        $r = Get-WinRect $c
        $out += [pscustomobject]@{ Hwnd = [int64]$c; Class = $sb.ToString(); Visible = [NpxKit]::IsWindowVisible($c); X = $r.Left; Y = $r.Top; W = $r.Right - $r.Left; H = $r.Bottom - $r.Top }
    }
    $out
}
# Isola del pannello (DesktopChildSiteBridge alta e larga almeno 40 px: le barre di scorrimento sono
# isole alte ma strette, 18 px; il pannello compresso è largo 56 DIP) ed editor visibile
function Get-PanelRect([int64]$Hwnd) {
    $wr = Get-WinRect $Hwnd
    Get-ChildWindows $Hwnd | Where-Object { $_.Visible -and $_.Class -like '*DesktopChildSiteBridge' -and $_.X -ge $wr.Left -and $_.H -gt 200 -and $_.W -ge 40 } |
        Select-Object -First 1
}
function Get-EditorRect([int64]$Hwnd) { Get-ChildWindows $Hwnd | Where-Object { $_.Visible -and $_.Class -eq 'NotepadTextBox' } | Select-Object -First 1 }
# Barra dei menu (isola larga e bassa che finisce dove comincia l'editor)
function Get-MenuBarRect([int64]$Hwnd) {
    $er = Get-EditorRect $Hwnd
    if (-not $er) { return $null }
    Get-ChildWindows $Hwnd | Where-Object { $_.Visible -and $_.Class -like '*DesktopChildSiteBridge' -and [math]::Abs(($_.Y + $_.H) - $er.Y) -le 2 -and $_.H -lt 120 -and $_.W -gt 200 } |
        Select-Object -First 1
}
# Geometria della finestra e del mouse vero: origine dell'area client, finestra sotto un punto dello schermo (rispetta le
# regioni, come il sistema), risposta a WM_NCHITTEST (1 client, 2 barra del titolo), ingrandita
if (-not ('NpxGeo' -as [type])) {
    Add-Type -Namespace '' -Name NpxGeo -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
[DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
[DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
[DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr rc, IntPtr rgn, uint flags);
'@
}
function Get-ClientOrigin([int64]$Hwnd) { $p = New-Object NpxGeo+POINT; [void][NpxGeo]::ClientToScreen([IntPtr]$Hwnd, [ref]$p); $p }
# Ridimensionamento della SOLA finestra di prova (il Blocco note ricalcola il suo layout)
if (-not ('NpxPos' -as [type])) {
    Add-Type -Namespace '' -Name NpxPos -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
'@
}
# La finestra principale del Blocco note (senza WS_CLIPCHILDREN) ridipinge il proprio sfondo, ma non le figlie: come
# quando ridipinge la riga del titolo dopo un cambio di layout. Il suo sfondo copre l'editor, che poi deve ridisegnarsi.
function Invoke-MainRepaintOnly([int64]$Hwnd) {
    [void][NpxGeo]::RedrawWindow([IntPtr]$Hwnd, [IntPtr]::Zero, [IntPtr]::Zero, 0x0001 -bor 0x0004 -bor 0x0040 -bor 0x0100) # INVALIDATE|ERASE|NOCHILDREN|UPDATENOW
}
# Colori in cima all'editor, 3 px sotto il bordo: a sinistra (sotto la riga del titolo originale) e a destra (sotto i
# pulsanti della finestra, fuori dalla riga del titolo), più uno 30 px sotto. Lontani dal testo (le prove hanno righe corte).
# Colore dell'editor in cima (3 px sotto il bordo) e a metà altezza, lontano dal testo. Con la riga del titolo nascosta
# la cima dell'editor era dentro la zona vetro (Mica) del Blocco note e lì era più chiara (#474747 invece di #272727).
function Get-EditorBandColors([int64]$Hwnd) {
    $er = Get-EditorRect $Hwnd
    [pscustomobject]@{
        Top    = Get-ScreenColor ($er.X + 200) ($er.Y + 3)
        Middle = Get-ScreenColor ($er.X + 200) ($er.Y + [int]($er.H / 2))
    }
}
function Get-EditorTopColors([int64]$Hwnd) {
    $er = Get-EditorRect $Hwnd
    [pscustomobject]@{
        Left  = Get-ScreenColor ($er.X + 200) ($er.Y + 3)
        Right = Get-ScreenColor ($er.X + $er.W - 120) ($er.Y + 3)
        Below = Get-ScreenColor ($er.X + 200) ($er.Y + 30)
    }
}
function Get-WindowAtScreen([int]$X, [int]$Y) { $p = New-Object NpxGeo+POINT; $p.X = $X; $p.Y = $Y; [int64][NpxGeo]::WindowFromPoint($p) }
# Pulsanti della finestra nostri (CaptionButtons della mod, con la riga del titolo nascosta): la loro finestra, o 0
function Get-CaptionButtons([int64]$Hwnd) { [int64][NpxKit]::FindWindowEx([IntPtr]$Hwnd, [IntPtr]::Zero, 'VTabsNotepad.CaptionButtons', $null) }
# Pulsanti della finestra di DWM: larghezza della loro zona (DWMWA_CAPTION_BUTTON_BOUNDS); 0 = nascosti (ci sono i nostri)
function Get-DwmButtonsWidth([int64]$Hwnd) { $r = New-Object NpxKit+RECT; [void][NpxKit]::DwmGetWindowAttribute([IntPtr]$Hwnd, 5, [ref]$r, 16); $r.Right - $r.Left }
function Get-NcHitScreen([int64]$Target, [int]$X, [int]$Y) {
    [int64][NpxKit]::SendMessage([IntPtr]$Target, 0x84, [IntPtr]::Zero, [IntPtr](([int64]($Y -band 0xFFFF) -shl 16) -bor ($X -band 0xFFFF)))
}
# Editor visibile: contenitore (NotepadTextBox), superficie di testo (RichEditD2DPT) e di quanto questa
# sporge oltre il bordo destro del contenitore (deve essere 0: altrimenti il testo finisce fuori vista)
function Get-EditorSurface([int64]$Hwnd) {
    $container = Get-EditorRect $Hwnd
    if (-not $container) { return $null }
    $surface = Get-ChildWindows $container.Hwnd | Where-Object { $_.Visible -and $_.Class -eq 'RichEditD2DPT' } | Select-Object -First 1
    [pscustomobject]@{
        Container = $container
        Surface   = $surface
        Overhang  = $(if ($surface) { ($surface.X + $surface.W) - ($container.X + $container.W) } else { 0 })
    }
}
# Attiva "A capo automatico" nella scheda visibile (nel Blocco note vale per scheda) e dice se il testo
# ora va a capo: la voce del menu si commuta e si verifica l'effetto (più righe visive), non lo stato
# dichiarato dal menu, che non è affidabile.
function Enable-WordWrap([int64]$Hwnd) {
    for ($i = 0; $i -lt 2; $i++) {
        if (@(Get-EditorTextRects $Hwnd).Count -gt 1) { return $true }
        $view = Find-Control $Hwnd (UiName 'View' 'Visualizza') $CT::MenuItem
        $view.GetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
        Start-Sleep -Milliseconds 600
        $item = Get-MenuItems | Where-Object { $_.Current.Name -eq (UiName 'Word wrap' 'A capo automatico') } | Select-Object -First 1
        if (-not $item) { $view.GetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern).Collapse(); return $false }
        Invoke-Any $item
        Start-Sleep -Milliseconds 1200
    }
    @(Get-EditorTextRects $Hwnd).Count -gt 1
}
# Rettangoli delle righe visive del documento nell'editor visibile (UI Automation, coordinate schermo)
function Get-EditorTextRects([int64]$Hwnd) {
    $editor = Get-EditorSurface $Hwnd
    if (-not $editor -or -not $editor.Surface) { return @() }
    $text = (Win $editor.Surface.Hwnd).GetCurrentPattern([Windows.Automation.TextPattern]::Pattern)
    @($text.DocumentRange.GetBoundingRectangles())
}
# Pulsante "Torna su": la sua isola (figlia della finestra con la proprietà VTabsNotepad.Overlay) e il
# pulsante via UI Automation
function Get-BackToTopIsland([int64]$Hwnd) {
    Get-ChildWindows $Hwnd | Where-Object { [NpxKit]::GetProp([IntPtr]$_.Hwnd, 'VTabsNotepad.Overlay') -ne [IntPtr]::Zero } | Select-Object -First 1
}
function Get-BackToTopButton([int64]$Hwnd) {
    (Win $Hwnd).FindFirst($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::AutomationIdProperty, 'NpxBackToTop')))
}
# Pulsante "Torna su" visibile: la sua isola resta mostrata ("parcheggiata") anche a pulsante nascosto, ma allora è
# trasparente al mouse (WS_EX_TRANSPARENT): visibile = isola mostrata e non trasparente.
if (-not ('NpxStyle' -as [type])) {
    Add-Type -Namespace '' -Name NpxStyle -MemberDefinition '[DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int index);'
}
function Test-IslandClickThrough([int64]$Island) { ([int64][NpxStyle]::GetWindowLongPtr([IntPtr]$Island, -20) -band 0x20) -ne 0 } # GWL_EXSTYLE, WS_EX_TRANSPARENT
function Test-BackToTopShown([int64]$Hwnd) { $i = Get-BackToTopIsland $Hwnd; [bool]($i -and $i.Visible -and -not (Test-IslandClickThrough $i.Hwnd)) }
# Barra di ricerca (Ctrl+F): il Blocco note la mette in un'isola XAML sospesa in cima all'editor (centrata, alta poco), e
# impagina il suo contenuto per la misura che le dà. Stato: isola, editor, controlli (pulsanti e caselle visibili) e quelli
# che escono dall'isola (tagliati), distanza fra il centro dell'isola e quello dell'editor. Nomi dei controlli soltanto:
# la casella contiene la ricerca dell'utente.
function Get-FindBarIsland([int64]$Hwnd) {
    $editor = Get-EditorRect $Hwnd
    if (-not $editor) { return $null }
    $panel = Get-PanelRect $Hwnd
    Get-ChildWindows $Hwnd | Where-Object { $_.Visible -and $_.Class -like '*DesktopChildSiteBridge' -and [math]::Abs($_.Y - $editor.Y) -le 4 -and $_.H -lt 400 -and $_.W -gt 150 -and (-not $panel -or $_.Hwnd -ne $panel.Hwnd) } |
        Sort-Object { $_.W } -Descending | Select-Object -First 1
}
function Get-FindBarState([int64]$Hwnd) {
    $island = Get-FindBarIsland $Hwnd
    if (-not $island) { return $null }
    $editor = Get-EditorRect $Hwnd
    $controls = @($AE::FromHandle([IntPtr]$island.Hwnd).FindAll($TS::Descendants, [Windows.Automation.Condition]::TrueCondition) |
        Where-Object { $_.Current.ControlType.ProgrammaticName -match 'Button|Edit' -and $_.Current.BoundingRectangle.Width -gt 0 -and -not $_.Current.IsOffscreen })
    $outside = @($controls | Where-Object { $r = $_.Current.BoundingRectangle; $r.Left -lt $island.X - 1 -or $r.Right -gt $island.X + $island.W + 1 -or $r.Top -lt $island.Y - 1 -or $r.Bottom -gt $island.Y + $island.H + 1 } |
        ForEach-Object { if ($_.Current.ControlType.ProgrammaticName -match 'Edit') { '(casella)' } else { $_.Current.Name } })
    [pscustomobject]@{ Island = $island; Editor = $editor; Controls = $controls.Count; Outside = $outside; CenterDelta = [int](($island.X + $island.W / 2) - ($editor.X + $editor.W / 2)) }
}
function Format-FindBar($State) {
    if (-not $State) { return 'barra di ricerca non trovata' }
    "isola [{0},{1} {2}x{3}], editor [{4},{5} {6}x{7}], centro - centro editor {8}; {9} controlli, fuori dall'isola: {10}" -f $State.Island.X, $State.Island.Y, $State.Island.W, $State.Island.H, $State.Editor.X, $State.Editor.Y,
        $State.Editor.W, $State.Editor.H, $State.CenterDelta, $State.Controls, $(if ($State.Outside.Count) { $State.Outside -join ', ' } else { 'nessuno' })
}
function Invoke-FindBarButton([int64]$Hwnd, [string]$NameLike) {
    $island = Get-FindBarIsland $Hwnd
    if (-not $island) { return $false }
    $b = $AE::FromHandle([IntPtr]$island.Hwnd).FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::Button))) |
        Where-Object { $_.Current.Name -like $NameLike -or $_.Current.AutomationId -eq $NameLike } | Select-Object -First 1
    if (-not $b) { return $false }
    Invoke-Element $b
    $true
}
# Colori dell'editor (immagine della finestra con PrintWindow, anche se coperta; thread per-monitor: pixel fisici): 6 px
# sotto la cima a 30 px dal bordo sinistro, e a metà altezza. La cima dell'editor salito nella riga del titolo diventava nera
# quando un'isola sospesa sopra di lui stava più in alto nell'ordine Z (l'editor non disegna sotto le sorelle sopra di lui).
if (-not ('NpxPrint' -as [type])) {
    Add-Type -Namespace '' -Name NpxPrint -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
[DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
'@
}
# (Nome diverso da Get-EditorTopColors, che legge lo schermo: con lo stesso nome questa la copriva e i controlli sulla
# fascia in cima all'editor trovavano campi vuoti.)
function Get-EditorPrintColors([int64]$Hwnd) {
    $old = [NpxPrint]::SetThreadDpiAwarenessContext([IntPtr](-4))
    try {
        $editor = Get-EditorRect $Hwnd
        $w = New-Object NpxPrint+RECT; [void][NpxPrint]::GetWindowRect([IntPtr]$Hwnd, [ref]$w)
        $e = New-Object NpxPrint+RECT; [void][NpxPrint]::GetWindowRect([IntPtr]$editor.Hwnd, [ref]$e)
        $bmp = New-Object Drawing.Bitmap ($w.Right - $w.Left), ($w.Bottom - $w.Top)
        $g = [Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc(); [void][NpxPrint]::PrintWindow([IntPtr]$Hwnd, $dc, 2); $g.ReleaseHdc($dc)
        $top = $bmp.GetPixel($e.Left - $w.Left + 30, $e.Top - $w.Top + 6)
        $middle = $bmp.GetPixel($e.Left - $w.Left + 30, [int](($e.Top + $e.Bottom) / 2) - $w.Top)
        $g.Dispose(); $bmp.Dispose()
        [pscustomobject]@{ Top = $top; Middle = $middle }
    }
    finally { [void][NpxPrint]::SetThreadDpiAwarenessContext($old) }
}
# Barra di scorrimento verticale dell'editor: la sua isola (stretta e alta, sul bordo destro dell'editor) e il ScrollViewer
# del Blocco note dentro (UI Automation), in px dello schermo. Top/Bottom $null se la barra non c'è (documento corto).
function Get-VScrollBar([int64]$Hwnd) {
    $er = Get-EditorRect $Hwnd
    if (-not $er) { return $null }
    $island = Get-ChildWindows $Hwnd | Where-Object {
        $_.Visible -and $_.Class -like '*DesktopChildSiteBridge' -and $_.W -le 40 -and $_.H -gt 3 * $_.W -and
        ($_.X + $_.W) -le ($er.X + $er.W + 2) -and ($_.X + $_.W) -ge ($er.X + $er.W - 40)
    } | Select-Object -First 1
    if (-not $island) { return $null }
    $sv = $AE::FromHandle([IntPtr]$island.Hwnd).FindFirst($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ClassNameProperty, 'ScrollViewer')))
    $b = if ($sv) { $sv.Current.BoundingRectangle } else { $null }
    $shown = $b -and -not $b.IsEmpty
    [pscustomobject]@{
        Island = $island
        Editor = $er
        Top    = $(if ($shown) { [int][math]::Round($b.Y) } else { $null })
        Bottom = $(if ($shown) { [int][math]::Round($b.Y + $b.Height) } else { $null })
    }
}
# Barra di scorrimento orizzontale dell'editor: la sua isola (larga e bassa, sul fondo dell'editor), se visibile
function Get-HScrollIsland([int64]$Hwnd) {
    $er = Get-EditorRect $Hwnd
    if (-not $er) { return $null }
    Get-ChildWindows $Hwnd | Where-Object {
        $_.Visible -and $_.Class -like '*DesktopChildSiteBridge' -and $_.H -le 30 -and $_.W -gt 200 -and [math]::Abs(($_.Y + $_.H) - ($er.Y + $er.H)) -le 6
    } | Select-Object -First 1
}
# ScrollViewer del Blocco note di una delle due barre (UI Automation: ScrollPattern per percentuali e trascinamento)
function Get-ScrollBarViewer([int64]$Hwnd, [switch]$Horizontal) {
    $island = if ($Horizontal) { Get-HScrollIsland $Hwnd } else { $v = Get-VScrollBar $Hwnd; if ($v) { $v.Island } else { $null } }
    if (-not $island) { return $null }
    $AE::FromHandle([IntPtr]$island.Hwnd).FindFirst($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ClassNameProperty, 'ScrollViewer')))
}
# Posizione del testo nell'editor visibile (SCROLLINFO della superficie di testo): Pos da 0 a End
if (-not ('NpxScroll' -as [type])) {
    Add-Type -Namespace '' -Name NpxScroll -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct SCROLLINFO { public uint cbSize, fMask; public int nMin, nMax; public uint nPage; public int nPos, nTrackPos; }
[DllImport("user32.dll")] public static extern bool GetScrollInfo(IntPtr h, int bar, ref SCROLLINFO si);
'@
}
function Get-EditorScroll([int64]$Hwnd, [switch]$Horizontal) {
    $editor = Get-EditorSurface $Hwnd
    if (-not $editor -or -not $editor.Surface) { return $null }
    $si = New-Object NpxScroll+SCROLLINFO; $si.cbSize = 28; $si.fMask = 0x17 # RANGE | PAGE | POS
    if (-not [NpxScroll]::GetScrollInfo([IntPtr]$editor.Surface.Hwnd, $(if ($Horizontal) { 0 } else { 1 }), [ref]$si)) { return $null }
    [pscustomobject]@{ Pos = $si.nPos; End = $si.nMax - [int]$si.nPage + 1; Surface = $editor.Surface.Hwnd }
}
# Posizione interna della RichEdit (EM_GETSCROLLPOS, pixel del documento). A differenza di SCROLLINFO (Windows tiene nPos
# dentro la corsa della barra) mostra anche un testo scorso oltre la fine. Il puntatore del messaggio deve stare nel processo
# del Blocco note: New-RichScrollProbe vi alloca una volta un piccolo buffer, Get-RichScrollPos lo rilegge dopo ogni messaggio.
if (-not ('NpxRemote' -as [type])) {
    Add-Type -Namespace '' -Name NpxRemote -MemberDefinition @'
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
[DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr VirtualAllocEx(IntPtr p, IntPtr addr, IntPtr size, uint type, uint protect);
[DllImport("kernel32.dll", SetLastError = true)] public static extern bool VirtualFreeEx(IntPtr p, IntPtr addr, IntPtr size, uint type);
[DllImport("kernel32.dll", SetLastError = true)] public static extern bool ReadProcessMemory(IntPtr p, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);
[DllImport("kernel32.dll", SetLastError = true)] public static extern bool WriteProcessMemory(IntPtr p, IntPtr addr, byte[] buf, IntPtr size, out IntPtr written);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr SendMessageTimeoutW(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
'@
}
function New-RichScrollProbe([int64]$Surface) {
    $procId = [uint32]0
    [void][NpxRemote]::GetWindowThreadProcessId([IntPtr]$Surface, [ref]$procId)
    $proc = [NpxRemote]::OpenProcess(0x0438, $false, $procId) # VM_OPERATION | VM_READ | VM_WRITE | QUERY_INFORMATION
    if ($proc -eq [IntPtr]::Zero) { throw 'OpenProcess sul Blocco note non riuscito' }
    $buffer = [NpxRemote]::VirtualAllocEx($proc, [IntPtr]::Zero, [IntPtr]4096, 0x3000, 0x04)
    if ($buffer -eq [IntPtr]::Zero) { [void][NpxRemote]::CloseHandle($proc); throw 'VirtualAllocEx non riuscito' }
    [pscustomobject]@{ Surface = [IntPtr]$Surface; Process = $proc; Buffer = $buffer; Bytes = (New-Object byte[] 8) }
}
function Get-RichScrollPos($Probe) {
    $result = [IntPtr]::Zero
    if ([NpxRemote]::SendMessageTimeoutW($Probe.Surface, 0x04DD, [IntPtr]::Zero, $Probe.Buffer, 0x0002, 300, [ref]$result) -eq [IntPtr]::Zero) { return $null }
    $read = [IntPtr]::Zero
    if (-not [NpxRemote]::ReadProcessMemory($Probe.Process, $Probe.Buffer, $Probe.Bytes, [IntPtr]8, [ref]$read)) { return $null }
    [pscustomobject]@{ X = [BitConverter]::ToInt32($Probe.Bytes, 0); Y = [BitConverter]::ToInt32($Probe.Bytes, 4) }
}
# EM_SETSCROLLPOS con il punto scritto nel buffer della sonda (stesso processo del Blocco note)
function Set-RichScrollPos($Probe, [int]$X, [int]$Y) {
    $bytes = [BitConverter]::GetBytes($X) + [BitConverter]::GetBytes($Y)
    $written = [IntPtr]::Zero
    if (-not [NpxRemote]::WriteProcessMemory($Probe.Process, [IntPtr]($Probe.Buffer.ToInt64() + 64), [byte[]]$bytes, [IntPtr]8, [ref]$written)) { return $false }
    $result = [IntPtr]::Zero
    [NpxRemote]::SendMessageTimeoutW($Probe.Surface, 0x04DE, [IntPtr]::Zero, [IntPtr]($Probe.Buffer.ToInt64() + 64), 0x0002, 300, [ref]$result) -ne [IntPtr]::Zero
}
function Remove-RichScrollProbe($Probe) {
    if (-not $Probe) { return }
    [void][NpxRemote]::VirtualFreeEx($Probe.Process, $Probe.Buffer, [IntPtr]::Zero, 0x8000)
    [void][NpxRemote]::CloseHandle($Probe.Process)
}
# Trascinamento simulato del cursore di una barra (UIA ScrollPattern) a piccoli passi dalla posizione attuale, come la mano:
# un salto unico lungo si ferma a metà (il Blocco note lo frena con la sincronizzazione barra-testo) e uno fatto subito dopo
# uno scorrimento del testo viene ignorato
function Invoke-ScrollDrag($Pattern, [double]$To, [switch]$Horizontal, [int]$Steps = 20, [int]$DelayMs = 60) {
    $from = if ($Horizontal) { $Pattern.Current.HorizontalScrollPercent } else { $Pattern.Current.VerticalScrollPercent }
    for ($i = 1; $i -le $Steps; $i++) {
        $p = $from + ($To - $from) * $i / $Steps
        if ($Horizontal) { $Pattern.SetScrollPercent($p, -1) } else { $Pattern.SetScrollPercent(-1, $p) }
        Start-Sleep -Milliseconds $DelayMs
    }
}
# Barra di scorrimento verticale sotto il mouse vero (solo finestra di prova): è larga ~200 ms e ~900 ms dopo che il cursore
# ci entra? Larga = si vedono le due frecce, in cima e in fondo alla striscia (la barra sottile non le ha; il suo cursore non
# arriva in quelle fasce). Prima il cursore resta fuori 3 s; alla fine esce di nuovo. Il modello di WinUI la allarga solo dopo
# 0,4 s (ScrollBarHover toglie il ritardo).
function Measure-VScrollHover([int64]$Hwnd) {
    $v = Get-VScrollBar $Hwnd
    if (-not $v) { return $null }
    $island = $v.Island
    $wr = Get-WinRect $Hwnd
    $W = $wr.Right - $wr.Left; $H = $wr.Bottom - $wr.Top
    $ix = $island.X - $wr.Left; $iy = $island.Y - $wr.Top
    $cx = $island.X + [int]($island.W / 2); $cy = $island.Y + [int]($island.H * 0.6)
    $outX = $island.X - 160
    Assert-OnWindow $Hwnd $cx $cy
    Assert-OnWindow $Hwnd $outX $cy
    $capture = {
        $bmp = New-Object Drawing.Bitmap $W, $H
        $g = [Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc(); [void][NpxKit]::PrintWindow([IntPtr]$Hwnd, $dc, 2); $g.ReleaseHdc($dc); $g.Dispose()
        $bmp
    }
    $arrows = {
        param($bmp)
        $bg = $bmp.GetPixel($ix - 4, $iy + 6)
        $band = {
            param([int]$Top)
            $n = 0
            for ($y = $Top; $y -lt $Top + 13; $y++) {
                for ($x = $ix + 3; $x -lt $ix + $island.W - 3; $x++) { $c = $bmp.GetPixel($x, $y); if ([math]::Abs($c.R - $bg.R) + [math]::Abs($c.G - $bg.G) + [math]::Abs($c.B - $bg.B) -gt 60) { $n++ } }
            }
            $n
        }
        $top = & $band ($iy + 2); $bottom = & $band ($iy + $island.H - 15)
        $bmp.Dispose()
        [pscustomobject]@{ Top = $top; Bottom = $bottom; Shown = $top -ge 6 -and $bottom -ge 6 }
    }
    Move-To $outX $cy 12
    Start-Sleep -Milliseconds 3000
    foreach ($k in 1..4) { Move-To ([int]($outX + ($cx - $outX) * $k / 4)) $cy; Start-Sleep -Milliseconds 8 }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt 200) { Start-Sleep -Milliseconds 2 }
    $earlyBmp = & $capture; $earlyMs = $sw.ElapsedMilliseconds
    while ($sw.ElapsedMilliseconds -lt 900) { Start-Sleep -Milliseconds 5 }
    $lateBmp = & $capture
    foreach ($k in 1..4) { Move-To ([int]($cx + ($outX - $cx) * $k / 4)) $cy; Start-Sleep -Milliseconds 8 }
    $early = & $arrows $earlyBmp; $late = & $arrows $lateBmp
    [pscustomobject]@{ EarlyMs = $earlyMs; Early = $early.Shown; Late = $late.Shown; Detail = ("frecce a {0} ms: {1}/{2} px, a 900 ms: {3}/{4} px" -f $earlyMs, $early.Top, $early.Bottom, $late.Top, $late.Bottom) }
}
# Uno scatto di rotella sul testo dell'editor visibile (WM_MOUSEWHEEL messo in coda come quelli veri: passa dagli hook di
# WheelScroll) e posizione del testo campionata per 600 ms: quanti cambi (1 = istantaneo, molti = scorrimento fluido).
# -Keys: tasti premuti nel wParam (0x0008 = Ctrl).
function Measure-WheelSteps([int64]$Hwnd, [int]$Delta = -120, [int]$Keys = 0) {
    $s0 = Get-EditorScroll $Hwnd
    if (-not $s0) { return $null }
    $surface = [IntPtr]$s0.Surface
    $r = Get-WinRect $surface
    $x = [int](($r.Left + $r.Right) / 2); $y = [int](($r.Top + $r.Bottom) / 2)
    $lParam = [IntPtr]((($y -band 0xFFFF) -shl 16) -bor ($x -band 0xFFFF))
    $wParam = [IntPtr]((($Delta -band 0xFFFF) -shl 16) -bor ($Keys -band 0xFFFF))
    Start-Sleep -Milliseconds 400
    $p0 = (Get-EditorScroll $Hwnd).Pos
    $changes = 0; $last = $p0
    $sw = [Diagnostics.Stopwatch]::StartNew()
    [void][NpxKit]::PostMessage($surface, 0x020A, $wParam, $lParam)
    while ($sw.ElapsedMilliseconds -lt 600) {
        $p = (Get-EditorScroll $Hwnd).Pos
        if ($p -ne $last) { $changes++; $last = $p }
        Start-Sleep -Milliseconds 4
    }
    [pscustomobject]@{ From = $p0; To = $last; Changes = $changes; End = (Get-EditorScroll $Hwnd).End }
}
# Accende/spegne "A capo automatico" (menu Visualizza; nel Blocco note vale per scheda)
function Switch-WordWrap([int64]$Hwnd) {
    $view = Find-Control $Hwnd (UiName 'View' 'Visualizza') $CT::MenuItem
    $view.GetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
    Start-Sleep -Milliseconds 600
    $item = Get-MenuItems | Where-Object { $_.Current.Name -eq (UiName 'Word wrap' 'A capo automatico') } | Select-Object -First 1
    if (-not $item) { $view.GetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern).Collapse(); return $false }
    Invoke-Any $item
    Start-Sleep -Milliseconds 1200
    $true
}
# Prime lettere della prima riga visibile dell'editor (i file di prova iniziano con il numero di riga)
function Get-FirstVisibleText([int64]$Hwnd, [int]$Length = 4) {
    $editor = Get-EditorSurface $Hwnd
    if (-not $editor -or -not $editor.Surface) { return '' }
    $text = (Win $editor.Surface.Hwnd).GetCurrentPattern([Windows.Automation.TextPattern]::Pattern)
    (@($text.GetVisibleRanges()) | Select-Object -First 1).GetText($Length).Trim()
}
# Scorre l'editor di qualche pagina in giù come la tastiera (messaggi all'editor, nessun input reale)
function Send-EditorPageDown([int64]$Hwnd, [int]$Pages = 3) {
    $editor = Get-EditorSurface $Hwnd
    1..$Pages | ForEach-Object { [NpxKit]::PostMessage([IntPtr]$editor.Surface.Hwnd, 0x0115, [IntPtr]3, [IntPtr]::Zero) | Out-Null; Start-Sleep -Milliseconds 120 } # WM_VSCROLL SB_PAGEDOWN
}
# Finestra con il focus della tastiera sul thread dell'editor visibile
function Test-EditorHasFocus([int64]$Hwnd) {
    $editor = Get-EditorSurface $Hwnd
    if (-not $editor -or -not $editor.Surface) { return $false }
    $procId = 0; $tid = [NpxKit]::GetWindowThreadProcessId([IntPtr]$editor.Surface.Hwnd, [ref]$procId)
    $info = New-Object NpxKit+GUITHREADINFO; $info.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($info)
    [NpxKit]::GetGUIThreadInfo($tid, [ref]$info) -and [int64]$info.hwndFocus -eq $editor.Surface.Hwnd
}
# Riapri la scheda chiusa (Ctrl+Maiusc+T): chiusura di una scheda dal pannello, comando della
# scorciatoia e verifica che la combinazione sia riservata, tutto senza premere tasti davvero
# -NoWait: non aspetta la conferma della chiusura (per provare la pressione immediata)
function Close-PanelTab([int64]$Hwnd, [string]$Prefix, [switch]$NoWait) {
    $name = Safe-Name $Prefix
    Select-Element (Find-PanelRow $Hwnd $Prefix)
    $deadline = (Get-Date).AddSeconds(4)
    while ((Get-SelectedPanel $Hwnd) -ne $name -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
    Invoke-Element (Find-ByName (Find-PanelRow $Hwnd $Prefix) (UiName 'Close tab' 'Chiudi scheda')) # X della riga attiva
    $deadline = (Get-Date).AddSeconds(6)
    while ((Get-PanelOrder $Hwnd) -contains $name -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
    if (-not $NoWait) { Start-Sleep -Milliseconds 1200 } # la mod conferma la chiusura (non era un riordino)
}
# Il comando della scorciatoia. Senza -Cli si segnala direttamente l'evento che il launcher userebbe:
# è immediato (serve per provare la pressione subito dopo una chiusura) e non lascia in giro il
# processo notepad.exe di passaggio.
function Invoke-ReopenTab([switch]$Cli) {
    if ($Cli) { & (Join-Path $NpxRepo 'build\bin\VTabsNotepad.exe') --reopen-tab; Start-Sleep -Milliseconds 400; return }
    $p = Get-NpProcess
    $event = [Threading.EventWaitHandle]::OpenExisting("Local\VTabsNotepad.ReopenTab.$($p.Id)")
    try { $event.Set() | Out-Null } finally { $event.Dispose() }
    Start-Sleep -Milliseconds 150
}
# true se Ctrl+Maiusc+T è tenuta da un altro programma: se la tiene il Blocco note, noi non riusciamo a
# registrarla (MOD_CONTROL | MOD_SHIFT, 'T')
function Test-HotkeyHeld {
    $ok = [NpxKit]::RegisterHotKey([IntPtr]::Zero, 9911, 6, 0x54)
    if ($ok) { [NpxKit]::UnregisterHotKey([IntPtr]::Zero, 9911) | Out-Null }
    -not $ok
}
# La striscia schede nativa deve stare fuori dall'area visibile (a sinistra della finestra)
function Test-NativeStripHidden([int64]$Hwnd) {
    $wr = Get-WinRect $Hwnd
    $strip = Get-ChildWindows $Hwnd | Where-Object { $_.Visible -and $_.Class -like '*DesktopChildSiteBridge' -and $_.X + $_.W -lt $wr.Left }
    @($strip).Count -ge 1
}
# Finestre top-level visibili del processo (dialoghi e popup inclusi)
function Get-TopWindows([string]$Class = '') {
    $p = Get-NpProcess
    $out = @()
    for ($h = [NpxKit]::GetWindow([NpxKit]::GetDesktopWindow(), 5); $h -ne [IntPtr]::Zero; $h = [NpxKit]::GetWindow($h, 2)) {
        $procId = 0; [NpxKit]::GetWindowThreadProcessId($h, [ref]$procId) | Out-Null
        if (-not $p -or $procId -ne $p.Id -or -not [NpxKit]::IsWindowVisible($h)) { continue }
        $sb = New-Object System.Text.StringBuilder 256; [NpxKit]::GetClassName($h, $sb, 256) | Out-Null
        if ($Class -and $sb.ToString() -ne $Class) { continue }
        $r = Get-WinRect $h
        $out += [pscustomobject]@{ Hwnd = [int64]$h; Class = $sb.ToString(); X = $r.Left; Y = $r.Top; W = $r.Right - $r.Left; H = $r.Bottom - $r.Top }
    }
    $out
}
function Get-OpenPopups { @(Get-TopWindows | Where-Object { $_.Class -like '*PopupWindowSiteBridge' }) }

# ------------------------------------------------------------------------------------------------
# Menu del Blocco note, dialogo "Apri", menu contestuale
# ------------------------------------------------------------------------------------------------

function Get-MenuItems {
    $p = Get-NpProcess
    $c = New-Object Windows.Automation.AndCondition(
        (New-Object Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $p.Id)),
        (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::MenuItem)))
    @($AE::RootElement.FindAll($TS::Descendants, $c))
}
function Invoke-MenuItem([string]$Name) {
    $item = Get-MenuItems | Where-Object { $_.Current.Name -eq $Name } | Select-Object -First 1
    if (-not $item) { throw "Voce di menu '$Name' non trovata" }
    Invoke-Element $item
}
# Comando della barra dei menu (es. File > Nuova finestra)
function Invoke-MenuCommand([int64]$Hwnd, [string]$Menu, [string]$Command) {
    $c = New-Object Windows.Automation.AndCondition(
        (New-Object Windows.Automation.PropertyCondition($AE::NameProperty, $Menu)),
        (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::MenuItem)))
    $m = (Win $Hwnd).FindFirst($TS::Descendants, $c)
    if (-not $m) { throw "Menu $Menu non trovato" }
    $m.GetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
    # Il menu può metterci più di mezzo secondo (finestra appena riaperta da ridotta a icona, scala alta): si aspetta fino a 4 s
    $item = $null
    $deadline = (Get-Date).AddSeconds(4)
    do { Start-Sleep -Milliseconds 300; $item = Get-MenuItems | Where-Object { $_.Current.Name -eq $Command } | Select-Object -First 1 } while (-not $item -and (Get-Date) -lt $deadline)
    if (-not $item) { $m.GetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern).Collapse(); throw "Voce $Command non trovata" }
    Invoke-Element $item
}
# Apre un file in una finestra precisa: File > Apri, nome nel dialogo (WM_SETTEXT) e IDOK (WM_COMMAND)
function Open-FileInWindow([int64]$Hwnd, [string]$Path) {
    Invoke-MenuCommand $Hwnd 'File' (UiName 'Open' 'Apri')
    $deadline = (Get-Date).AddSeconds(15)
    do { Start-Sleep -Milliseconds 300; $dlg = Get-TopWindows '#32770' | Select-Object -First 1 } while (-not $dlg -and (Get-Date) -lt $deadline)
    if (-not $dlg) { throw 'Finestra Apri non comparsa' }
    Start-Sleep -Milliseconds 600
    $d = [IntPtr]$dlg.Hwnd
    $edit = [NpxKit]::FindWindowEx([NpxKit]::FindWindowEx([NpxKit]::FindWindowEx($d, [IntPtr]::Zero, 'ComboBoxEx32', $null), [IntPtr]::Zero, 'ComboBox', $null), [IntPtr]::Zero, 'Edit', $null)
    if ($edit -eq [IntPtr]::Zero) {
        [NpxKit]::SendMessage($d, 0x0111, [IntPtr]2, [IntPtr]::Zero) | Out-Null # IDCANCEL
        throw 'Casella del nome file non trovata'
    }
    [NpxKit]::SendMessageText($edit, 0x000C, [IntPtr]::Zero, $Path) | Out-Null           # WM_SETTEXT
    Start-Sleep -Milliseconds 200
    [NpxKit]::SendMessage($d, 0x0111, [IntPtr]1, [NpxKit]::GetDlgItem($d, 1)) | Out-Null  # WM_COMMAND IDOK
    $deadline = (Get-Date).AddSeconds(10)
    while (@(Get-TopWindows '#32770').Count -gt 0 -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
}
# Menu contestuale di una riga del pannello (o, con -Native, di una scheda della striscia nativa)
# senza mouse (IUIAutomationElement3::ShowContextMenu)
function Show-ContextMenu([int64]$Hwnd, [string]$RowPrefix, [switch]$Native) {
    if (-not (Test-Path $NpxUiaTool)) { throw "Manca $NpxUiaTool (compila con build.ps1)" }
    $out = if ($Native) { & $NpxUiaTool ('{0:X}' -f $Hwnd) $RowPrefix tab } else { & $NpxUiaTool ('{0:X}' -f $Hwnd) $RowPrefix }
    if ($out -notmatch '0x00000000') { Write-Warning ("strumento UIA: " + ($out -join ' ')) }
    Start-Sleep -Milliseconds 800
}

# ------------------------------------------------------------------------------------------------
# Sessione del Blocco note e finestra di prova
# ------------------------------------------------------------------------------------------------

function Backup-Session {
    $dir = Join-Path $NpxData ("backups\{0}-test" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item -LiteralPath (Join-Path $NpxLocalState 'TabState') -Destination $dir -Recurse
    Copy-Item -LiteralPath (Join-Path $NpxLocalState 'WindowState') -Destination $dir -Recurse
    $dir
}
function Get-TestState {
    $path = Join-Path $NpxTestDir 'state.json'
    if (Test-Path $path) { Get-Content $path -Raw | ConvertFrom-Json }
}
# Righe del log della mod per il processo attuale (-Since: solo quelle da quell'ora in poi, stesso giorno)
function Get-ModLog([int]$N = 20, [string]$Filter = '', [Nullable[datetime]]$Since = $null) {
    $path = Join-Path $NpxData 'logs\mod.log'
    if (-not (Test-Path $path)) { return @() }
    $read = { param($file) $fs = [IO.File]::Open($file, 'Open', 'Read', 'ReadWrite'); $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8); $text = $sr.ReadToEnd(); $sr.Close(); $text }
    $all = & $read $path
    # Con -Since conta anche il file ruotato: se il log ha superato la sua misura durante la prova (log dettagliato acceso),
    # le righe di prima della rotazione sono in mod.old.log
    $old = Join-Path $NpxData 'logs\mod.old.log'
    if ($null -ne $Since -and (Test-Path $old) -and (Get-Item -LiteralPath $old).LastWriteTime -ge [datetime]$Since) { $all = (& $read $old) + "`r`n" + $all }
    $p = Get-NpProcess
    $lines = $all -split "`r`n" | Where-Object { $_ -and (-not $p -or $_ -match "\[\s*$($p.Id):") -and $_ -notmatch '\+ (NotepadXamlUI|Microsoft)\.' }
    if ($Filter) { $lines = $lines | Where-Object { $_ -match $Filter } }
    if ($null -ne $Since) {
        # PowerShell consegna il Nullable già "spacchettato": è direttamente un DateTime
        $from = ([datetime]$Since).TimeOfDay
        $lines = $lines | Where-Object { $_ -match '^(\d\d:\d\d:\d\d\.\d+)' -and [TimeSpan]::Parse($Matches[1]) -ge $from }
    }
    @($lines | Select-Object -Last $N)
}
function Get-Ini { Get-Content (Join-Path $NpxData 'settings.ini') }
function Get-IniValue([string]$Key) { ((Get-Ini) | Where-Object { $_ -match "^$Key=" } | Select-Object -First 1) -replace "^$Key=", '' }
# Toglie una chiave da settings.ini (vale di nuovo il valore predefinito), mantenendo la codifica del file
function Remove-IniKey([string]$Key) {
    $path = Join-Path $NpxData 'settings.ini'
    $bytes = [IO.File]::ReadAllBytes($path)
    $unicode = $bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE
    $lines = @(Get-Content -LiteralPath $path)
    $kept = @($lines | Where-Object { $_ -notmatch "^\s*$Key\s*=" })
    if ($kept.Count -ne $lines.Count) {
        if ($unicode) { $kept | Set-Content -LiteralPath $path -Encoding Unicode } else { $kept | Set-Content -LiteralPath $path -Encoding Default }
    }
}
function Set-Opt([string]$Section, [string]$Key, [string]$Value) {
    & (Join-Path $NpxRepo 'tools\dev-test.ps1') -Set -Section $Section -Key $Key -Value $Value 6>$null | Out-Null
}
# Valore di una chiave in una sezione precisa (la stessa chiave, es. Enabled, sta in più sezioni);
# $null se manca
function Get-IniSectionValue([string]$Section, [string]$Key) {
    $current = ''
    foreach ($line in Get-Ini) {
        if ($line -match '^\s*\[(.+)\]\s*$') { $current = $Matches[1]; continue }
        if ($current -eq $Section -and $line -match "^\s*$Key\s*=(.*)$") { return $Matches[1].Trim() }
    }
    $null
}
# Scrive (o, con $null, toglie) una chiave in una sezione precisa
function Set-IniSectionValue([string]$Section, [string]$Key, [AllowNull()][string]$Value) {
    if (-not ('NpxIni' -as [type])) {
        # (un $null passato a un parametro string diventerebbe "": per togliere la chiave serve un puntatore nullo)
        Add-Type -Namespace '' -Name NpxIni -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)] public static extern bool WritePrivateProfileString(string s, string k, string v, string f);
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, EntryPoint = "WritePrivateProfileStringW")] public static extern bool DeleteKey(string s, string k, IntPtr v, string f);
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, EntryPoint = "WritePrivateProfileStringW")] public static extern bool DeleteSection(string s, IntPtr k, IntPtr v, string f);
'@
    }
    $file = Join-Path $NpxData 'settings.ini'
    if ($null -ne $Value -and $Value -ne '') { [NpxIni]::WritePrivateProfileString($Section, $Key, $Value, $file) | Out-Null; return }
    [NpxIni]::DeleteKey($Section, $Key, [IntPtr]::Zero, $file) | Out-Null
    # Sezione rimasta vuota: si toglie anche l'intestazione
    $inSection = $false; $keys = 0
    foreach ($line in Get-Ini) {
        if ($line -match '^\s*\[(.+)\]\s*$') { $inSection = $Matches[1] -eq $Section; continue }
        if ($inSection -and $line -match '=') { $keys++ }
    }
    if ($keys -eq 0) { [NpxIni]::DeleteSection($Section, [IntPtr]::Zero, [IntPtr]::Zero, $file) | Out-Null }
}

# Chiude la finestra di prova (solo se contiene esclusivamente schede di prova o vuote non modificate)
function Close-TestWindow([switch]$RemoveFiles) {
    $state = Get-TestState
    if ($state -and [NpxKit]::IsWindow([IntPtr]$state.test)) {
        $foreign = @(Get-NativeTabs $state.test | Where-Object { -not ($_.Current.Name.StartsWith($NpxTestPrefix) -or $_.Current.Name -match '^(Senza titolo\. Non modificato|Untitled\. Unmodified)') })
        if ($foreign.Count -gt 0) { throw 'La finestra di prova contiene schede che non sono di prova: non la chiudo.' }
        [NpxKit]::PostMessage([IntPtr]$state.test, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        $deadline = (Get-Date).AddSeconds(10)
        while ([NpxKit]::IsWindow([IntPtr]$state.test) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
        if ([NpxKit]::IsWindow([IntPtr]$state.test)) { throw 'La finestra di prova non si è chiusa.' }
    }
    if ($RemoveFiles) {
        Remove-Item -LiteralPath $NpxTestDir -Recurse -Force -ErrorAction SilentlyContinue
        Remove-TestReopenEntries
    }
}

# Cronologia di Ctrl+Maiusc+T su disco (closed-tabs.txt della mod): righe "posto<TAB>percorso". Le schede di prova chiuse vi
# restano come quelle dell'utente: si tolgono solo le righe di prova (cartella di prova), le altre non si leggono né si toccano.
function Get-ReopenHistoryCount([switch]$TestOnly) {
    $store = Join-Path $NpxData 'closed-tabs.txt'
    if (-not (Test-Path -LiteralPath $store)) { return 0 }
    $lines = @([IO.File]::ReadAllLines($store, [Text.Encoding]::UTF8) | Where-Object { $_ -and -not $_.StartsWith('#') })
    if ($TestOnly) { $lines = @($lines | Where-Object { $_ -match [regex]::Escape($NpxTestDir) }) }
    $lines.Count
}
# La cronologia è comune a tutte le finestre: un Ctrl+Maiusc+T di prova ripescherebbe una scheda chiusa dall'utente e la
# aprirebbe nella finestra di prova. Durante le prove quella dell'utente sta da parte (closed-tabs.user.txt); alla fine torna,
# con in coda le schede che l'utente ha chiuso nel frattempo (le righe che non sono di prova). Se una prova precedente si è
# interrotta, la copia da parte c'è già: non si sovrascrive, si unisce.
function Enter-ReopenIsolation {
    $store = Join-Path $NpxData 'closed-tabs.txt'
    $aside = Join-Path $NpxData 'closed-tabs.user.txt'
    $live = if (Test-Path -LiteralPath $store) { @([IO.File]::ReadAllLines($store, [Text.Encoding]::UTF8) | Where-Object { $_ -and -not $_.StartsWith('#') -and -not ($_ -match [regex]::Escape($NpxTestDir)) }) } else { @() }
    $old = if (Test-Path -LiteralPath $aside) { @([IO.File]::ReadAllLines($aside, [Text.Encoding]::UTF8) | Where-Object { $_ -and -not $_.StartsWith('#') }) } else { @() }
    $merged = @($old) + @($live)
    if ($merged.Count -gt 0) { [IO.File]::WriteAllText($aside, (($merged -join "`r`n") + "`r`n"), (New-Object Text.UTF8Encoding $false)) }
    Remove-Item -LiteralPath $store -Force -ErrorAction SilentlyContinue
}
function Exit-ReopenIsolation {
    $store = Join-Path $NpxData 'closed-tabs.txt'
    $aside = Join-Path $NpxData 'closed-tabs.user.txt'
    $old = if (Test-Path -LiteralPath $aside) { @([IO.File]::ReadAllLines($aside, [Text.Encoding]::UTF8) | Where-Object { $_ -and -not $_.StartsWith('#') }) } else { @() }
    $live = if (Test-Path -LiteralPath $store) { @([IO.File]::ReadAllLines($store, [Text.Encoding]::UTF8) | Where-Object { $_ -and -not $_.StartsWith('#') -and -not ($_ -match [regex]::Escape($NpxTestDir)) }) } else { @() }
    $merged = @(@($old) + @($live) | Select-Object -Last 25)
    if ($merged.Count -gt 0) { [IO.File]::WriteAllText($store, (($merged -join "`r`n") + "`r`n"), (New-Object Text.UTF8Encoding $false)) }
    else { Remove-Item -LiteralPath $store -Force -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $aside -Force -ErrorAction SilentlyContinue
}
function Remove-TestReopenEntries {
    $store = Join-Path $NpxData 'closed-tabs.txt'
    if (-not (Test-Path -LiteralPath $store)) { return }
    $all = [IO.File]::ReadAllLines($store, [Text.Encoding]::UTF8)
    $keep = @($all | Where-Object { -not ($_ -match [regex]::Escape($NpxTestDir)) })
    if ($keep.Count -eq $all.Count) { return }
    if (@($keep | Where-Object { $_ -and -not $_.StartsWith('#') }).Count -eq 0) { Remove-Item -LiteralPath $store -Force -ErrorAction SilentlyContinue }
    else { [IO.File]::WriteAllText($store, (($keep -join "`r`n") + "`r`n"), (New-Object Text.UTF8Encoding $false)) }
}

# Ricarica il Blocco note (per una nuova build della mod): backup, chiusura normale, riavvio, iniezione
function Restart-Notepad {
    Close-TestWindow
    $wins = Get-NpWindows
    if ($wins.Count -gt 1) { throw "Finestre del Blocco note aperte: $($wins.Count). Il riavvio automatico è previsto con una sola finestra." }
    if ($wins.Count -eq 1) {
        $backup = Backup-Session
        Write-Host "Backup della sessione: $backup"
        $p = Get-NpProcess
        [NpxKit]::PostMessage([IntPtr](Get-Hwnd $wins[0]), 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        $deadline = (Get-Date).AddSeconds(25)
        while ((Get-Process -Id $p.Id -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
        if (Get-Process -Id $p.Id -ErrorAction SilentlyContinue) { throw 'Il Blocco note non si è chiuso (forse chiede qualcosa): controlla la finestra.' }
    }
    Start-Process notepad.exe
    $deadline = (Get-Date).AddSeconds(30)
    do { Start-Sleep -Milliseconds 500; $w = Get-NpWindows } while ($w.Count -lt 1 -and (Get-Date) -lt $deadline)
    if ($w.Count -lt 1) { throw 'Il Blocco note non si è avviato.' }
    $p = Get-NpProcess
    if (-not (Get-Process VTabsNotepad -ErrorAction SilentlyContinue)) {
        & (Join-Path $NpxRepo 'build\bin\VTabsNotepad.exe') --inject-pid $p.Id
    }
    $deadline = (Get-Date).AddSeconds(30)
    do { Start-Sleep -Milliseconds 700 } while (-not (Get-ModLog 60 'schede verticali attive|pannello creato') -and (Get-Date) -lt $deadline)
    Write-Host ("Blocco note riavviato: PID {0}, {1} schede ripristinate." -f $p.Id, (Get-NativeTabs (Get-Hwnd $w[0])).Count)
}

# Crea la finestra di prova (File > Nuova finestra) con i file npx-test-1..N. -OnCreated riceve l'handle
# appena la finestra esiste, prima di aspettarne il pannello (es. per abilitarla in dev-windows.txt).
function New-TestWindow([int]$Files = 5, [scriptblock]$OnCreated = $null) {
    $script:NpxTestStart = Get-Date
    New-Item -ItemType Directory -Force $NpxTestDir | Out-Null
    1..$Files | ForEach-Object { Set-Content -LiteralPath (Join-Path $NpxTestDir "$NpxTestPrefix$_.txt") -Value "Prova $_" -Encoding UTF8 }
    $before = @(Get-NpWindows | ForEach-Object { Get-Hwnd $_ })
    if ($before.Count -lt 1) { throw 'Nessun Blocco note aperto.' }
    # Da ridotta a icona il menu della finestra non si apre: la si riapre per un attimo senza attivarla
    # e poi torna ridotta a icona (le sue schede non vengono toccate)
    $iconic = [NpxKit]::IsIconic([IntPtr]$before[0])
    if ($iconic) { [NpxKit]::ShowWindow([IntPtr]$before[0], 4) | Out-Null; Start-Sleep -Milliseconds 1500 } # SW_SHOWNOACTIVATE
    try {
        # Appena riaperta da ridotta a icona la finestra a volte non mostra ancora le voci del menu: secondo tentativo
        try { Invoke-MenuCommand $before[0] 'File' (UiName 'New window' 'Nuova finestra') }
        catch { Start-Sleep -Milliseconds 1200; Invoke-MenuCommand $before[0] 'File' (UiName 'New window' 'Nuova finestra') }
        $deadline = (Get-Date).AddSeconds(10)
        do { Start-Sleep -Milliseconds 400; $new = @(Get-NpWindows | Where-Object { $before -notcontains (Get-Hwnd $_) }) } while ($new.Count -lt 1 -and (Get-Date) -lt $deadline)
    }
    finally {
        if ($iconic) { [NpxKit]::ShowWindow([IntPtr]$before[0], 7) | Out-Null } # SW_SHOWMINNOACTIVE
    }
    if ($new.Count -lt 1) { throw 'Finestra di prova non creata.' }
    $test = Get-Hwnd $new[0]
    @{ test = $test } | ConvertTo-Json | Set-Content (Join-Path $NpxTestDir 'state.json')
    if ($OnCreated) { & $OnCreated $test }
    $deadline = (Get-Date).AddSeconds(10)
    while (-not (Get-PanelList $test) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    1..$Files | ForEach-Object { Open-FileInWindow $test (Join-Path $NpxTestDir "$NpxTestPrefix$_.txt"); Start-Sleep -Milliseconds 800 }
    $test
}

# Ordine delle schede salvato dal Blocco note (LocalState\WindowState) per la finestra di prova.
# Si considerano solo le finestre le cui schede sono TUTTE nate durante il test (TabState creati dopo
# New-TestWindow): i file di sessione delle schede dell'utente non vengono mai letti.
function Read-Shared([string]$Path) {
    try {
        $fs = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
        try { $ms = New-Object IO.MemoryStream; $fs.CopyTo($ms); return , $ms.ToArray() } finally { $fs.Close() }
    }
    catch { return $null }
}
function Get-SavedOrders([datetime]$Since = $script:NpxTestStart) {
    if (-not $Since) { throw 'Get-SavedOrders: manca l''inizio del test (usa New-TestWindow).' }
    $tabDir = Join-Path $NpxLocalState 'TabState'
    $files = Get-ChildItem $tabDir -Filter '*.bin'
    $guids = $files | ForEach-Object { ($_.Name -split '\.')[0] } | Sort-Object -Unique
    $ours = $files | Where-Object { $_.CreationTime -ge $Since.AddSeconds(-2) } | ForEach-Object { ($_.Name -split '\.')[0] } | Sort-Object -Unique
    foreach ($f in Get-ChildItem (Join-Path $NpxLocalState 'WindowState') -Filter '*.bin') {
        $bytes = Read-Shared $f.FullName
        if (-not $bytes) { continue }
        $hex = [BitConverter]::ToString($bytes)
        $found = @(foreach ($g in $guids) {
                $i = $hex.IndexOf([BitConverter]::ToString(([Guid]$g).ToByteArray()))
                if ($i -ge 0) { [pscustomobject]@{ Guid = $g; Pos = $i } }
            }) | Sort-Object Pos
        if ($found.Count -eq 0 -or @($found | Where-Object { $ours -notcontains $_.Guid }).Count -gt 0) { continue } # non è la finestra di prova
        $names = foreach ($t in $found) {
            $raw = Read-Shared (Join-Path $tabDir "$($t.Guid).bin")
            $text = if ($raw -and $raw.Length -gt 2) { [Text.Encoding]::Unicode.GetString($raw) + '|' + [Text.Encoding]::Unicode.GetString($raw, 1, $raw.Length - 1) } else { '' }
            if ($text -match 'npx-test-(\d+)') { "npx-test-$($Matches[1])" } else { 'Senza titolo' }
        }
        [pscustomobject]@{ File = $f.Name; Order = ($names -join ',') }
    }
}

# ------------------------------------------------------------------------------------------------
# Input reale (solo dopo un preavviso all'utente)
# ------------------------------------------------------------------------------------------------

$script:NpxLastCursor = $null
function Get-Cursor { $p = New-Object NpxKit+POINT; [NpxKit]::GetCursorPos([ref]$p) | Out-Null; $p }
function Reset-CursorWatch { $script:NpxLastCursor = $null }
# -Tolerance: spostamento esterno ammesso dall'ultimo movimento nostro (stretto durante un gesto,
# più largo all'inizio di un gesto nuovo, dopo un'attesa: piccole derive non sono un utente attivo)
function Move-To([int]$X, [int]$Y, [int]$Tolerance = 6) {
    if ($script:NpxLastCursor) {
        $c = Get-Cursor
        if ([math]::Abs($c.X - $script:NpxLastCursor.X) -gt $Tolerance -or [math]::Abs($c.Y - $script:NpxLastCursor.Y) -gt $Tolerance) {
            [NpxKit]::mouse_event(0x04, 0, 0, 0, [IntPtr]::Zero); [NpxKit]::mouse_event(0x10, 0, 0, 0, [IntPtr]::Zero)
            throw ("Mouse mosso dall'esterno (atteso {0},{1}, trovato {2},{3}): passo interrotto." -f $script:NpxLastCursor.X, $script:NpxLastCursor.Y, $c.X, $c.Y)
        }
    }
    $vx = [NpxKit]::GetSystemMetrics(76); $vy = [NpxKit]::GetSystemMetrics(77)
    $vw = [NpxKit]::GetSystemMetrics(78); $vh = [NpxKit]::GetSystemMetrics(79)
    $nx = [int][math]::Round(($X - $vx) * 65535.0 / ($vw - 1)); $ny = [int][math]::Round(($Y - $vy) * 65535.0 / ($vh - 1))
    [NpxKit]::mouse_event(0x0001 -bor 0x8000 -bor 0x4000, $nx, $ny, 0, [IntPtr]::Zero) # MOVE | ABSOLUTE | VIRTUALDESK
    $sw = [Diagnostics.Stopwatch]::StartNew()
    do { Start-Sleep -Milliseconds 2; $c = Get-Cursor } while (($c.X -ne $X -or $c.Y -ne $Y) -and $sw.ElapsedMilliseconds -lt 400)
    $script:NpxLastCursor = $c
}
# Scatti di rotella nel punto del cursore (-Horizontal: rotella orizzontale, come le due dita sul touchpad)
function Send-Wheel([int]$Delta, [int]$Count = 1, [switch]$Horizontal) {
    for ($i = 0; $i -lt $Count; $i++) {
        [NpxKit]::mouse_event($(if ($Horizontal) { 0x1000 } else { 0x0800 }), 0, 0, $Delta, [IntPtr]::Zero) # MOUSEEVENTF_HWHEEL / WHEEL
        Start-Sleep -Milliseconds 150
    }
}
function Mouse-Down([string]$Button = 'Left') { [NpxKit]::mouse_event($(if ($Button -eq 'Right') { 0x08 } else { 0x02 }), 0, 0, 0, [IntPtr]::Zero) }
function Mouse-Up([string]$Button = 'Left') { [NpxKit]::mouse_event($(if ($Button -eq 'Right') { 0x10 } else { 0x04 }), 0, 0, 0, [IntPtr]::Zero) }
function Click-At([int]$X, [int]$Y, [string]$Button = 'Left') { Move-To $X $Y 12; Start-Sleep -Milliseconds 150; Mouse-Down $Button; Start-Sleep -Milliseconds 60; Mouse-Up $Button }
function Double-Click-At([int]$X, [int]$Y) {
    Move-To $X $Y 12; Start-Sleep -Milliseconds 200
    Mouse-Down; Start-Sleep -Milliseconds 40; Mouse-Up; Start-Sleep -Milliseconds 90
    Mouse-Down; Start-Sleep -Milliseconds 40; Mouse-Up
}
# Trascinamento graduale; -HoldMs tiene premuto alla fine (es. per lo scorrimento automatico)
function Drag([int]$X1, [int]$Y1, [int]$X2, [int]$Y2, [int]$Steps = 25, [int]$DelayMs = 20, [int]$HoldMs = 250) {
    Move-To $X1 $Y1 12; Start-Sleep -Milliseconds 150
    Mouse-Down; Start-Sleep -Milliseconds 120
    try {
        for ($i = 1; $i -le $Steps; $i++) {
            Move-To ([int]($X1 + ($X2 - $X1) * $i / $Steps)) ([int]($Y1 + ($Y2 - $Y1) * $i / $Steps))
            Start-Sleep -Milliseconds $DelayMs
        }
        for ($t = 0; $t -lt $HoldMs; $t += 100) { Start-Sleep -Milliseconds 100; Move-To $X2 $Y2 }
    }
    finally { Mouse-Up }
}
function Get-CursorShape {
    $ci = New-Object NpxKit+CURSORINFO; $ci.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($ci)
    [NpxKit]::GetCursorInfo([ref]$ci) | Out-Null
    $names = @{ 32512 = 'ARROW'; 32513 = 'IBEAM'; 32644 = 'SIZEWE'; 32645 = 'SIZENS'; 32649 = 'HAND'; 32646 = 'SIZEALL' }
    foreach ($id in $names.Keys) { if ([NpxKit]::LoadCursor([IntPtr]::Zero, [IntPtr]$id) -eq $ci.hCursor) { return $names[$id] } }
    '0x{0:X}' -f [int64]$ci.hCursor
}
# Il punto deve cadere sulla finestra indicata (nessun'altra finestra sopra)
function Assert-OnWindow([int64]$Hwnd, [int]$X, [int]$Y) {
    $p = New-Object NpxKit+POINT; $p.X = $X; $p.Y = $Y
    $root = [NpxKit]::GetAncestor([NpxKit]::WindowFromPoint($p), 2)
    if ([int64]$root -ne $Hwnd) { throw ("Il punto ({0},{1}) non è sulla finestra di prova" -f $X, $Y) }
}
# Colori sullo schermo (la finestra deve essere visibile e in primo piano)
function Get-ScreenColor([int]$X, [int]$Y) {
    $bmp = New-Object Drawing.Bitmap 1, 1
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($X, $Y, 0, 0, $bmp.Size)
    $c = $bmp.GetPixel(0, 0); $g.Dispose(); $bmp.Dispose(); $c
}
function Test-SameColor($A, $B, [int]$Tolerance = 3) {
    if (-not $A -or -not $B) { return $false }
    [math]::Abs([int]$A.R - [int]$B.R) -le $Tolerance -and [math]::Abs([int]$A.G - [int]$B.G) -le $Tolerance -and [math]::Abs([int]$A.B - [int]$B.B) -le $Tolerance
}
function Format-Color($C) { if ($C) { '#{0:X2}{1:X2}{2:X2}' -f $C.R, $C.G, $C.B } else { '?' } }
# Colore che risulta disegnando $Over con opacità $Alpha (0-255) sopra $Base
function Blend($Base, $Over, [int]$Alpha) {
    $a = $Alpha / 255.0
    [Drawing.Color]::FromArgb([int][math]::Round($Base.R + ($Over.R - $Base.R) * $a), [int][math]::Round($Base.G + ($Over.G - $Base.G) * $a),
        [int][math]::Round($Base.B + ($Over.B - $Base.B) * $a))
}
function Shade($Color, [double]$Factor) {
    [Drawing.Color]::FromArgb([int][math]::Round($Color.R * $Factor), [int][math]::Round($Color.G * $Factor), [int][math]::Round($Color.B * $Factor))
}
# Pixel di una striscia orizzontale dello schermo
function Get-ScreenRow([int]$X, [int]$Y, [int]$Width) {
    $bmp = New-Object Drawing.Bitmap $Width, 1
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($X, $Y, 0, 0, $bmp.Size)
    $pixels = for ($i = 0; $i -lt $Width; $i++) { $bmp.GetPixel($i, 0) }
    $g.Dispose(); $bmp.Dispose()
    $pixels
}
# Colore di fondo della barra dei menu: il più frequente lungo la sua riga, dalla fine dei menu al centro
# della finestra (niente scritte né pulsanti)
function Get-MenuBarColor([int64]$Hwnd) {
    $view = Find-Control $Hwnd (UiName 'View' 'Visualizza') $CT::MenuItem
    if (-not $view) { return $null }
    $b = $view.Current.BoundingRectangle
    $wr = Get-WinRect $Hwnd
    $x1 = [int]($b.Right + 20); $x2 = [math]::Max($x1 + 40, [int](($wr.Left + $wr.Right) / 2))
    Get-ScreenRow $x1 ([int]($b.Y + $b.Height / 2)) ($x2 - $x1) | Group-Object { $_.ToArgb() } | Sort-Object Count -Descending |
        Select-Object -First 1 | ForEach-Object { $_.Group[0] }
}
# "Inchiostro" del titolo di una riga: pixel di testo (molto chiari nel tema scuro, molto scuri nel
# chiaro) nella zona del titolo, esclusi stanghetta a sinistra e X a destra. Un titolo più spesso ne ha di più.
function Measure-RowInk([int64]$Hwnd, [string]$Prefix, [bool]$Dark = $true) {
    $r = (Find-PanelRow $Hwnd $Prefix).Current.BoundingRectangle
    $x = [int]($r.X + $r.Width * 0.06); $w = [int]($r.Width * 0.7); $y = [int]($r.Y + $r.Height * 0.25); $height = [int]($r.Height * 0.5)
    $bmp = New-Object Drawing.Bitmap $w, $height
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x, $y, 0, 0, $bmp.Size)
    $ink = 0
    for ($i = 0; $i -lt $w; $i++) {
        for ($j = 0; $j -lt $height; $j++) {
            $c = $bmp.GetPixel($i, $j); $l = 0.299 * $c.R + 0.587 * $c.G + 0.114 * $c.B
            if (($Dark -and $l -gt 150) -or (-not $Dark -and $l -lt 90)) { $ink++ }
        }
    }
    $g.Dispose(); $bmp.Dispose()
    $ink
}
# true se lungo la linea centrale della riga (prime decine di pixel: l'icona) compare il colore
function Test-ColorInRow([int64]$Hwnd, [string]$Prefix, $Color, [int]$Tolerance = 12) {
    $r = (Find-PanelRow $Hwnd $Prefix).Current.BoundingRectangle
    $width = [int][math]::Min(90, $r.Width)
    $bmp = New-Object Drawing.Bitmap $width, 1
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen([int]$r.X, [int]($r.Y + $r.Height / 2), 0, 0, $bmp.Size)
    $found = $false
    for ($i = 0; $i -lt $width -and -not $found; $i++) { $found = Test-SameColor $bmp.GetPixel($i, 0) $Color $Tolerance }
    $g.Dispose(); $bmp.Dispose(); $found
}

# true se lo schermo si può catturare: in una sessione Desktop remoto con il client ridotto a icona la
# sessione non disegna e la cattura fallisce (i controlli sui pixel vanno saltati)
function Test-ScreenCapture {
    $bmp = New-Object Drawing.Bitmap 1, 1
    $g = [Drawing.Graphics]::FromImage($bmp)
    try { $g.CopyFromScreen(0, 0, 0, 0, $bmp.Size); $true } catch { $false } finally { $g.Dispose(); $bmp.Dispose() }
}
# Screenshot dello schermo nel rettangolo visibile della finestra (popup compresi, bordi invisibili esclusi).
# Se lo schermo non si può catturare (es. sessione Desktop remoto con il client ridotto a icona) si
# ripiega su PrintWindow, che disegna solo la finestra (senza popup).
function Save-WindowShot([int64]$Hwnd, [string]$Path) {
    $r = New-Object NpxKit+RECT
    if ([NpxKit]::DwmGetWindowAttribute([IntPtr]$Hwnd, 9, [ref]$r, 16) -ne 0) { $r = Get-WinRect $Hwnd } # DWMWA_EXTENDED_FRAME_BOUNDS
    $bmp = New-Object Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
    $g = [Drawing.Graphics]::FromImage($bmp)
    try { $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size) }
    catch {
        $g.Dispose(); $bmp.Dispose()
        $w = Get-WinRect $Hwnd
        $bmp = New-Object Drawing.Bitmap ($w.Right - $w.Left), ($w.Bottom - $w.Top)
        $g = [Drawing.Graphics]::FromImage($bmp)
        $dc = $g.GetHdc(); [NpxKit]::PrintWindow([IntPtr]$Hwnd, $dc, 2) | Out-Null; $g.ReleaseHdc($dc) # PW_RENDERFULLCONTENT
    }
    New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null
    $bmp.Save($Path); $g.Dispose(); $bmp.Dispose()
}

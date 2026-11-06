<#
.SYNOPSIS
    Test dal vivo delle schede verticali su una finestra di prova del Blocco note.

.DESCRIPTION
    Crea una finestra del Blocco note dedicata (File > Nuova finestra) con file di prova e verifica:
    allineamento del pannello, selezione nei due sensi, sfondo del pannello e della barra dei menu
    uguale alla barra del titolo, niente ricerca nel pannello (tolta), pannello compresso/espanso,
    colori delle schede dal menu contestuale (tinta tenue su tutta la riga e icona), aspetto della
    scheda attiva (bordino, stanghetta nel suo colore, titolo più spesso), titoli identici alle schede
    orizzontali (carattere, dimensione, peso, colore), "+" sotto l'ultima scheda,
    menu contestuale nativo e coerenza del Blocco note dopo il riordino, pulsante "Torna su" (compare
    scorrendo, riporta all'inizio, sparisce in cima, segue la scheda visibile e il pannello). Con -RealInput usa anche il
    mouse reale: riordino con trascinamento (anche a pannello compresso) e salvataggio dell'ordine
    nella sessione, scorrimento automatico, clic destro sotto il cursore, divisore
    (cursore, trascinamento, doppio clic). Infine controlla che fuori dal pannello il menu nativo
    resti quello originale.
    Alla fine chiude la finestra di prova e ripristina settings.ini. Le schede dell'utente non vengono
    toccate né stampate (le impostazioni sono condivise: durante il test il pannello delle altre
    finestre può comprimersi o cambiare larghezza per qualche secondo).

.EXAMPLE
    .\tools\test\Run-Tests.ps1                      # solo UI Automation, nessun input reale
    .\tools\test\Run-Tests.ps1 -RealInput           # anche mouse reale (avvisare chi usa il PC)
    .\tools\test\Run-Tests.ps1 -Restart -RealInput  # prima ricarica il Blocco note con la nuova build
#>
[CmdletBinding()]
param(
    [switch] $Restart,
    [switch] $RealInput,
    [switch] $KeepWindow,
    [string] $ShotsDir = (Join-Path $env:TEMP 'npx-test-shots')
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\TestKit.ps1"

$results = [System.Collections.Generic.List[object]]::new()
function Check([string]$Name, [bool]$Ok, [string]$Detail = '') {
    $results.Add([pscustomobject]@{ Test = $Name; Ok = $Ok; Dettagli = $Detail })
    $mark = if ($Ok) { '[OK]      ' } else { '[FALLITO] ' }
    Write-Host ($mark + $Name + $(if ($Detail) { "  ($Detail)" } else { '' })) -ForegroundColor $(if ($Ok) { 'Green' } else { 'Red' })
}
function Join-Order([object[]]$Order) { $Order -join ',' }
function Wait-Until([scriptblock]$Condition, [int]$TimeoutMs = 3000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) { if (& $Condition) { return $true }; Start-Sleep -Milliseconds 200 }
    [bool](& $Condition)
}
# Come Wait-Until, ma la condizione deve valere ancora dopo un attimo (niente stati che rimbalzano)
function Wait-Stable([scriptblock]$Condition, [int]$TimeoutMs = 3000, [int]$HoldMs = 1500) {
    if (-not (Wait-Until $Condition $TimeoutMs)) { return $false }
    Start-Sleep -Milliseconds $HoldMs
    [bool](& $Condition)
}
# Punto su cui premere in una riga: sul titolo (o al centro, se il pannello è compresso)
function Row-Point([int64]$Hwnd, [string]$Prefix, [double]$Fraction = 0.5) {
    $r = (Find-PanelRow $Hwnd $Prefix).Current.BoundingRectangle
    [int]($r.X + [math]::Min(60, $r.Width / 2)), [int]($r.Y + $r.Height * $Fraction)
}
function Reopen([int64]$Hwnd, [int[]]$Numbers) {
    foreach ($n in $Numbers) { Open-FileInWindow $Hwnd (Join-Path $NpxTestDir "$NpxTestPrefix$n.txt"); Start-Sleep -Milliseconds 700 }
}
# true se nella riga c'è l'iniziale dell'icona (un testo di una sola lettera o cifra; i glifi dei
# pulsanti sono caratteri privati e non contano)
function Test-RowHasInitial([int64]$Hwnd, [string]$Prefix) {
    $row = Find-PanelRow $Hwnd $Prefix
    if (-not $row) { return $false }
    $texts = @($row.FindAll($TS::Descendants, (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::Text))))
    @($texts | Where-Object { $_.Current.Name.Length -eq 1 -and [char]::IsLetterOrDigit($_.Current.Name[0]) }).Count -gt 0
}
# Barra dei menu nella riga del titolo: lo spazio vuoto tra "Visualizza" e "Novità" è della finestra (sotto il punto c'è
# lei, non l'isola: buco nella regione) e risponde come barra del titolo; "Visualizza" e "Novità" restano dell'isola
function Test-MenuBarGap([int64]$Hwnd, [int64]$MenuBarHwnd, [string]$Label) {
    $view = Find-Control $Hwnd 'Visualizza' $CT::MenuItem
    $news = Find-Control $Hwnd 'Novità' $CT::Button
    if (-not $view -or -not $news) { Check "${Label}: ""Visualizza"" e ""Novità"" trovati" $false; return }
    $v = $view.Current.BoundingRectangle; $n = $news.Current.BoundingRectangle
    $y = [int]($v.Y + $v.Height / 2)
    $gapX = [int](($v.X + $v.Width + $n.X) / 2)
    $gapUnder = Get-WindowAtScreen $gapX $y
    $gapHit = Get-NcHitScreen $Hwnd $gapX $y
    $edgeX = [int]($v.X + $v.Width + 24) # appena a destra di "Visualizza"
    $edgeOk = (Get-WindowAtScreen $edgeX $y) -eq $Hwnd -and (Get-NcHitScreen $Hwnd $edgeX $y) -eq 2
    Check "${Label}: lo spazio vuoto tra i menu e ""Novità"" trascina la finestra" ($gapUnder -eq $Hwnd -and $gapHit -eq 2 -and $edgeOk) ("x={0}: sotto il punto la finestra {1}, risposta {2}; vicino a ""Visualizza"" {3}" -f $gapX, ($gapUnder -eq $Hwnd), $gapHit, $edgeOk)
    $viewUnder = Get-WindowAtScreen ([int]($v.X + $v.Width / 2)) $y
    $newsUnder = Get-WindowAtScreen ([int]($n.X + $n.Width / 2)) ([int]($n.Y + $n.Height / 2))
    Check "${Label}: ""Visualizza"" e ""Novità"" restano cliccabili" ($viewUnder -eq $MenuBarHwnd -and $newsUnder -eq $MenuBarHwnd) ("sotto Visualizza {0:x}, sotto Novità {1:x}, barra {2:x}" -f $viewUnder, $newsUnder, $MenuBarHwnd)
}
function Open-ColorMenu([int64]$Hwnd, [string]$Prefix) {
    Show-ContextMenu $Hwnd $Prefix
    $sub = Find-InProcess 'Colore scheda'
    if ($sub) {
        try { $sub.GetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern).Expand() } catch { Invoke-Any $sub }
        Start-Sleep -Milliseconds 600
    }
    $sub
}

# Impostazioni: il test ricorda i valori originali delle chiavi che cambia e alla fine ripristina solo
# quelle che hanno ancora il valore scritto dal test. Così, se nel frattempo l'utente cambia qualcosa
# (ad esempio la larghezza del pannello), la sua scelta resta.
$devWindows = Join-Path $NpxData 'dev-windows.txt'
$originalSettings = @{}
foreach ($key in 'Enabled', 'Collapsed', 'Width', 'VerboseLog') { $originalSettings[$key] = Get-IniValue $key }
# [BackToTop] Enabled ha lo stesso nome di [VerticalTabs] Enabled: si legge e si ripristina per sezione
$originalBackToTop = Get-IniSectionValue 'BackToTop' 'Enabled'
$originalReopenTab = Get-IniSectionValue 'ReopenTab' 'Enabled'
$originalSmoothScroll = Get-IniSectionValue 'SmoothScroll' 'Disabled'
$testWrites = @{}
function Set-TestOpt([string]$Section, [string]$Key, [string]$Value) {
    Set-Opt $Section $Key $Value
    $testWrites[$Key] = @{ Section = $Section; Value = $Value }
}
# Comprimi/espandi con il pulsante del pannello: è il pannello a scrivere Collapsed in settings.ini, e
# il test lo annota come una sua scrittura (così, anche se si interrompe a metà, torna il valore originale)
function Invoke-PanelToggle([int64]$Hwnd, [bool]$Collapse) {
    $testWrites['Collapsed'] = @{ Section = 'VerticalTabs'; Value = $(if ($Collapse) { '1' } else { '0' }) }
    Invoke-Element (Find-Control $Hwnd $(if ($Collapse) { 'Comprimi schede' } else { 'Espandi schede' }) $CT::Button)
}
$cursorStart = Get-Cursor
$h = 0
$devWindowsOurs = $false
$reopenIsolated = $false
try {
    if ($Restart) { Restart-Notepad }
    if (-not (Get-NpProcess)) { throw 'Il Blocco note non è aperto.' }
    if (Test-Path $devWindows) { throw 'C''è già un dev-windows.txt (modalità sviluppo): rimuovilo prima del test.' }
    # Schede verticali spente dall'utente: il pannello si accende solo nella finestra di prova (allowlist
    # dev-windows.txt, per ora vuota), così le sue finestre restano come le ha lasciate
    $onlyTestWindow = (Get-IniValue 'Enabled') -eq '0'
    if ($onlyTestWindow) {
        Write-Host 'Schede verticali spente: le accendo solo nella finestra di prova.'
        Set-Content -LiteralPath $devWindows -Value '0' -Encoding ASCII
        $devWindowsOurs = $true
        Start-Sleep -Milliseconds 800
    }
    Set-TestOpt 'VerticalTabs' 'Enabled' '1'; Set-TestOpt 'VerticalTabs' 'Collapsed' '0'
    Set-TestOpt 'Debug' 'VerboseLog' '1'
    Start-Sleep -Seconds 2

    # Cronologia di Ctrl+Maiusc+T (comune a tutte le finestre, su disco): quella dell'utente sta da parte durante la prova
    Enter-ReopenIsolation
    $reopenIsolated = $true
    Write-Host 'Creo la finestra di prova...'
    $h = New-TestWindow -Files 5 -OnCreated { param($hwnd) if ($onlyTestWindow) { Set-Content -LiteralPath $devWindows -Value ([string]$hwnd) -Encoding ASCII } }
    $expected = 'Senza titolo,npx-test-1,npx-test-2,npx-test-3,npx-test-4,npx-test-5'

    # --- Allineamento e selezione -------------------------------------------------------------
    Check 'Pannello allineato alle schede native' ((Join-Order (Get-PanelOrder $h)) -eq $expected) (Join-Order (Get-PanelOrder $h))
    Select-Element (Find-PanelRow $h 'npx-test-3')
    Check 'Selezione dal pannello al Blocco note' (Wait-Until { (Get-SelectedNative $h) -eq 'npx-test-3' })
    Select-Element (Find-NativeTab $h 'npx-test-1')
    Check 'Selezione dal Blocco note al pannello' (Wait-Until { (Get-SelectedPanel $h) -eq 'npx-test-1' })
    # Titoli come nelle schede orizzontali: carattere, dimensione, peso e colore (secondo UI Automation),
    # per una scheda non attiva (npx-test-2) e per quella attiva (npx-test-1)
    $fontText = { param($f) if ($f) { '{0} {1} peso {2} colore {3:X6}' -f $f.Name, $f.Size, $f.Weight, $f.Color } else { '?' } }
    $nativeFont = Get-TextFont (Find-NativeTab $h 'npx-test-2')
    $panelFont = Get-TextFont (Find-PanelRow $h 'npx-test-2')
    Check 'Titoli grandi come nelle schede orizzontali' ($nativeFont -and $panelFont -and $nativeFont.Size -eq $panelFont.Size -and $nativeFont.Name -eq $panelFont.Name) ("schede {0}, pannello {1}" -f (& $fontText $nativeFont), (& $fontText $panelFont))
    Check 'Scheda non attiva: peso e colore del titolo come nelle schede orizzontali' ($nativeFont -and $panelFont -and $nativeFont.Weight -eq $panelFont.Weight -and $nativeFont.Color -eq $panelFont.Color) ("schede {0}, pannello {1}" -f (& $fontText $nativeFont), (& $fontText $panelFont))
    $nativeSel = Get-TextFont (Find-NativeTab $h 'npx-test-1')
    $panelSel = Get-TextFont (Find-PanelRow $h 'npx-test-1')
    Check 'Scheda attiva: peso e colore del titolo come nelle schede orizzontali' ($nativeSel -and $panelSel -and $nativeSel.Weight -eq $panelSel.Weight -and $nativeSel.Color -eq $panelSel.Color -and $nativeSel.Weight -gt $nativeFont.Weight) ("schede {0}, pannello {1}" -f (& $fontText $nativeSel), (& $fontText $panelSel))

    # --- "+" subito sotto l'ultima scheda, solo icona, centrato -----------------------------------
    $plus = Find-Control $h 'Nuova scheda' $CT::Button
    $last = (Get-PanelRows $h)[-1].Current.BoundingRectangle
    $pb = $plus.Current.BoundingRectangle
    $lb = (Get-PanelList $h).Current.BoundingRectangle
    Check '"+" subito sotto l''ultima scheda' ($pb.Y -ge $last.Bottom - 2 -and $pb.Y - $last.Bottom -le 12) ("ultima riga fino a {0}, ""+"" da {1}" -f [int]$last.Bottom, [int]$pb.Y)
    $label = $plus.FindFirst($TS::Descendants, (New-Object Windows.Automation.AndCondition(
                (New-Object Windows.Automation.PropertyCondition($AE::NameProperty, 'Nuova scheda')),
                (New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::Text)))))
    Check '"+" centrato e senza scritta' ([math]::Abs(($pb.X + $pb.Width / 2) - ($lb.X + $lb.Width / 2)) -le 3 -and -not $label) ("centro + {0}, centro elenco {1}" -f [int]($pb.X + $pb.Width / 2), [int]($lb.X + $lb.Width / 2))
    1..18 | ForEach-Object { Invoke-Element (Find-Control $h 'Nuova scheda' $CT::Button); Start-Sleep -Milliseconds 250 }
    Start-Sleep -Milliseconds 900
    $pr = Get-PanelRect $h
    $pb = (Find-Control $h 'Nuova scheda' $CT::Button).Current.BoundingRectangle
    $lb = (Get-PanelList $h).Current.BoundingRectangle
    Check 'Con molte schede l''elenco scorre e il "+" resta in fondo' ($pb.Bottom -le $pr.Y + $pr.H + 1 -and $pb.Y -ge $lb.Bottom - 2 -and $pb.Bottom -ge $pr.Y + $pr.H - 60) ("elenco fino a {0}, ""+"" {1}-{2}, pannello fino a {3}" -f [int]$lb.Bottom, [int]$pb.Y, [int]$pb.Bottom, ($pr.Y + $pr.H))
    Show-ContextMenu $h 'npx-test-5'
    Invoke-MenuItem 'Chiudi schede a destra'
    Check 'Schede vuote aggiunte richiuse' (Wait-Until { (Join-Order (Get-NativeOrder $h)) -eq $expected -and (Join-Order (Get-PanelOrder $h)) -eq $expected }) (Join-Order (Get-PanelOrder $h))
    Select-Element (Find-PanelRow $h 'npx-test-1')
    Wait-Until { (Get-SelectedNative $h) -eq 'npx-test-1' } | Out-Null

    # --- Sfondo come la barra del titolo (pannello e barra dei menu) --------------------------------
    # I controlli sui pixel valgono solo con la finestra di prova in primo piano e il mouse fuori dal
    # pannello (il passaggio del mouse cambia il fondo delle righe).
    $wr = Get-WinRect $h
    $titlePoint = @([int]($wr.Left + ($wr.Right - $wr.Left) * 0.45), ($wr.Top + 14))
    $dark = $true
    # Schermo catturabile (non lo è, ad esempio, con il client di Desktop remoto ridotto a icona)
    $screen = Test-ScreenCapture
    if (-not $screen) { Write-Warning 'Schermo non catturabile (es. Desktop remoto ridotto a icona): controlli sui pixel saltati.' }
    $pixels = $screen -and [int64][NpxKit]::GetForegroundWindow() -eq $h
    $title = $null; $panel = $null
    if ($pixels) {
        $pr = Get-PanelRect $h
        $title = Get-ScreenColor $titlePoint[0] $titlePoint[1]
        $panel = Get-ScreenColor ($pr.X + [int]($pr.W / 2)) ($pr.Y + $pr.H - 90)
        $dark = $title.R -lt 128
        Check 'Sfondo del pannello uguale alla barra del titolo' (Test-SameColor $title $panel 2) ("titolo {0}, pannello {1}" -f (Format-Color $title), (Format-Color $panel))
        $menu = Get-MenuBarColor $h
        Check 'Barra dei menu dello stesso colore della barra del titolo' (Test-SameColor $title $menu 2) ("titolo {0}, menu {1}" -f (Format-Color $title), (Format-Color $menu))
        $c = Get-Cursor
        if ($c.X -ge $pr.X -and $c.X -le $pr.X + $pr.W -and $c.Y -ge $pr.Y -and $c.Y -le $pr.Y + $pr.H) {
            Write-Warning 'Mouse sopra il pannello: controlli sui colori delle righe saltati.'
            $pixels = $false
        }
    }
    elseif ($screen) { Write-Warning 'Finestra di prova non in primo piano: controlli sui colori saltati.' }
    # Velo neutro e colori della scheda nel tema in uso (come in TabColors.cpp e VerticalTabs.cpp)
    $veil = if ($dark) { [Drawing.Color]::White } else { [Drawing.Color]::Black }
    $alpha = if ($dark) { @{ Colored = 0x26; ColoredSelected = 0x45; Neutral = 0x0B; NeutralSelected = 0x1A; Border = 0x22 } }
             else { @{ Colored = 0x1F; ColoredSelected = 0x38; Neutral = 0x08; NeutralSelected = 0x0F; Border = 0x1F } }

    # --- Colonna in cima alla finestra (come le schede verticali di Chrome) --------------------------
    # Pannello a sinistra: dalla cima della finestra al fondo dell'editor; barra dei menu ed editor dal suo bordo.
    # Nella fascia del titolo in cima alla colonna, fuori dai pulsanti, l'isola del pannello non c'è (regione):
    # il mouse arriva alla finestra, che risponde come la sua barra del titolo.
    $origin = Get-ClientOrigin $h
    $pr = Get-PanelRect $h; $er = Get-EditorRect $h; $mr = Get-MenuBarRect $h
    $panelScale = [NpxGeo]::GetDpiForWindow([IntPtr]$h) / 96.0
    Check 'Colonna dalla cima della finestra al fondo dell''editor' ($pr.Y -eq $origin.Y -and $pr.Y + $pr.H -eq $er.Y + $er.H) "colonna y $($pr.Y)-$($pr.Y + $pr.H), finestra da y=$($origin.Y), editor fino a y=$($er.Y + $er.H)"
    Check '  barra dei menu ed editor dal bordo della colonna' ($mr -and $mr.X -eq $pr.X + $pr.W -and $er.X -eq $pr.X + $pr.W -and $mr.Y + $mr.H -eq $er.Y) "barra dei menu da x=$($mr.X), editor da x=$($er.X), colonna fino a x=$($pr.X + $pr.W)"
    $bandX = $pr.X + [int]($pr.W / 2); $bandY = $pr.Y + [int](16 * $panelScale)
    $under = Get-WindowAtScreen $bandX $bandY
    $hit = Get-NcHitScreen $h $bandX $bandY
    Check '  fascia del titolo della colonna: lo spazio vuoto è della finestra (barra del titolo)' ($under -eq $h -and $hit -eq 2) ("sotto il punto {0:x} (finestra {1:x}), risposta {2}" -f $under, $h, $hit)
    Check '  sotto la fascia, il pannello' ((Get-WindowAtScreen $bandX ($pr.Y + [int](60 * $panelScale))) -eq $pr.Hwnd)
    $toggleBox = (Find-Control $h 'Comprimi schede' $CT::Button).Current.BoundingRectangle
    $toggleOk = $toggleBox.Y -lt $pr.Y + 44 * $panelScale -and (Get-WindowAtScreen ([int]($toggleBox.X + $toggleBox.Width / 2)) ([int]($toggleBox.Y + $toggleBox.Height / 2))) -eq $pr.Hwnd
    Check '  il suo pulsante (comprimi), nella fascia, resta del pannello' $toggleOk ("comprimi y={0}" -f [int]$toggleBox.Y)
    # La ricerca nelle schede è stata tolta (2026-11-05): nel pannello non c'è più né la lente né la casella
    Check '  niente ricerca nel pannello' (-not (Find-Control $h 'Cerca nelle schede' $CT::Button) -and -not (Find-Control $h 'Cerca nelle schede' $CT::Edit))
    # Riga del titolo nascosta (rimasta vuota con le schede nella colonna): la barra dei menu sale nella riga del
    # titolo, fino ai pulsanti della finestra più un tratto libero; l'editor sale con lei e la sua superficie di testo
    # si allunga; i pulsanti della finestra, il tratto libero (trascinabile) e il bordo superiore restano della finestra
    $clientW = (Get-WinRect $h).Right - (Get-WinRect $h).Left - 2 * ($origin.X - (Get-WinRect $h).Left)
    $clientRight = $origin.X + $clientW
    $reservePx = [math]::Round(154 * $panelScale)
    Check '  barra dei menu nella riga del titolo, fino ai pulsanti della finestra' ($mr.Y - $origin.Y -ge 0 -and $mr.Y - $origin.Y -le 5 -and [math]::Abs(($mr.X + $mr.W) - ($clientRight - $reservePx)) -le 2) ("barra dei menu y={0} (finestra da y={1}), fino a x={2} (pulsanti della finestra da x={3})" -f $mr.Y, $origin.Y, ($mr.X + $mr.W), ($clientRight - $reservePx))
    $menuItemY = (Find-Control $h 'File' $CT::MenuItem).Current.BoundingRectangle
    Check '  menu allineati ai pulsanti della colonna' ([math]::Abs(($menuItemY.Y + $menuItemY.Height / 2) - ($toggleBox.Y + $toggleBox.Height / 2)) -le 4) ("File a y={0:N0}, comprimi a y={1:N0}" -f ($menuItemY.Y + $menuItemY.Height / 2), ($toggleBox.Y + $toggleBox.Height / 2))
    $surface = Get-EditorSurface $h
    Check '  superficie di testo alta quanto l''editor salito' ($surface.Surface -and $surface.Surface.Y -eq $surface.Container.Y -and $surface.Surface.H -eq $surface.Container.H) ("editor y={0} h={1}, superficie y={2} h={3}" -f $surface.Container.Y, $surface.Container.H, $surface.Surface.Y, $surface.Surface.H)
    $gapX = $clientRight - $reservePx + 6
    # Pulsanti della finestra: i nostri (CaptionButtons), dalla cima della finestra al fondo della barra dei menu salita
    # come in Chrome, al posto di quelli di DWM (più bassi); 3 da 46 DIP dal bordo destro, primi nell'ordine Z
    $ours = Get-CaptionButtons $h
    $br = if ($ours) { $r = Get-WinRect $ours; [pscustomobject]@{ X = $r.Left; Y = $r.Top; W = $r.Right - $r.Left; H = $r.Bottom - $r.Top } } else { $null }
    $buttonsW = 3 * [math]::Round(46 * $panelScale)
    Check '  pulsanti della finestra nostri, alti quanto la barra dei menu (quelli di DWM nascosti)' ($br -and $br.Y -eq $origin.Y -and $br.Y + $br.H -eq $mr.Y + $mr.H -and $br.X + $br.W -eq $clientRight -and [math]::Abs($br.W - $buttonsW) -le 1 -and (Get-DwmButtonsWidth $h) -eq 0) ("pulsanti {0}, barra dei menu fino a y={1}, finestra da y={2}, DWM largo {3}" -f $(if ($br) { "[$($br.X),$($br.Y) $($br.W)x$($br.H)]" } else { 'assenti' }), ($mr.Y + $mr.H), $origin.Y, (Get-DwmButtonsWidth $h))
    Check '  primi nell''ordine Z delle figlie' ($ours -and [int64][NpxKit]::GetWindow([IntPtr]$h, 5) -eq $ours) ("prima figlia {0:x}" -f [int64][NpxKit]::GetWindow([IntPtr]$h, 5))
    # Riduci/ingrandisci/chiudi a metà e in fondo alla riga, bordo superiore in cima (la nostra finestra lo lascia, 1 px, alla
    # finestra principale), tratto libero alla loro sinistra trascinabile. Hit test solo alle finestre del Blocco note di prova:
    # i nostri pulsanti dove ci sono, altrimenti la finestra principale (HTTRANSPARENT passa a lei, come fa il sistema)
    $hitAt = {
        param($X, $Y)
        $target = if ($br -and $X -ge $br.X -and $X -lt $br.X + $br.W -and $Y -ge $br.Y -and $Y -lt $br.Y + $br.H) { $ours } else { $h }
        $code = Get-NcHitScreen $target $X $Y
        if ($code -eq -1 -and $target -ne $h) { $code = Get-NcHitScreen $h $X $Y }
        $code
    }
    $capHits = @((& $hitAt ($clientRight - 120) ($origin.Y + 16)), (& $hitAt ($clientRight - 70) ($origin.Y + 16)), (& $hitAt ($clientRight - 20) ($origin.Y + 16)))
    $lowHits = @((& $hitAt ($clientRight - 120) ($mr.Y + $mr.H - 3)), (& $hitAt ($clientRight - 20) ($mr.Y + $mr.H - 3)))
    $edgeHit = & $hitAt ($clientRight - 20) $origin.Y
    $gapUnder = Get-WindowAtScreen $gapX ($origin.Y + 16)
    $gapHit = Get-NcHitScreen $h $gapX ($origin.Y + 16)
    Check '  pulsanti della finestra al loro posto su tutta la riga, bordo in cima, tratto libero trascinabile' (($capHits -join ',') -eq '8,9,20' -and ($lowHits -join ',') -eq '8,20' -and $edgeHit -eq 12 -and $gapUnder -eq $h -and $gapHit -eq 2) ("riduci/ingrandisci/chiudi {0}; in fondo {1}; in cima {2}; tratto libero: finestra {3}, risposta {4}" -f ($capHits -join ','), ($lowHits -join ','), $edgeHit, ($gapUnder -eq $h), $gapHit)
    $topUnder = Get-WindowAtScreen ($mr.X + 100) ($origin.Y + 1)
    $topHit = Get-NcHitScreen $h ($mr.X + 100) ($origin.Y + 1)
    Check '  in cima, sopra la barra dei menu: il bordo della finestra (ridimensiona)' ($topUnder -eq $h -and $topHit -eq 12) ("sotto il punto finestra {0}, risposta {1}" -f ($topUnder -eq $h), $topHit)
    # Zona vuota della barra dei menu salita (tra "Visualizza" e "Novità"): la sua isola XAML si prendeva il mouse, ora lì
    # la sua regione ha un buco e la finestra risponde come barra del titolo; menu e pulsanti restano della barra
    Test-MenuBarGap $h $mr.Hwnd '  barra dei menu'
    # La finestra principale non ha WS_CLIPCHILDREN: se ridipinge il suo sfondo (senza le figlie) copre l'editor salito, che
    # deve tornare com'era (prima, dopo comprimi/espandi, restava una fascia scura in cima all'editor sotto la riga del titolo)
    if ($screen -and [int64][NpxKit]::GetForegroundWindow() -eq $h) {
        $topBefore = Get-EditorTopColors $h
        Invoke-MainRepaintOnly $h
        Start-Sleep -Milliseconds 800
        $topAfter = Get-EditorTopColors $h
        Check '  la finestra ridipinta non lascia coperto l''editor' ((Test-SameColor $topBefore.Left $topAfter.Left 3) -and (Test-SameColor $topBefore.Below $topAfter.Below 3) -and (Test-SameColor $topAfter.Left $topAfter.Right 3)) ("in cima {0} -> {1}, sotto {2} -> {3}, a destra {4}" -f (Format-Color $topBefore.Left), (Format-Color $topAfter.Left), (Format-Color $topBefore.Below), (Format-Color $topAfter.Below), (Format-Color $topAfter.Right))
        # Zona vetro (Mica) del Blocco note: arrivava fino al fondo della barra dei menu originale e la cima dell'editor
        # salito, lì dentro, era una fascia più chiara (DWM somma i suoi pixel allo sfondo). Ora finisce dove comincia
        # l'editor, anche dopo ingrandisci/ripristina (lì il Blocco note la reimposta)
        $bandNormal = Get-EditorBandColors $h
        [void][NpxKit]::ShowWindow([IntPtr]$h, 3); Start-Sleep -Milliseconds 1500 # SW_MAXIMIZE
        $bandMax = Get-EditorBandColors $h
        [void][NpxKit]::ShowWindow([IntPtr]$h, 9); Start-Sleep -Milliseconds 1500 # SW_RESTORE
        $bandRestored = Get-EditorBandColors $h
        $bandOk = @($bandNormal, $bandMax, $bandRestored | Where-Object { -not (Test-SameColor $_.Top $_.Middle 3) }).Count -eq 0
        Check '  niente fascia chiara in cima all''editor (normale, ingrandita, ripristinata)' $bandOk ("in cima/a metà: {0}/{1}, {2}/{3}, {4}/{5}" -f (Format-Color $bandNormal.Top), (Format-Color $bandNormal.Middle), (Format-Color $bandMax.Top), (Format-Color $bandMax.Middle), (Format-Color $bandRestored.Top), (Format-Color $bandRestored.Middle))
    }

    # --- Pannello compresso / espanso -------------------------------------------------------------
    $scale = (Get-PanelRect $h).W / [double](Get-IniValue 'Width')
    Invoke-PanelToggle $h $true
    Check 'Comprimi: pannello stretto (56 DIP)' (Wait-Stable { [math]::Abs((Get-PanelRect $h).W - 56 * $scale) -le 3 }) ("{0} px" -f (Get-PanelRect $h).W)
    $pr = Get-PanelRect $h; $er = Get-EditorRect $h
    Check 'Comprimi: l''editor si allarga accanto al pannello' ([math]::Abs($er.X - ($pr.X + $pr.W)) -le 2) "editor x=$($er.X)"
    Check 'Comprimi: stato salvato' ((Get-IniValue 'Collapsed') -eq '1')
    Check 'Comprimi: tutte le schede restano nell''elenco' ((Join-Order (Get-PanelOrder $h)) -eq $expected)
    Check 'Comprimi: pulsante "Espandi schede"' ($null -ne (Find-Control $h 'Espandi schede' $CT::Button))
    Test-MenuBarGap $h (Get-MenuBarRect $h).Hwnd 'Comprimi: barra dei menu'
    $topCheck = $screen -and [int64][NpxKit]::GetForegroundWindow() -eq $h
    if ($topCheck) {
        Start-Sleep -Milliseconds 500
        $top = Get-EditorTopColors $h
        Check 'Comprimi: nessuna fascia scura in cima all''editor' (Test-SameColor $top.Left $top.Right 3) ("a sinistra {0}, a destra {1}" -f (Format-Color $top.Left), (Format-Color $top.Right))
    }
    Save-WindowShot $h (Join-Path $ShotsDir 'compresso.png')
    Invoke-PanelToggle $h $false
    Check 'Espandi: di nuovo largo, stato salvato' (Wait-Stable { [math]::Abs((Get-PanelRect $h).W - [double](Get-IniValue 'Width') * $scale) -le 3 -and (Get-IniValue 'Collapsed') -eq '0' }) ("{0} px" -f (Get-PanelRect $h).W)
    # Comprimi e subito espandi (come un doppio clic): deve restare espanso, senza rimbalzi
    Invoke-PanelToggle $h $true
    Wait-Until { (Get-IniValue 'Collapsed') -eq '1' } | Out-Null
    Invoke-PanelToggle $h $false
    Check 'Comprimi e subito espandi: resta espanso alla larghezza di prima' (Wait-Stable { [math]::Abs((Get-PanelRect $h).W - [double](Get-IniValue 'Width') * $scale) -le 3 -and (Get-IniValue 'Collapsed') -eq '0' }) ("{0} px" -f (Get-PanelRect $h).W)
    if ($topCheck -and [int64][NpxKit]::GetForegroundWindow() -eq $h) {
        $top = Get-EditorTopColors $h
        Check 'Espandi: nessuna fascia scura in cima all''editor' (Test-SameColor $top.Left $top.Right 3) ("a sinistra {0}, a destra {1}" -f (Format-Color $top.Left), (Format-Color $top.Right))
    }

    # --- Colori delle schede ----------------------------------------------------------------------
    # (tab-colors.ini contiene anche i colori delle schede dell'utente: si conta rispetto a prima)
    $colorsFile = Join-Path $NpxData 'tab-colors.ini'
    $redBefore = @(Get-Content $colorsFile -ErrorAction SilentlyContinue | Where-Object { $_ -match '=red$' }).Count
    $sub = Open-ColorMenu $h 'npx-test-2'
    Check 'Menu contestuale: voce "Colore scheda"' ($null -ne $sub)
    $palette = 'Nessuno', 'Grigio', 'Blu', 'Rosso', 'Giallo', 'Verde', 'Rosa', 'Viola', 'Azzurro', 'Arancione'
    Check 'Sottomenu con "Nessuno" e 9 colori' (@($palette | Where-Object { Find-InProcess $_ }).Count -eq 10)
    Invoke-Any (Find-InProcess 'Rosso')
    Check 'Colore assegnato alla scheda' (Wait-Until { (Find-PanelRow $h 'npx-test-2').Current.ItemStatus -eq 'Colore: Rosso' }) ((Find-PanelRow $h 'npx-test-2').Current.ItemStatus)
    Check 'Colore salvato in tab-colors.ini' (@(Get-Content $colorsFile -ErrorAction SilentlyContinue | Where-Object { $_ -match '=red$' }).Count -eq $redBefore + 1)
    $red = if ($dark) { [Drawing.Color]::FromArgb(0xF2, 0x8B, 0x82) } else { [Drawing.Color]::FromArgb(0xD9, 0x30, 0x25) }
    Check 'Menu chiuso e striscia nativa fuori vista' ((Wait-Until { @(Get-OpenPopups).Count -eq 0 }) -and (Test-NativeStripHidden $h))
    # Da espanso nessuna icona: il colore è nella tinta della riga (non selezionata). Confronto con una
    # riga senza colore, nello stesso punto relativo (stesse condizioni di disegno).
    Wait-Stable { [math]::Abs((Get-PanelRect $h).W - [double](Get-IniValue 'Width') * $scale) -le 3 } | Out-Null
    Check 'Da espanso nessuna icona nelle righe' (-not (Test-RowHasInitial $h 'npx-test-3') -and -not (Test-RowHasInitial $h 'Senza titolo') -and -not ($screen -and (Test-ColorInRow $h 'npx-test-2' $red)))
    $r = (Find-PanelRow $h 'npx-test-2').Current.BoundingRectangle
    $r4 = (Find-PanelRow $h 'npx-test-4').Current.BoundingRectangle
    $px = [int]($r.X + $r.Width * 0.72); $py = [int]($r.Y + $r.Height / 2)
    $cursor = Get-Cursor
    $hovered = @($r, $r4 | Where-Object { $cursor.X -ge $_.X -and $cursor.X -le $_.Right -and $cursor.Y -ge $_.Y -and $cursor.Y -le $_.Bottom }).Count -gt 0
    $bg = $null; $c = $null
    if ($screen) {
        $bg = Get-ScreenColor $px ([int]($r4.Y + $r4.Height / 2))
        $c = Get-ScreenColor $px $py
    }
    $pixels = $pixels -and -not $hovered -and [int64][NpxKit]::GetForegroundWindow() -eq $h
    if ($pixels) {
        $expected = Blend $panel $red $alpha.Colored
        Check 'Tutta la riga nel colore scelto, in tinta tenue' (Test-SameColor $c $expected 3) ("{0}, attesa {1}" -f (Format-Color $c), (Format-Color $expected))
        $expected = Blend $panel $veil $alpha.Neutral
        Check 'Riga senza colore: velo neutro appena visibile' (Test-SameColor $bg $expected 3) ("{0}, atteso {1}" -f (Format-Color $bg), (Format-Color $expected))
    }
    Save-WindowShot $h (Join-Path $ShotsDir 'riga-colorata.png')

    # --- Scheda attiva: tinta più decisa, bordino, stanghetta nel colore della scheda, titolo più spesso ---
    $panelTextNormal = $null; $panelTextSelected = $null
    if ($pixels) {
        $panelTextNormal = Get-RowTextColor $h 'npx-test-4' $dark   # senza colore, non attiva
        $inkNormal = Measure-RowInk $h 'npx-test-2' $dark
        Select-Element (Find-PanelRow $h 'npx-test-2')
        Wait-Until { (Get-SelectedPanel $h) -eq 'npx-test-2' } | Out-Null
        Start-Sleep -Milliseconds 500
        $c = Get-ScreenColor $px $py
        $selectedCard = Blend $panel $red $alpha.ColoredSelected
        Check 'Scheda attiva: tinta più decisa' (Test-SameColor $c $selectedCard 3) ("{0}, attesa {1}" -f (Format-Color $c), (Format-Color $selectedCard))
        # Bordino: scendendo dal bordo alto della riga, il primo pixel diverso dallo sfondo del pannello
        $edge = $null
        for ($y = [int]$r.Y; $y -lt $py -and -not $edge; $y++) { $p = Get-ScreenColor $px $y; if (-not (Test-SameColor $p $panel 1)) { $edge = $p } }
        $expected = Blend $selectedCard $veil $alpha.Border
        Check 'Scheda attiva: bordino' (Test-SameColor $edge $expected 4) ("{0}, atteso {1}" -f (Format-Color $edge), (Format-Color $expected))
        $pill = if ($dark) { Shade $red 0.9 } else { $red }
        Check 'Scheda attiva: stanghetta nel colore della scheda' (Test-ColorInRow $h 'npx-test-2' $pill 6) (Format-Color $pill)
        $inkBold = Measure-RowInk $h 'npx-test-2' $dark
        Check 'Scheda attiva: titolo più spesso' ($inkBold -ge $inkNormal * 1.15) ("{0} -> {1} pixel di testo" -f $inkNormal, $inkBold)
        Save-WindowShot $h (Join-Path $ShotsDir 'scheda-attiva.png')

        # Senza colore: velo neutro più deciso, bordino e stanghetta grigia (ControlStrongFillColorDefault:
        # bianco al 55% nel tema scuro, nero al 45% nel chiaro, sopra il fondo della riga)
        Select-Element (Find-PanelRow $h 'npx-test-1')
        Wait-Until { (Get-SelectedPanel $h) -eq 'npx-test-1' } | Out-Null
        Start-Sleep -Milliseconds 500
        $r1 = (Find-PanelRow $h 'npx-test-1').Current.BoundingRectangle
        $y1 = [int]($r1.Y + $r1.Height / 2)
        $c = Get-ScreenColor $px $y1
        $expected = Blend $panel $veil $alpha.NeutralSelected
        Check 'Scheda attiva senza colore: velo neutro più deciso' (Test-SameColor $c $expected 3) ("{0}, atteso {1}" -f (Format-Color $c), (Format-Color $expected))
        $panelTextSelected = Get-RowTextColor $h 'npx-test-1' $dark
        # (solo la fascia della stanghetta, prima del titolo: i bordi sfumati del testo sono grigi anche loro)
        $zone = @(Get-ScreenRow ([int]$r1.X) $y1 ([int]($r1.Height * 0.32)))
        $saturated = @($zone | Where-Object { ([math]::Max($_.R, [math]::Max($_.G, $_.B)) - [math]::Min($_.R, [math]::Min($_.G, $_.B))) -gt 8 })
        $pill = Blend $expected $veil $(if ($dark) { 0x8B } else { 0x72 })
        $found = @($zone | Where-Object { Test-SameColor $_ $pill 4 })
        Check 'Scheda attiva senza colore: stanghetta grigia neutra' ($found.Count -gt 0 -and $saturated.Count -eq 0) ("attesa {0}, trovati {1} pixel; pixel colorati: {2}" -f (Format-Color $pill), $found.Count, $saturated.Count)
        $c = Get-ScreenColor $px $py
        Check 'La riga lasciata torna come prima' (Test-SameColor $c (Blend $panel $red $alpha.Colored) 3) (Format-Color $c)
    }

    # Da compresso compaiono le icone (iniziali), quella della scheda colorata nel suo colore
    Invoke-PanelToggle $h $true
    Wait-Stable { (Get-IniValue 'Collapsed') -eq '1' -and [math]::Abs((Get-PanelRect $h).W - 56 * $scale) -le 3 } | Out-Null
    Check 'Da compresso compaiono le icone' ((Test-RowHasInitial $h 'npx-test-3') -and (Test-RowHasInitial $h 'Senza titolo'))
    if ($screen) { Check 'Da compresso l''icona della scheda colorata è nel suo colore' (Test-ColorInRow $h 'npx-test-2' $red) }
    Save-WindowShot $h (Join-Path $ShotsDir 'compresso-colore.png')
    Invoke-PanelToggle $h $false
    Wait-Stable { (Get-IniValue 'Collapsed') -eq '0' -and [math]::Abs((Get-PanelRect $h).W - [double](Get-IniValue 'Width') * $scale) -le 3 } | Out-Null
    Check 'Riespanso: di nuovo senza icone' (-not (Test-RowHasInitial $h 'npx-test-3'))

    Open-ColorMenu $h 'npx-test-2' | Out-Null
    Invoke-Any (Find-InProcess 'Nessuno')
    Check '"Nessuno" toglie il colore' (Wait-Until { -not (Find-PanelRow $h 'npx-test-2').Current.ItemStatus })
    if ($screen) { Check 'Riga di nuovo senza colore' ($hovered -or (Wait-Until { Test-SameColor (Get-ScreenColor $px $py) $bg 3 })) (Format-Color (Get-ScreenColor $px $py)) }
    Check 'Colore tolto da tab-colors.ini' (@(Get-Content $colorsFile -ErrorAction SilentlyContinue | Where-Object { $_ -match '=red$' }).Count -eq $redBefore)
    # Il colore è legato al TabId: deve essere l'identificativo della sessione (TabState\<id>.bin)
    $ids = @(Get-ModLog 400 'riga creata per l''indice' -Since $script:NpxTestStart | ForEach-Object { if ($_ -match 'TabId \{?([0-9a-fA-F-]{36})\}?') { $Matches[1].ToUpperInvariant() } } | Sort-Object -Unique)
    $states = @(Get-ChildItem (Join-Path $NpxLocalState 'TabState') -Filter '*.bin' | Where-Object { $_.CreationTime -ge $script:NpxTestStart.AddSeconds(-2) } |
            ForEach-Object { ($_.Name -split '\.')[0].ToUpperInvariant() } | Sort-Object -Unique)
    # Ogni stato salvato delle schede di prova aperte corrisponde al TabId di una riga del pannello
    # (le schede create e poi chiuse dal test non hanno più uno stato e non contano)
    Check 'TabId = identificativo della sessione (colori persistenti)' ($states.Count -ge 6 -and @($states | Where-Object { $ids -notcontains $_ }).Count -eq 0) ("{0} stati, tutti tra i {1} TabId" -f $states.Count, $ids.Count)

    # --- Menu contestuale nativo (via UI Automation) -------------------------------------------
    Show-ContextMenu $h 'npx-test-3'
    $names = @(Get-MenuItems | ForEach-Object { $_.Current.Name })
    $wanted = 'Nuova scheda', 'Chiudi scheda', 'Chiudi altre schede', 'Chiudi schede a destra'
    Check 'Menu contestuale nativo con le 4 voci' (@($wanted | Where-Object { $names -contains $_ }).Count -eq 4)
    Invoke-MenuItem 'Chiudi schede a destra'
    Check '"Chiudi schede a destra" agisce sulla scheda giusta' (Wait-Until { (Join-Order (Get-NativeOrder $h)) -eq 'Senza titolo,npx-test-1,npx-test-2,npx-test-3' }) (Join-Order (Get-NativeOrder $h))
    Check 'Pannello aggiornato dopo la chiusura' ((Join-Order (Get-PanelOrder $h)) -eq (Join-Order (Get-NativeOrder $h)))
    Check 'Striscia nativa di nuovo fuori vista' ((@(Get-OpenPopups).Count -eq 0) -and (Test-NativeStripHidden $h))

    Show-ContextMenu $h 'npx-test-1'
    Invoke-MenuItem 'Nuova scheda'
    Check '"Nuova scheda" inserisce accanto alla scheda' (Wait-Until { (Join-Order (Get-NativeOrder $h)) -eq 'Senza titolo,npx-test-1,Senza titolo,npx-test-2,npx-test-3' }) (Join-Order (Get-NativeOrder $h))
    $blank = @(Get-PanelRows $h)[2]
    Invoke-Element (Find-ByName $blank 'Chiudi scheda')   # X della riga (visibile: è la scheda attiva)
    Check 'X della riga chiude la scheda' (Wait-Until { (Join-Order (Get-NativeOrder $h)) -eq 'Senza titolo,npx-test-1,npx-test-2,npx-test-3' })
    Reopen $h 4, 5

    if ($RealInput) {
        Write-Host 'Input reale: il test si interrompe se il mouse viene mosso.' -ForegroundColor Yellow
        Reset-CursorWatch

        # --- Fascia del titolo in cima alla colonna: trascina la finestra, doppio clic la ingrandisce ----
        $wr = Get-WinRect $h; $pr = Get-PanelRect $h
        $bx = $pr.X + [int]($pr.W / 2); $by = $pr.Y + [int](16 * $panelScale)
        Assert-OnWindow $h $bx $by
        Drag $bx $by ($bx + 80) ($by + 40) 15 25
        Start-Sleep -Milliseconds 500
        $wr2 = Get-WinRect $h
        Check 'Trascinando la fascia in cima alla colonna la finestra si sposta' ([math]::Abs(($wr2.Left - $wr.Left) - 80) -le 4 -and [math]::Abs(($wr2.Top - $wr.Top) - 40) -le 4) "spostata di $($wr2.Left - $wr.Left),$($wr2.Top - $wr.Top)"
        Reset-CursorWatch
        $pr = Get-PanelRect $h
        $bx = $pr.X + [int]($pr.W / 2); $by = $pr.Y + [int](16 * $panelScale)
        Assert-OnWindow $h $bx $by
        Drag $bx $by ($bx - 80) ($by - 40) 15 25
        Start-Sleep -Milliseconds 500
        Reset-CursorWatch
        $pr = Get-PanelRect $h
        $bx = $pr.X + [int]($pr.W / 2); $by = $pr.Y + [int](16 * $panelScale)
        Assert-OnWindow $h $bx $by
        Double-Click-At $bx $by
        Check 'Doppio clic sulla fascia: finestra ingrandita' (Wait-Until { [NpxGeo]::IsZoomed([IntPtr]$h) } 3000)
        Start-Sleep -Milliseconds 800
        Reset-CursorWatch
        $pr = Get-PanelRect $h
        $o = Get-ClientOrigin $h
        Check '  ingrandita: colonna sempre in cima' ($pr.Y -eq $o.Y) "colonna da y=$($pr.Y), finestra da y=$($o.Y)"
        $bx = $pr.X + [int]($pr.W / 2); $by = $pr.Y + [int](16 * $panelScale)
        Assert-OnWindow $h $bx $by
        Double-Click-At $bx $by
        Check '  di nuovo: torna com''era' (Wait-Until { -not [NpxGeo]::IsZoomed([IntPtr]$h) } 3000)
        Start-Sleep -Milliseconds 800
        Reset-CursorWatch

        # --- Riordino con trascinamento ---------------------------------------------------------
        $x1, $y1 = Row-Point $h 'npx-test-5'
        $x2, $y2 = Row-Point $h 'npx-test-1' 0.3
        Assert-OnWindow $h $x1 $y1; Assert-OnWindow $h $x2 $y2
        Drag $x1 $y1 $x2 $y2 30 20
        $order = 'Senza titolo,npx-test-5,npx-test-1,npx-test-2,npx-test-3,npx-test-4'
        Check 'Trascinamento verso l''alto riordina le schede native' (Wait-Until { (Join-Order (Get-NativeOrder $h)) -eq $order }) (Join-Order (Get-NativeOrder $h))
        Check 'Pannello nello stesso ordine' ((Join-Order (Get-PanelOrder $h)) -eq $order)
        Check 'Nuovo ordine salvato nella sessione del Blocco note' (Wait-Until { @(Get-SavedOrders | Where-Object { $_.Order -eq $order }).Count -ge 1 } 6000)

        $x1, $y1 = Row-Point $h 'npx-test-1'
        $x2, $y2 = Row-Point $h 'npx-test-4' 0.75
        Assert-OnWindow $h $x1 $y1; Assert-OnWindow $h $x2 $y2
        Drag $x1 $y1 $x2 $y2 30 20
        $order = 'Senza titolo,npx-test-5,npx-test-2,npx-test-3,npx-test-4,npx-test-1'
        Check 'Trascinamento verso il basso' (Wait-Until { (Join-Order (Get-NativeOrder $h)) -eq $order -and (Join-Order (Get-PanelOrder $h)) -eq $order }) (Join-Order (Get-NativeOrder $h))
        Check 'Nessuna ricostruzione d''emergenza dell''elenco' (@(Get-ModLog 50 'rifiutat|ricostruit').Count -eq 0)

        # Anche a pannello compresso (righe con la sola icona)
        Invoke-PanelToggle $h $true
        Wait-Until { (Get-IniValue 'Collapsed') -eq '1' } | Out-Null
        Start-Sleep -Milliseconds 600
        Reset-CursorWatch
        $x1, $y1 = Row-Point $h 'npx-test-1'
        $x2, $y2 = Row-Point $h 'npx-test-4' 0.25
        Assert-OnWindow $h $x1 $y1; Assert-OnWindow $h $x2 $y2
        Drag $x1 $y1 $x2 $y2 20 20
        $order = 'Senza titolo,npx-test-5,npx-test-2,npx-test-3,npx-test-1,npx-test-4'
        Check 'Trascinamento a pannello compresso' (Wait-Until { (Join-Order (Get-NativeOrder $h)) -eq $order -and (Join-Order (Get-PanelOrder $h)) -eq $order }) (Join-Order (Get-NativeOrder $h))
        Invoke-PanelToggle $h $false
        Wait-Until { (Get-IniValue 'Collapsed') -eq '0' } | Out-Null
        Start-Sleep -Milliseconds 600

        Show-ContextMenu $h 'npx-test-3'
        Invoke-MenuItem 'Chiudi schede a destra'
        Check 'Il Blocco note usa il nuovo ordine ("a destra" = sotto)' (Wait-Until { (Join-Order (Get-NativeOrder $h)) -eq 'Senza titolo,npx-test-5,npx-test-2,npx-test-3' }) (Join-Order (Get-NativeOrder $h))
        Reopen $h 1, 4
        Reset-CursorWatch

        # --- Clic destro sotto il cursore -------------------------------------------------------
        $cx, $cy = Row-Point $h 'npx-test-2'
        $cx += 20
        Assert-OnWindow $h $cx $cy
        Click-At $cx $cy 'Right'
        Start-Sleep -Milliseconds 900
        $first = Get-MenuItems | Where-Object { $_.Current.Name -eq 'Nuova scheda' } | Select-Object -First 1
        $b = if ($first) { $first.Current.BoundingRectangle } else { $null }
        Check 'Clic destro: menu nativo sotto il cursore' ($b -and [math]::Abs($b.X - $cx) -le 25 -and [math]::Abs($b.Y - $cy) -le 25) $(if ($b) { "cursore $cx,$cy - prima voce $([int]$b.X),$([int]$b.Y)" })
        Save-WindowShot $h (Join-Path $ShotsDir 'menu-contestuale.png')
        Invoke-MenuItem 'Chiudi scheda'
        Check 'Clic destro + "Chiudi scheda" chiude quella riga' (Wait-Until { (Get-NativeOrder $h) -notcontains 'npx-test-2' })
        Reset-CursorWatch

        # --- Divisore ---------------------------------------------------------------------------
        Set-TestOpt 'VerticalTabs' 'Width' '240'
        Start-Sleep -Seconds 2
        $pr = Get-PanelRect $h
        $scale = $pr.W / 240.0
        $sx = $pr.X + $pr.W - 3; $sy = $pr.Y + [int]($pr.H / 2)
        Assert-OnWindow $h $sx $sy
        Move-To ($sx - 60) $sy; Start-Sleep -Milliseconds 300
        $shapeList = Get-CursorShape
        Move-To $sx $sy; Start-Sleep -Milliseconds 400
        $shapeSplitter = Get-CursorShape
        Check 'Cursore: freccia sull''elenco, ridimensiona sul divisore' ($shapeList -eq 'ARROW' -and $shapeSplitter -eq 'SIZEWE') "$shapeList / $shapeSplitter"
        Save-WindowShot $h (Join-Path $ShotsDir 'divisore.png')
        Drag $sx $sy ($sx + 100) $sy 20 20
        Start-Sleep -Milliseconds 800
        $pr2 = Get-PanelRect $h; $er = Get-EditorRect $h
        Check 'Trascinamento del divisore: +100 px' ([math]::Abs($pr2.W - $pr.W - 100) -le 2 -and [math]::Abs($er.X - ($pr2.X + $pr2.W)) -le 2) "pannello $($pr.W) -> $($pr2.W) px, editor x=$($er.X)"
        $width = [int](Get-IniValue 'Width')
        Check 'Larghezza salvata in settings.ini' ([math]::Abs($width - [math]::Round($pr2.W / $scale)) -le 1) "Width=$width"
        Double-Click-At ($pr2.X + $pr2.W - 3) $sy
        Start-Sleep -Milliseconds 900
        Check 'Doppio clic: larghezza predefinita' ((Get-IniValue 'Width') -eq '240' -and [math]::Abs((Get-PanelRect $h).W - $pr.W) -le 2) "Width=$(Get-IniValue 'Width')"
        Reset-CursorWatch

        # --- Scorrimento automatico con molte schede -------------------------------------------
        $add = Find-Control $h 'Nuova scheda' $CT::Button
        1..20 | ForEach-Object { Invoke-Element $add; Start-Sleep -Milliseconds 300 }
        Select-Element (Find-PanelRow $h 'npx-test-5')
        Start-Sleep -Milliseconds 800
        $lr = (Get-PanelList $h).Current.BoundingRectangle
        $x1, $y1 = Row-Point $h 'npx-test-5'
        $yEdge = [int]($lr.Y + $lr.Height - 12)
        Assert-OnWindow $h $x1 $y1; Assert-OnWindow $h $x1 $yEdge
        Drag $x1 $y1 $x1 $yEdge 25 15 2500
        # Con molte schede UI Automation vede solo quelle realizzate dalla striscia nativa: l'ordine
        # completo si legge dalla sessione salvata dal Blocco note (25 schede in questa finestra).
        $savedOk = Wait-Until { @(Get-SavedOrders | Where-Object { $_.Order.Split(',').Count -eq 25 -and $_.Order.EndsWith(',npx-test-5') }).Count -ge 1 } 6000
        $saved = (Get-SavedOrders | Where-Object { $_.Order.Split(',').Count -eq 25 } | Sort-Object { -not $_.Order.EndsWith(',npx-test-5') } | Select-Object -First 1).Order
        Check 'Scorrimento automatico: riga portata in fondo' $savedOk ("ultima: {0}" -f $(if ($saved) { $saved.Split(',')[-1] } else { '?' }))
        $panel = Join-Order (Get-PanelOrder $h)
        Check 'Pannello coerente con l''ordine salvato' ($saved -and (",$saved,").Contains(",$panel,")) ("{0} righe esposte" -f @(Get-PanelOrder $h).Count)
        Reset-CursorWatch

        # --- Rotella orizzontale (due dita sul touchpad): l'elenco non deve scorrere ----------------
        $list = Get-PanelList $h
        $scroll = $list.GetCurrentPattern([Windows.Automation.ScrollPattern]::Pattern)
        $scroll.SetScrollPercent(-1, 50)
        Start-Sleep -Milliseconds 600
        $before = $scroll.Current.VerticalScrollPercent
        $lr = $list.Current.BoundingRectangle
        $wx = [int]($lr.X + $lr.Width / 2); $wy = [int]($lr.Y + $lr.Height / 2)
        Assert-OnWindow $h $wx $wy
        Move-To $wx $wy 12
        Start-Sleep -Milliseconds 300
        Send-Wheel 120 3 -Horizontal
        Send-Wheel -120 3 -Horizontal
        Start-Sleep -Milliseconds 800
        Check 'Rotella orizzontale: l''elenco non scorre' ([math]::Abs($scroll.Current.VerticalScrollPercent - $before) -lt 0.5) ("{0:N1}% -> {1:N1}%" -f $before, $scroll.Current.VerticalScrollPercent)
        Send-Wheel 120 2
        Start-Sleep -Milliseconds 800
        Check 'Rotella verticale: l''elenco scorre' ($scroll.Current.VerticalScrollPercent -lt $before - 1) ("{0:N1}% -> {1:N1}%" -f $before, $scroll.Current.VerticalScrollPercent)
        Reset-CursorWatch
    }

    # --- Editor: la superficie di testo segue il contenitore ristretto -----------------------------
    # (altrimenti il testo, a capo automatico compreso, finisce oltre il bordo visibile, sotto la
    # barra di scorrimento)
    Set-Content -LiteralPath (Join-Path $NpxTestDir "${NpxTestPrefix}lungo.txt") -Value ((('parola ' * 80) + 'FINE')) -Encoding UTF8
    Open-FileInWindow $h (Join-Path $NpxTestDir "${NpxTestPrefix}lungo.txt")
    Start-Sleep -Milliseconds 900
    Check 'Editor: superficie di testo larga quanto il contenitore' (Wait-Until { (Get-EditorSurface $h).Overhang -le 0 }) ("sporge di {0} px" -f (Get-EditorSurface $h).Overhang)
    # A capo automatico (nel Blocco note vale per scheda): attivato solo nella scheda di prova
    if (Enable-WordWrap $h) {
        $lines = Get-EditorTextRects $h
        $editor = Get-EditorSurface $h
        $right = ($lines | ForEach-Object { $_.X + $_.Width } | Measure-Object -Maximum).Maximum
        $editorRight = $editor.Container.X + $editor.Container.W
        Check 'A capo automatico: il testo va a capo dentro l''editor' ($right -le $editorRight) ("{0} righe, testo fino a {1}, editor fino a {2}" -f $lines.Count, [int]$right, $editorRight)
    }
    else { Check 'A capo automatico attivabile nella scheda di prova' $false }
    Invoke-PanelToggle $h $true
    Wait-Stable { (Get-IniValue 'Collapsed') -eq '1' } | Out-Null
    Check 'Editor da compresso: superficie larga quanto il contenitore' (Wait-Until { $e = Get-EditorSurface $h; $e.Overhang -le 0 -and $e.Container.W -eq $e.Surface.W }) ("contenitore {0}, superficie {1}" -f (Get-EditorSurface $h).Container.W, (Get-EditorSurface $h).Surface.W)
    Invoke-PanelToggle $h $false
    Wait-Stable { (Get-IniValue 'Collapsed') -eq '0' } | Out-Null
    Select-Element (Find-PanelRow $h 'npx-test-1')
    Check 'Editor dopo un cambio di scheda: superficie larga quanto il contenitore' (Wait-Until { (Get-SelectedNative $h) -eq 'npx-test-1' -and (Get-EditorSurface $h).Overhang -le 0 })

    # --- Barra di ricerca (Ctrl+F): intera e centrata sull'editor ------------------------------------
    # Il Blocco note la mette in un'isola XAML sospesa in cima all'editor e ne impagina il contenuto per la misura che le
    # dà: va spostata con l'editor, non ristretta dal bordo sinistro né allungata verso l'alto come lui (restava tagliata
    # ai due lati: mancavano la freccia di "Sostituisci", l'inizio della casella, le opzioni e la chiusura).
    $script:fb = $null
    try { Invoke-MenuCommand $h 'Modifica' 'Trova' } catch { }
    $okFind = Wait-Until { $script:fb = Get-FindBarState $h; $script:fb -and $script:fb.Controls -ge 5 }
    Check 'Barra di ricerca (Ctrl+F): intera, centrata sull''editor, dentro l''editor' ($okFind -and $fb.Outside.Count -eq 0 -and [math]::Abs($fb.CenterDelta) -le 2 -and $fb.Island.X -ge $fb.Editor.X -and $fb.Island.Y -eq $fb.Editor.Y) (Format-FindBar $fb)
    $okReplace = (Invoke-FindBarButton $h '*sostituzione*') -and (Wait-Until { $script:fb = Get-FindBarState $h; $script:fb -and $script:fb.Controls -ge 8 })
    Check 'Barra di ricerca in modalità Sostituisci (più alta): intera' ($okReplace -and $fb.Outside.Count -eq 0 -and [math]::Abs($fb.CenterDelta) -le 2) (Format-FindBar $fb)
    Invoke-PanelToggle $h $true
    Wait-Stable { (Get-IniValue 'Collapsed') -eq '1' } | Out-Null
    $okCollapsed = Wait-Until { $script:fb = Get-FindBarState $h; $script:fb -and [math]::Abs($script:fb.CenterDelta) -le 2 -and $script:fb.Outside.Count -eq 0 }
    Check 'Barra di ricerca con il pannello compresso: di nuovo centrata, intera' $okCollapsed (Format-FindBar $fb)
    Invoke-PanelToggle $h $false
    Wait-Stable { (Get-IniValue 'Collapsed') -eq '0' } | Out-Null
    # Finestra stretta: la barra (larga circa 2/3 dell'editor del Blocco note, che non sa della colonna) non sta nell'editor:
    # resta intera e sporge sulla colonna, con il pannello sotto di lei; l'isola resta sotto l'editor nell'ordine Z (portata
    # in cima, l'editor non disegnava più sotto di lei e nei margini trasparenti si vedeva il nero)
    $w0 = Get-WinRect $h
    $narrow = [int](480 * [NpxGeo]::GetDpiForWindow([IntPtr]$h) / 96) # a 600 DIP la barra con una sola riga stava ancora nell'editor
    [void][NpxPos]::SetWindowPos([IntPtr]$h, [IntPtr]::Zero, $w0.Left, $w0.Top, $narrow, ($w0.Bottom - $w0.Top), 0x0014)
    $okNarrow = Wait-Until { $script:fb = Get-FindBarState $h; $script:fb -and $script:fb.Island.W -gt $script:fb.Editor.W -and $script:fb.Outside.Count -eq 0 }
    Start-Sleep -Milliseconds 800
    $colors = Get-EditorTopColors $h
    Check 'Finestra stretta: barra di ricerca intera, sopra la colonna' ($okNarrow -and $fb.Island.X -lt $fb.Editor.X -and $fb.Island.X -ge $w0.Left) (Format-FindBar $fb)
    Check '  in cima all''editor il suo colore, niente fascia nera' ($colors.Top.ToArgb() -eq $colors.Middle.ToArgb() -and ($colors.Top.R + $colors.Top.G + $colors.Top.B) -gt 0) ("cima #{0:X2}{1:X2}{2:X2}, metà #{3:X2}{4:X2}{5:X2}" -f $colors.Top.R, $colors.Top.G, $colors.Top.B, $colors.Middle.R, $colors.Middle.G, $colors.Middle.B)
    [void][NpxPos]::SetWindowPos([IntPtr]$h, [IntPtr]::Zero, $w0.Left, $w0.Top, ($w0.Right - $w0.Left), ($w0.Bottom - $w0.Top), 0x0014)
    Wait-Until { $script:fb = Get-FindBarState $h; $script:fb -and [math]::Abs($script:fb.CenterDelta) -le 2 } | Out-Null
    [void](Invoke-FindBarButton $h 'Esci da Trova*')
    Check 'Barra di ricerca chiusa' (Wait-Until { -not (Get-FindBarIsland $h) })

    # --- Pulsante "Torna su" ---------------------------------------------------------------------
    # Documento lungo: le righe iniziano con il loro numero (001, 002...), così la prima riga visibile
    # dice dove si trova l'editor. Lo scorrimento si fa con i messaggi all'editor (come la tastiera).
    Set-IniSectionValue 'BackToTop' 'Enabled' '1'
    $scorri = Join-Path $NpxTestDir "${NpxTestPrefix}scorri.txt"
    Set-Content -LiteralPath $scorri -Value (1..400 | ForEach-Object { '{0:D3} riga di prova per il pulsante Torna su' -f $_ }) -Encoding UTF8
    Open-FileInWindow $h $scorri
    Start-Sleep -Milliseconds 900
    Check 'Torna su: in cima al documento il pulsante non c''è' ((Wait-Until { (Get-FirstVisibleText $h 3) -eq '001' }) -and -not (Test-BackToTopShown $h)) (Get-FirstVisibleText $h 3)
    # L'isola del pulsante resta mostrata ("parcheggiata") anche a pulsante nascosto, ma trasparente al mouse: mostrarla al
    # primo scorrimento faceva generare a Windows un movimento del mouse per l'editor, e il Blocco note passava la barra di
    # scorrimento all'indicatore "da mouse" (cursore più corto e più in basso) proprio mentre si scorreva.
    $parked = Wait-Until { $i = Get-BackToTopIsland $h; $i -and $i.Visible -and (Test-IslandClickThrough $i.Hwnd) }
    $pi = Get-BackToTopIsland $h
    $underText = if ($pi) { $u = Get-WindowAtScreen ($pi.X + [int]($pi.W / 2)) ($pi.Y + [int]($pi.H / 2)); $sb = New-Object System.Text.StringBuilder 128; [void][NpxKit]::GetClassName([IntPtr]$u, $sb, 128); $sb.ToString() } else { '' }
    Check 'Torna su: a pulsante nascosto l''isola resta mostrata ma il mouse va al testo sotto' ($parked -and $underText -eq 'RichEditD2DPT') ("isola mostrata {0}, trasparente {1}, sotto il centro {2}" -f $(if ($pi) { $pi.Visible } else { 'no' }), $(if ($pi) { Test-IslandClickThrough $pi.Hwnd } else { '-' }), $underText)
    $parkedHwnd = if ($pi) { $pi.Hwnd } else { 0 }
    Send-EditorPageDown $h 3
    Check 'Torna su: compare appena il documento scorre' (Wait-Until { Test-BackToTopShown $h }) ("prima riga {0}" -f (Get-FirstVisibleText $h 3))
    Check '  senza mostrare finestre (stessa isola, già visibile)' ($parkedHwnd -ne 0 -and (Get-BackToTopIsland $h).Hwnd -eq $parkedHwnd)
    $isl = Get-BackToTopIsland $h; $ed = (Get-EditorSurface $h).Container
    $inCorner = { param($i, $e) ($i.X + $i.W) -le ($e.X + $e.W) -and ($i.X + $i.W) -ge ($e.X + $e.W - 80) -and ($i.Y + $i.H) -le ($e.Y + $e.H) -and ($i.Y + $i.H) -ge ($e.Y + $e.H - 100) }
    Check 'Torna su: nell''angolo in basso a destra dell''editor' (& $inCorner $isl $ed) ("pulsante {0},{1} {2}x{3}, editor fino a {4},{5}" -f $isl.X, $isl.Y, $isl.W, $isl.H, ($ed.X + $ed.W), ($ed.Y + $ed.H))
    $button = Get-BackToTopButton $h
    Check 'Torna su: pulsante accessibile con nome' ($button -and $button.Current.Name -eq 'Torna su')
    $from = Get-FirstVisibleText $h 3
    Invoke-Element $button
    Check 'Torna su: il clic riporta all''inizio del documento' (Wait-Until { (Get-FirstVisibleText $h 3) -eq '001' }) ("da {0} a {1}" -f $from, (Get-FirstVisibleText $h 3))
    Check 'Torna su: in cima il pulsante sparisce' (Wait-Until { -not (Test-BackToTopShown $h) })
    Check 'Torna su: il focus torna all''editor' (Wait-Until { Test-EditorHasFocus $h })
    # La posizione è per documento: cambiando scheda il pulsante segue quella visibile
    Send-EditorPageDown $h 2
    Wait-Until { Test-BackToTopShown $h } | Out-Null
    Select-Element (Find-PanelRow $h 'npx-test-1')
    Check 'Torna su: su una scheda in cima al documento sparisce' (Wait-Until { (Get-SelectedNative $h) -eq 'npx-test-1' -and -not (Test-BackToTopShown $h) })
    Select-Element (Find-PanelRow $h "${NpxTestPrefix}scorri")
    Check 'Torna su: tornando sulla scheda scorsa ricompare' (Wait-Until { (Get-SelectedNative $h) -eq "${NpxTestPrefix}scorri" -and (Test-BackToTopShown $h) })
    # Barra di scorrimento verticale: con la riga del titolo nascosta la sua isola sale con l'editor, ma il Blocco note fissa
    # l'altezza del suo ScrollViewer sull'isola non allungata e la reimposta a ogni ricalcolo del layout. La barra deve
    # arrivare comunque in fondo (prima in fondo restavano vuoti i px dell'editor salito), anche dopo un ridimensionamento
    $fitOk = {
        $v = Get-VScrollBar $h
        $v -and $null -ne $v.Top -and [math]::Abs($v.Island.Y - $v.Editor.Y) -le 1 -and [math]::Abs($v.Top - $v.Island.Y) -le 1 -and
            [math]::Abs($v.Bottom - ($v.Island.Y + $v.Island.H)) -le 1
    }
    $fitText = { $v = Get-VScrollBar $h; if ($v) { "editor da y={0}, isola y={1}-{2}, barra y={3}-{4}" -f $v.Editor.Y, $v.Island.Y, ($v.Island.Y + $v.Island.H), $v.Top, $v.Bottom } else { 'barra non trovata' } }
    Check 'Barra di scorrimento verticale dalla cima dell''editor salito fino in fondo' (Wait-Until $fitOk) (& $fitText)
    $wr = Get-WinRect $h
    [void][NpxPos]::SetWindowPos([IntPtr]$h, [IntPtr]::Zero, $wr.Left, $wr.Top, $wr.Right - $wr.Left, $wr.Bottom - $wr.Top - 90, 0x0014) # NOZORDER|NOACTIVATE
    $shrunk = Wait-Until { (Get-WinRect $h).Bottom -eq $wr.Bottom - 90 -and (& $fitOk) }
    $shrunkText = & $fitText
    [void][NpxPos]::SetWindowPos([IntPtr]$h, [IntPtr]::Zero, $wr.Left, $wr.Top, $wr.Right - $wr.Left, $wr.Bottom - $wr.Top, 0x0014)
    $restored = Wait-Until { (Get-WinRect $h).Bottom -eq $wr.Bottom -and (& $fitOk) }
    Check '  anche dopo un ridimensionamento della finestra (il Blocco note la reimposta)' ($shrunk -and $restored) ("ridotta: {0}; ripristinata: {1}" -f $shrunkText, (& $fitText))
    # Il Blocco note traduce la posizione della barra in quella del testo sulla corsa (contenuto - vista): allungando solo la
    # vista, trascinando il cursore fino in fondo il testo si fermava prima della fine (ultime righe irraggiungibili)
    $vsv = Get-ScrollBarViewer $h
    if ($vsv) {
        Start-Sleep -Milliseconds 800
        Invoke-ScrollDrag $vsv.GetCurrentPattern([Windows.Automation.ScrollPattern]::Pattern) 100
        $atEnd = Wait-Until { $s = Get-EditorScroll $h; $s -and $s.Pos -ge $s.End - 1 }
        $s = Get-EditorScroll $h
        Check '  trascinata fino in fondo: il testo arriva alla fine' $atEnd ("testo {0} su {1}" -f $s.Pos, $s.End)
    }
    else { Check '  trascinata fino in fondo: il testo arriva alla fine' $false 'barra non trovata' }
    # Barre di scorrimento subito larghe sotto il mouse (ScrollBarHover, sempre attiva): il modello di WinUI le allarga solo
    # dopo 0,4 s. Senza mouse vero si controlla dal registro della mod che le abbia adattate nella finestra di prova (che
    # uscendo dalla mod tornino come prima e rientrando si riadattino si vede più avanti, con il gate di sviluppo); con
    # -RealInput si misura anche l'espansione sotto il cursore.
    $testTid = [NpxKit]::GetWindowThreadProcessId([IntPtr]$h, [ref]0)
    $hoverLog = { param([string]$Pattern, $Since) @(Get-ModLog 2000 $Pattern -Since $Since | Where-Object { $_ -match ":\s*$testTid\]" }).Count -gt 0 }
    Check 'Barre di scorrimento subito larghe sotto il mouse: adattate nella finestra di prova' (& $hoverLog 'ScrollBarHover: (\d+ barre di scorrimento si allargano subito|altre \d+ barre adattate)' $NpxTestStart)
    if ($RealInput) {
        $fast = Measure-VScrollHover $h
        Check '  mouse vero: a 200 ms la barra è già larga' ($fast -and $fast.Early -and $fast.Late) $(if ($fast) { $fast.Detail } else { 'barra non trovata' })
    }
    # "Disattiva lo scorrimento fluido" (WheelScroll): uno scatto di rotella sul testo sposta il documento in un colpo solo
    # (le righe di Windows), invece dei ~10 piccoli passi in 0,3 s dell'editor; spenta l'opzione torna l'animazione.
    # Ctrl+rotella resta lo zoom del Blocco note (cambia l'altezza del documento in px).
    Set-IniSectionValue 'SmoothScroll' 'Disabled' '1'
    # Il documento è in fondo (prova precedente): a metà, così la rotella può andare nei due versi
    $wheelSurface = [IntPtr](Get-EditorScroll $h).Surface
    [void][NpxKit]::SendMessage($wheelSurface, 0x0115, [IntPtr]6, [IntPtr]::Zero) # SB_TOP
    foreach ($k in 1..2) { [void][NpxKit]::SendMessage($wheelSurface, 0x0115, [IntPtr]3, [IntPtr]::Zero) } # SB_PAGEDOWN
    Start-Sleep -Milliseconds 1500
    $wheel = Measure-WheelSteps $h
    Check 'Scorrimento fluido disattivato: uno scatto di rotella sposta il testo in un colpo solo' ($wheel -and $wheel.Changes -eq 1 -and $wheel.To -gt $wheel.From) ("{0} -> {1}, {2} cambi" -f $wheel.From, $wheel.To, $wheel.Changes)
    $wheelUp = Measure-WheelSteps $h 120
    Check '  anche verso l''alto' ($wheelUp -and $wheelUp.Changes -eq 1 -and $wheelUp.To -lt $wheelUp.From) ("{0} -> {1}, {2} cambi" -f $wheelUp.From, $wheelUp.To, $wheelUp.Changes)
    $zoomIn = Measure-WheelSteps $h 120 0x0008
    $zoomOut = Measure-WheelSteps $h -120 0x0008
    Check '  Ctrl+rotella resta lo zoom del Blocco note' ($zoomIn -and $zoomOut -and $zoomIn.End -gt $wheelUp.End -and $zoomOut.End -eq $wheelUp.End) ("altezza {0} -> {1} -> {2}" -f $wheelUp.End, $zoomIn.End, $zoomOut.End)
    Set-IniSectionValue 'SmoothScroll' 'Disabled' '0'
    Start-Sleep -Milliseconds 1500
    $smooth = Measure-WheelSteps $h
    Check '  opzione spenta: di nuovo lo scorrimento fluido del Blocco note' ($smooth -and $smooth.Changes -ge 3) ("{0} -> {1}, {2} cambi" -f $smooth.From, $smooth.To, $smooth.Changes)
    Set-IniSectionValue 'SmoothScroll' 'Disabled' '1'
    Start-Sleep -Milliseconds 1500
    $again = Measure-WheelSteps $h
    Check '  riattivata: di nuovo in un colpo solo' ($again -and $again.Changes -eq 1) ("{0} -> {1}, {2} cambi" -f $again.From, $again.To, $again.Changes)
    # In fondo al documento la rotella non porta il testo oltre la fine: EM_SETSCROLLPOS (usato quando le righe non si
    # muovono) la accettava, il testo andava nel vuoto e poi tornava indietro a scatti. Si guarda la posizione interna della
    # RichEdit: quella della barra (SCROLLINFO) Windows la tiene sempre dentro la corsa.
    $endProbe = New-RichScrollProbe $wheelSurface
    try {
        [void][NpxKit]::SendMessage($wheelSurface, 0x0115, [IntPtr]7, [IntPtr]::Zero) # SB_BOTTOM
        Start-Sleep -Milliseconds 500
        $yEnd = (Get-RichScrollPos $endProbe).Y
        $wr = Get-WinRect $wheelSurface
        $wl = [IntPtr]((([int](($wr.Top + $wr.Bottom) / 2) -band 0xFFFF) -shl 16) -bor ([int](($wr.Left + $wr.Right) / 2) -band 0xFFFF))
        foreach ($k in 1..3) { [void][NpxKit]::PostMessage($wheelSurface, 0x020A, [IntPtr]((-120 -band 0xFFFF) -shl 16), $wl); Start-Sleep -Milliseconds 60 }
        Start-Sleep -Milliseconds 700
        $yAfter = (Get-RichScrollPos $endProbe).Y
        Check '  in fondo al documento la rotella non porta il testo oltre la fine' ($null -ne $yEnd -and $yAfter -eq $yEnd) ("fine y={0}, dopo 3 scatti y={1}" -f $yEnd, $yAfter)
    }
    finally { Remove-RichScrollProbe $endProbe }
    # Il pulsante segue l'editor quando il pannello cambia larghezza (e il layout non lo sposta)
    Invoke-PanelToggle $h $true
    Wait-Stable { (Get-IniValue 'Collapsed') -eq '1' } | Out-Null
    Check 'Torna su: segue l''editor a pannello compresso' (Wait-Until { & $inCorner (Get-BackToTopIsland $h) (Get-EditorSurface $h).Container }) ("pulsante x={0}, editor fino a {1}" -f (Get-BackToTopIsland $h).X, ((Get-EditorSurface $h).Container.X + (Get-EditorSurface $h).Container.W))
    Invoke-PanelToggle $h $false
    Wait-Stable { (Get-IniValue 'Collapsed') -eq '0' } | Out-Null
    Check 'Torna su: segue l''editor a pannello espanso' (Wait-Until { & $inCorner (Get-BackToTopIsland $h) (Get-EditorSurface $h).Container })
    # Spento e riacceso dal menu della tray (settings.ini)
    Set-IniSectionValue 'BackToTop' 'Enabled' '0'
    Check 'Torna su spento: il pulsante sparisce' (Wait-Until { -not (Test-BackToTopShown $h) } 4000)
    Set-IniSectionValue 'BackToTop' 'Enabled' '1'
    Check 'Torna su riacceso: il pulsante ricompare' (Wait-Until { Test-BackToTopShown $h } 4000)
    Invoke-Element (Get-BackToTopButton $h)
    Wait-Until { -not (Test-BackToTopShown $h) } | Out-Null

    # --- Barra di scorrimento orizzontale ----------------------------------------------------------
    # Riga lunga e "A capo automatico" spento nella scheda di prova. L'isola della barra si accorcia della colonna, ma il
    # Blocco note dimensiona vista e contenuto della barra sul suo layout senza colonna: la barra sforava a destra della
    # larghezza della colonna (freccia destra e fine corsa tagliate fuori) e, corretta solo la vista, a testo tutto a
    # destra il cursore restava al 93%
    $largo = Join-Path $NpxTestDir "${NpxTestPrefix}largo.txt"
    Set-Content -LiteralPath $largo -Value (@('riga lunga ' + ((1..60 | ForEach-Object { "parola$_" }) -join ' ')) + @(1..30 | ForEach-Object { "riga $_" })) -Encoding UTF8
    Open-FileInWindow $h $largo
    Start-Sleep -Milliseconds 900
    $wrapSwitched = $false
    if (-not (Get-HScrollIsland $h)) { $wrapSwitched = Switch-WordWrap $h }
    $hShown = Wait-Until { $null -ne (Get-HScrollIsland $h) }
    Check 'Barra orizzontale presente (riga lunga, a capo automatico spento)' $hShown
    if ($hShown) {
        $hi = Get-HScrollIsland $h
        $hp = (Get-ScrollBarViewer $h -Horizontal).GetCurrentPattern([Windows.Automation.ScrollPattern]::Pattern)
        [void][NpxKit]::SendMessage([IntPtr](Get-EditorScroll $h -Horizontal).Surface, 0x0114, [IntPtr]7, [IntPtr]::Zero) # WM_HSCROLL SB_RIGHT
        $right = Wait-Until { $hp.Current.HorizontalScrollPercent -ge 99.5 }
        $s = Get-EditorScroll $h -Horizontal
        Check '  testo tutto a destra: cursore della barra in fondo' $right ("barra al {0:N1}%, testo {1} su {2}" -f $hp.Current.HorizontalScrollPercent, $s.Pos, $s.End)
        # Larghezza della vista della barra dalla proporzione vista/contenuto (UIA) e dalla corsa del testo (contenuto - vista
        # = corsa del testo): deve essere quella dell'isola
        $v = $hp.Current.HorizontalViewSize / 100.0
        $viewPx = if ($v -lt 1) { $v * $s.End / (1 - $v) } else { 0 }
        Check '  barra larga quanto la sua isola (non sfora a destra)' ([math]::Abs($viewPx - $hi.W) -le 3) ("vista {0:N0} px, isola {1} px" -f $viewPx, $hi.W)
        Start-Sleep -Milliseconds 1000
        Invoke-ScrollDrag $hp 50 -Horizontal -Steps 10
        $half = Wait-Until { $t = Get-EditorScroll $h -Horizontal; $t.End -gt 0 -and [math]::Abs(100.0 * $t.Pos / $t.End - 50) -le 2 }
        $t = Get-EditorScroll $h -Horizontal
        Check '  trascinata a metà: testo a metà' $half ("testo {0} su {1}" -f $t.Pos, $t.End)
        Invoke-ScrollDrag $hp 0 -Horizontal -Steps 10
    }
    if ($wrapSwitched) { Switch-WordWrap $h | Out-Null } # la scheda di prova torna com'era

    # --- Barra verticale aggiornata dopo l'apertura, senza scorrere (difetto del Blocco note, ScrollRangeFix) ----------
    # La RichEdit impagina in background e allunga la corsa senza ridisegnare; il Blocco note rilegge le corse solo a un
    # ridisegno (EN_UPDATE) e lasciava la barra con il cursore della corsa vecchia (45% invece del 17,6%): al primo scatto
    # di rotella il cursore cambiava di colpo grandezza. La mod gli manda lo stesso EN_UPDATE appena la corsa cambia.
    # (UI Automation non fa ridisegnare l'editor: senza la correzione la barra resta vecchia finché non si scorre.)
    $lungo = Join-Path $NpxTestDir "${NpxTestPrefix}lungo.txt"
    Set-Content -LiteralPath $lungo -Value (1..300 | ForEach-Object { "riga $_" }) -Encoding UTF8
    Open-FileInWindow $h $lungo
    $editorRange = {
        $box = Get-EditorRect $h
        if (-not $box -or -not $box.Hwnd) { return -1 } # editor della scheda nuova non ancora creato
        $surface = [NpxKit]::FindWindowEx([IntPtr]$box.Hwnd, [IntPtr]::Zero, 'RichEditD2DPT', [NullString]::Value)
        $v = New-Object NpxScroll+SCROLLINFO; $v.cbSize = 28; $v.fMask = 0x17; [void][NpxScroll]::GetScrollInfo($surface, 1, [ref]$v)
        if ($v.nPage -gt 0 -and $v.nMax -ge [int]$v.nPage) { $v.nMax - [int]$v.nPage + 1 } else { 0 }
    }
    $barInfo = {
        $expected = & $editorRange
        $vb = Get-VScrollBar $h
        $range = -1
        if ($vb -and $vb.Top) {
            $size = (Get-ScrollBarViewer $h).GetCurrentPattern([Windows.Automation.ScrollPattern]::Pattern).Current.VerticalViewSize
            if ($size -gt 0 -and $size -lt 100) { $range = ($vb.Bottom - $vb.Top) * (100 / $size - 1) }
        }
        [pscustomobject]@{ Expected = $expected; Bar = $range; Ok = $expected -gt 0 -and [math]::Abs($range - $expected) -le [math]::Max(6, 0.003 * $expected) }
    }
    # Prima la RichEdit deve aver finito di impaginare: corsa ferma per 0,6 s (subito dopo l'apertura è ancora provvisoria,
    # e lì la barra combacia anche senza la correzione)
    $sw = [Diagnostics.Stopwatch]::StartNew(); $lastRange = -1; $lastChange = 0; $settled = $false; $changes = 0
    while ($sw.ElapsedMilliseconds -lt 6000) {
        $e = & $editorRange
        if ($e -ne $lastRange) { $lastRange = $e; $lastChange = $sw.ElapsedMilliseconds; $changes++ }
        elseif ($e -gt 0 -and $sw.ElapsedMilliseconds - $lastChange -ge 600) { $settled = $true; break }
        Start-Sleep -Milliseconds 40
    }
    $barOk = $settled -and (Wait-Until { (& $barInfo).Ok } 1500)
    $bi = & $barInfo
    Check 'Documento appena aperto: la barra verticale ha già la corsa giusta, senza scorrere' $barOk ("barra {0:N0} px, editor {1} px (corsa cambiata {2} volte{3})" -f $bi.Bar, $bi.Expected, $changes, $(if ($settled) { '' } else { ', mai ferma' }))

    # --- Corsa verticale fasulla dell'editor (difetto del Blocco note, ScrollRangeFix) ----------------------------
    # Righe più larghe della vista ("a capo automatico" spento): quando il documento passa dal "non ci sta in altezza" al
    # "ci sta" (qui: finestra più alta) la RichEdit mette come massimo verticale quello orizzontale, e la barra XAML mostra
    # una corsa che non esiste (rotella e trascinamento non muovono nulla). La mod la azzera; riaccorciando torna la vera.
    $fasullo = Join-Path $NpxTestDir "${NpxTestPrefix}fasullo.txt"
    Set-Content -LiteralPath $fasullo -Value (1..60 | ForEach-Object { "riga $_ " + ('abcdefghij' * 19) }) -Encoding UTF8
    Open-FileInWindow $h $fasullo
    Start-Sleep -Milliseconds 900
    $wrapSwitched = $false
    if (-not (Get-HScrollIsland $h)) { $wrapSwitched = Switch-WordWrap $h }
    Start-Sleep -Milliseconds 900
    $vInfo = {
        $box = Get-EditorRect $h
        $surface = [NpxKit]::FindWindowEx([IntPtr]$box.Hwnd, [IntPtr]::Zero, 'RichEditD2DPT', [NullString]::Value)
        $v = New-Object NpxScroll+SCROLLINFO; $v.cbSize = 28; $v.fMask = 0x17; [void][NpxScroll]::GetScrollInfo($surface, 1, [ref]$v)
        $o = New-Object NpxScroll+SCROLLINFO; $o.cbSize = 28; $o.fMask = 0x17; [void][NpxScroll]::GetScrollInfo($surface, 0, [ref]$o)
        [pscustomobject]@{ VMax = $v.nMax; VPage = [int]$v.nPage; HMax = $o.nMax; Text = ("V max {0} pagina {1}, O max {2}" -f $v.nMax, $v.nPage, $o.nMax) }
    }
    $start = & $vInfo
    $wrFix = Get-WinRect $h
    # Più alta di quanto serve perché il documento ci stia (se già ci sta, basta il controllo)
    $grow = if ($start.VPage -gt 0 -and $start.VMax -ge $start.VPage) { $start.VMax - $start.VPage + 120 } else { 0 }
    if ($grow -gt 0) { [void][NpxPos]::SetWindowPos([IntPtr]$h, [IntPtr]::Zero, $wrFix.Left, $wrFix.Top, $wrFix.Right - $wrFix.Left, $wrFix.Bottom - $wrFix.Top + $grow, 0x0014) }
    $fixed = Wait-Until { $i = & $vInfo; $i.VPage -eq 0 -or $i.VMax -lt $i.VPage } 4000
    Check 'Documento a righe larghe che ci sta in altezza: nessuna corsa verticale fasulla' $fixed ("all'inizio {0}; finestra +{1} px: {2}; barra XAML {3}" -f $start.Text, $grow, (& $vInfo).Text, $(if (Get-ScrollBarViewer $h) { 'visibile' } else { 'nascosta' }))
    # E la rotella non lo porta nel vuoto (senza corsa le righe non si muovono; il ripiego a pixel andava oltre la fine)
    $fitsSurface = [NpxKit]::FindWindowEx([IntPtr](Get-EditorRect $h).Hwnd, [IntPtr]::Zero, 'RichEditD2DPT', [NullString]::Value)
    $fitsProbe = New-RichScrollProbe $fitsSurface
    try {
        $y0 = (Get-RichScrollPos $fitsProbe).Y
        $wr = Get-WinRect $fitsSurface
        $wl = [IntPtr]((([int](($wr.Top + $wr.Bottom) / 2) -band 0xFFFF) -shl 16) -bor ([int](($wr.Left + $wr.Right) / 2) -band 0xFFFF))
        foreach ($k in 1..3) { [void][NpxKit]::PostMessage($fitsSurface, 0x020A, [IntPtr]((-120 -band 0xFFFF) -shl 16), $wl); Start-Sleep -Milliseconds 60 }
        Start-Sleep -Milliseconds 700
        $y1 = (Get-RichScrollPos $fitsProbe).Y
        Check '  la rotella non lo sposta nel vuoto' ($null -ne $y0 -and $y1 -eq $y0) ("y {0} -> {1} dopo 3 scatti" -f $y0, $y1)
    }
    finally { Remove-RichScrollProbe $fitsProbe }
    if ($grow -gt 0) {
        [void][NpxPos]::SetWindowPos([IntPtr]$h, [IntPtr]::Zero, $wrFix.Left, $wrFix.Top, $wrFix.Right - $wrFix.Left, $wrFix.Bottom - $wrFix.Top, 0x0014)
        $back = Wait-Until { $i = & $vInfo; $i.VPage -gt 0 -and $i.VMax -ge $i.VPage -and $i.VMax -ne $i.HMax } 4000
        $surfaceFix = [IntPtr](Get-EditorScroll $h).Surface
        $p0 = (Get-EditorScroll $h).Pos; [void][NpxKit]::SendMessage($surfaceFix, 0x0115, [IntPtr]1, [IntPtr]::Zero); Start-Sleep -Milliseconds 300; $p1 = (Get-EditorScroll $h).Pos
        Check '  finestra di nuovo corta: torna la corsa vera e il testo scorre' ($back -and $p1 -gt $p0) ("{0}; SB_LINEDOWN {1} -> {2}" -f (& $vInfo).Text, $p0, $p1)
    }
    if ($wrapSwitched) { Switch-WordWrap $h | Out-Null }
    Select-Element (Find-PanelRow $h 'npx-test-1')
    Wait-Until { (Get-SelectedNative $h) -eq 'npx-test-1' } | Out-Null

    # --- Riapri la scheda chiusa (Ctrl+Maiusc+T) -------------------------------------------------
    # Senza premere tasti davvero: che la combinazione sia riservata si verifica provando a registrarla
    # dal test (mentre la tiene il Blocco note non ci riesce), e l'azione si chiede con --reopen-tab,
    # lo stesso comando della scorciatoia.
    Set-IniSectionValue 'ReopenTab' 'Enabled' '1'
    Start-Sleep -Milliseconds 800
    # Con -RealInput il clic destro ha chiuso npx-test-2, che qui serve: torna (in fondo all'elenco)
    if ((Get-PanelOrder $h) -notcontains 'npx-test-2') {
        Reopen $h 2
        Wait-Until { (Get-PanelOrder $h) -contains 'npx-test-2' } 6000 | Out-Null
        Start-Sleep -Milliseconds 800
    }
    # L'ordine si legge dal pannello: è quello vero delle schede del Blocco note (l'elenco delle schede
    # native, virtualizzato, può risultare ruotato dal riciclo degli elementi). Si parte da quello che
    # c'è adesso: le schede di prova più i file dei casi precedenti.
    $sorted = { param($o) (@($o) | Sort-Object) -join ',' }
    $base = Join-Order (Get-PanelOrder $h)
    Check 'Riapri scheda: pannello e schede del Blocco note allineati' ((& $sorted (Get-PanelOrder $h)) -eq (& $sorted (Get-NativeOrder $h))) $base
    $active = [int64][NpxKit]::GetForegroundWindow() -eq $h
    if (-not $active) { Write-Warning 'La finestra di prova non è attiva: controlli sulla combinazione saltati (l''azione si prova comunque).' }
    else { Check 'Ctrl+Maiusc+T riservata solo mentre la finestra del Blocco note è attiva' (Wait-Until { Test-HotkeyHeld } 3000) }
    # Scheda chiusa e riaperta: torna al posto che aveva (il Blocco note sa spostare le schede)
    $historyBefore = Get-ReopenHistoryCount -TestOnly
    Close-PanelTab $h 'npx-test-3'
    Check 'Scheda chiusa dal pannello' ((Get-PanelOrder $h) -notcontains 'npx-test-3') (Join-Order (Get-PanelOrder $h))
    # La cronologia è su disco (closed-tabs.txt): vale per le altre finestre e dopo aver chiuso e riaperto il Blocco note
    Check '  ricordata nella cronologia su disco' ((Get-ReopenHistoryCount -TestOnly) -eq $historyBefore + 1) ("righe di prova {0} -> {1}" -f $historyBefore, (Get-ReopenHistoryCount -TestOnly))
    Invoke-ReopenTab -Cli # dal comando del launcher, come dalla tray
    Check 'La scheda chiusa torna al suo posto nell''elenco' (Wait-Until { (Join-Order (Get-PanelOrder $h)) -eq $base } 12000) (Join-Order (Get-PanelOrder $h))
    Check '...e il Blocco note ha le stesse schede' ((& $sorted (Get-NativeOrder $h)) -eq (& $sorted (Get-PanelOrder $h))) (Join-Order (Get-NativeOrder $h))
    # Come Chrome la scheda riaperta è quella attiva (aperta in fondo e spostata, la selezione restava all'ultimo posto)
    Check '...ed è la scheda attiva' (Wait-Until { (Get-SelectedPanel $h) -eq 'npx-test-3' -and (Get-SelectedNative $h) -eq 'npx-test-3' } 4000) ("attiva: pannello {0}, Blocco note {1}" -f (Get-SelectedPanel $h), (Get-SelectedNative $h))
    # Due schede chiuse: tornano a ritroso (prima l'ultima chiusa)
    Close-PanelTab $h 'npx-test-2'
    Close-PanelTab $h 'npx-test-4'
    Invoke-ReopenTab
    Check 'Due schede chiuse: torna prima l''ultima' (Wait-Until { (Get-PanelOrder $h) -contains 'npx-test-4' } 12000) (Join-Order (Get-PanelOrder $h))
    Invoke-ReopenTab
    Check '...e premendo di nuovo torna la precedente' (Wait-Until { (Join-Order (Get-PanelOrder $h)) -eq $base } 12000) (Join-Order (Get-PanelOrder $h))
    # Pressione immediata: chi chiude una scheda per sbaglio preme subito, prima che la chiusura sia
    # confermata (mezzo secondo). Deve tornare quella scheda, non una di prima.
    Close-PanelTab $h 'npx-test-5' -NoWait
    Invoke-ReopenTab
    Check 'Premendo subito dopo la chiusura torna quella scheda' (Wait-Until { (Join-Order (Get-PanelOrder $h)) -eq $base } 12000) (Join-Order (Get-PanelOrder $h))
    # Scheda mai salvata: al comando non succede nulla (il Blocco note chiede già se salvarla). Si prova
    # con una scheda vuota aggiunta e richiusa, così la finestra resta come l'hanno lasciata i casi
    # precedenti.
    $before = @(Get-ModLog 400 'senza file salvato').Count
    Invoke-Element (Find-Control $h 'Nuova scheda' $CT::Button)
    Wait-Until { @(Get-PanelRows $h).Count -eq @($base -split ',').Count + 1 } 5000 | Out-Null
    $blank = Get-PanelRows $h | Where-Object { Test-Selected $_ } | Select-Object -First 1
    Invoke-Element (Find-ByName $blank 'Chiudi scheda')
    Wait-Until { (Join-Order (Get-PanelOrder $h)) -eq $base } 6000 | Out-Null
    Start-Sleep -Milliseconds 1200
    Check 'Scheda vuota chiusa: il Blocco note non la ricorda' (@(Get-ModLog 400 'senza file salvato').Count -gt $before)
    Invoke-ReopenTab
    Start-Sleep -Seconds 3
    Check 'Scheda mai salvata: al comando non succede nulla' ((Join-Order (Get-PanelOrder $h)) -eq $base) (Join-Order (Get-PanelOrder $h))
    # Spenta e riaccesa dal menu della tray (settings.ini)
    if ($active) {
        Set-IniSectionValue 'ReopenTab' 'Enabled' '0'
        Check 'Scorciatoia spenta: la combinazione torna agli altri programmi' (Wait-Until { -not (Test-HotkeyHeld) } 5000)
        Set-IniSectionValue 'ReopenTab' 'Enabled' '1'
        Check 'Scorciatoia riaccesa: la combinazione torna al Blocco note' (Wait-Until { Test-HotkeyHeld } 5000)
    }
    Select-Element (Find-PanelRow $h 'npx-test-1')
    Wait-Until { (Get-SelectedNative $h) -eq 'npx-test-1' } | Out-Null

    # --- Fuori dal pannello il menu nativo resta quello originale -------------------------------
    # Gate per finestra: solo le altre finestre restano in verticale, quella di prova torna orizzontale
    # (se l'utente le ha spente, nessuna finestra resta abilitata).
    $others = if ($onlyTestWindow) { @('0') } else { @(Get-NpWindows | ForEach-Object { Get-Hwnd $_ } | Where-Object { $_ -ne $h } | ForEach-Object { [string]$_ }) }
    $sinceGate = Get-Date
    Set-Content -LiteralPath $devWindows -Value $others -Encoding ASCII
    $devWindowsOurs = $true
    Check 'Finestra di prova di nuovo con le schede orizzontali' (Wait-Until { -not (Get-PanelList $h) } 8000)
    Check 'Finestra fuori dalla mod: barre di scorrimento di nuovo con il ritardo di sistema' (Wait-Until { & $hoverLog 'ScrollBarHover: finestra esclusa .* di nuovo con il ritardo di sistema \([1-9]\d* animazioni\)' $sinceGate } 5000)
    $pixels = $title -and [int64][NpxKit]::GetForegroundWindow() -eq $h
    if ($pixels) {
        # (nell'originale la barra dei menu ha il colore della scheda attiva e dell'editor)
        Check 'Senza pannello la barra dei menu torna al suo colore' (Wait-Until { $m = Get-MenuBarColor $h; $m -and -not (Test-SameColor $m $title 2) }) ("menu {0}, barra del titolo {1}" -f (Format-Color (Get-MenuBarColor $h)), (Format-Color $title))
        # Colore dei titoli sullo schermo: schede orizzontali (ora visibili) contro le righe misurate prima
        if ($panelTextNormal -and $panelTextSelected) {
            Start-Sleep -Milliseconds 400
            # Solo schede orizzontali sullo schermo: con -RealInput le 20 schede vuote aggiunte prima fanno uscire
            # dalla striscia alcune schede di prova (rettangolo vuoto o fuori dalla finestra)
            $wr = Get-WinRect $h
            $onScreen = { param($name) $t = Find-NativeTab $h $name; if (-not $t) { return $false }; $b = $t.Current.BoundingRectangle
                -not $b.IsEmpty -and -not [double]::IsInfinity($b.X) -and $b.Width -gt 20 -and $b.X -ge $wr.Left -and $b.X + $b.Width -le $wr.Right }
            $selectedNative = Get-SelectedNative $h
            $normalNative = @(Get-NativeOrder $h | Where-Object { $_ -like "$NpxTestPrefix*" -and $_ -ne $selectedNative -and (& $onScreen $_) })[0]
            if ($normalNative -and (& $onScreen $selectedNative)) {
                $nativeTextNormal = Get-NativeTabTextColor $h $normalNative $dark
                $nativeTextSelected = Get-NativeTabTextColor $h $selectedNative $dark
                Check 'Sullo schermo: titolo non attivo dello stesso colore' (Test-SameColor $panelTextNormal $nativeTextNormal 4) ("schede {0}, pannello {1}" -f (Format-Color $nativeTextNormal), (Format-Color $panelTextNormal))
                Check 'Sullo schermo: titolo attivo dello stesso colore' (Test-SameColor $panelTextSelected $nativeTextSelected 4) ("schede {0}, pannello {1}" -f (Format-Color $nativeTextSelected), (Format-Color $panelTextSelected))
            }
            else { Write-Warning 'Schede di prova non visibili nella striscia orizzontale: confronto dei colori dei titoli saltato.' }
        }
    }
    Check 'Senza pannello l''editor torna a tutta larghezza' (Wait-Until { $e = Get-EditorSurface $h; $e.Overhang -le 0 -and $e.Container.W -eq $e.Surface.W -and ($e.Container.X - (Get-WinRect $h).Left) -lt 20 }) ("contenitore x={0} w={1}, superficie w={2}" -f (Get-EditorSurface $h).Container.X, (Get-EditorSurface $h).Container.W, (Get-EditorSurface $h).Surface.W)
    Start-Sleep -Milliseconds 500
    # Una scheda di prova realizzata dalla striscia (con molte schede non lo sono tutte)
    $nativeTab = Get-NativeTabs $h | Where-Object { $_.Current.Name.StartsWith($NpxTestPrefix) -or $_.Current.Name.StartsWith('Senza titolo') } | Select-Object -First 1
    Show-ContextMenu $h $nativeTab.Current.Name -Native
    $names = @(Get-MenuItems | ForEach-Object { $_.Current.Name })
    Check 'Menu della scheda nativa senza voci aggiunte' (($names -contains 'Chiudi scheda') -and ($names -notcontains 'Colore scheda')) ($names -join ' | ')
    Invoke-MenuItem 'Nuova scheda' # chiude il menu (scheda vuota in più nella finestra di prova)
    $sinceGate = Get-Date
    if ($onlyTestWindow) { Set-Content -LiteralPath $devWindows -Value ([string]$h) -Encoding ASCII }
    else {
        Remove-Item -LiteralPath $devWindows
        $devWindowsOurs = $false
    }
    Check 'Pannello di nuovo attivo' (Wait-Until { Get-PanelList $h } 8000)
    Check 'Di nuovo nella mod: barre di scorrimento di nuovo subito larghe' (Wait-Until { & $hoverLog 'ScrollBarHover: (\d+ barre di scorrimento si allargano subito|altre \d+ barre adattate)' $sinceGate } 5000)
    if ($pixels) {
        Check 'Barra dei menu di nuovo come la barra del titolo' (Wait-Until { Test-SameColor (Get-MenuBarColor $h) $title 2 }) (Format-Color (Get-MenuBarColor $h))
    }
}
catch {
    Check 'Esecuzione completa' $false $_.Exception.Message
}
finally {
    Mouse-Up; Mouse-Up 'Right'
    Reset-CursorWatch
    if ($RealInput) { [NpxKit]::SetCursorPos($cursorStart.X, $cursorStart.Y) | Out-Null }
    if (-not $KeepWindow) {
        try { Close-TestWindow -RemoveFiles } catch { Write-Warning $_.Exception.Message }
        # La finestra di prova ha tenuto Ctrl+Maiusc+T: chiudendola deve tornare libera (la
        # registrazione è legata al thread, non alla finestra). Vale solo se il fuoco non è andato su
        # un'altra finestra del Blocco note, che la terrebbe a buon diritto.
        Start-Sleep -Milliseconds 1000
        $fg = [int64][NpxKit]::GetForegroundWindow()
        if (@(Get-NpWindows | ForEach-Object { Get-Hwnd $_ }) -contains $fg) {
            Write-Warning 'Una finestra del Blocco note è attiva: controllo sulla combinazione libera saltato.'
        }
        else {
            Check 'Chiusa la finestra di prova, Ctrl+Maiusc+T torna libera' (-not (Test-HotkeyHeld))
        }
    }
    foreach ($key in @($testWrites.Keys)) {
        $write = $testWrites[$key]
        if ((Get-IniValue $key) -ne $write.Value) {
            Write-Warning "$key è stato cambiato durante il test (non dal test): resta il valore attuale."
            continue
        }
        if ($originalSettings[$key]) { Set-Opt $write.Section $key $originalSettings[$key] }
        else { Remove-IniKey $key } # la chiave non c'era: torna il valore predefinito
    }
    Set-IniSectionValue 'BackToTop' 'Enabled' $originalBackToTop # (se mancava, la chiave si toglie)
    Set-IniSectionValue 'ReopenTab' 'Enabled' $originalReopenTab
    Set-IniSectionValue 'SmoothScroll' 'Disabled' $originalSmoothScroll
    # Torna la cronologia dell'utente, con in coda le schede che ha chiuso durante la prova
    if ($reopenIsolated) { Start-Sleep -Milliseconds 800; Exit-ReopenIsolation }
    # L'allowlist si toglie per ultima: con le schede verticali spente dall'utente, prima deve valere di
    # nuovo Enabled=0, altrimenti le sue finestre si accenderebbero per un attimo
    if ($devWindowsOurs) {
        if ($onlyTestWindow) { Start-Sleep -Milliseconds 1500 }
        Remove-Item -LiteralPath $devWindows -ErrorAction SilentlyContinue
    }
}

$failed = @($results | Where-Object { -not $_.Ok }).Count
Write-Host ''
Write-Host ("Risultato: {0} test superati, {1} falliti." -f ($results.Count - $failed), $failed) -ForegroundColor $(if ($failed) { 'Red' } else { 'Green' })
Write-Host "Screenshot: $ShotsDir"
exit $failed

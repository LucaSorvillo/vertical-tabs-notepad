<#
.SYNOPSIS
    Test dal vivo di VTabsNotepad su un'istanza dedicata del Blocco note.

.DESCRIPTION
    Il Blocco note è a istanza singola e all'avvio ripristina la sessione precedente: l'istanza di
    prova riaprirà quindi le schede dell'utente. Per questo il test:
      - agisce SOLO sulla finestra di prova (gate per-finestra in dev-windows.txt);
      - crea una scheda vuota e la rende attiva prima degli screenshot (niente contenuti privati);
      - non chiude né modifica le schede esistenti;
      - salva settings.ini e lo ripristina con -Cleanup.

    -Start    con nessun Blocco note aperto: avvia, inietta, screenshot prima/dopo
    -Shot     screenshot della finestra di prova (-Name per il nome del file)
    -Set      cambia un'impostazione (es. -Set -Section VerticalTabs -Key Collapsed -Value 1)
    -Stop     chiude le schede vuote create dal test (solo "Senza titolo" non modificate) e poi
              chiude normalmente il Blocco note (la sessione dell'utente viene salvata)
    -Cleanup  spegne le schede verticali, toglie la modalità sviluppo, ripristina settings.ini
              (il Blocco note resta aperto e utilizzabile)
.EXAMPLE
    .\tools\dev-test.ps1 -Start -OutDir C:\temp\npx
    .\tools\dev-test.ps1 -Shot -Name dopo-modifica
    .\tools\dev-test.ps1 -Set -Section VerticalTabs -Key Width -Value 300
    .\tools\dev-test.ps1 -Cleanup
#>
[CmdletBinding(DefaultParameterSetName = 'Start')]
param(
    [Parameter(ParameterSetName = 'Start')] [switch] $Start,
    [Parameter(ParameterSetName = 'Shot', Mandatory)] [switch] $Shot,
    [Parameter(ParameterSetName = 'Shot')] [string] $Name = 'shot',
    [Parameter(ParameterSetName = 'Set', Mandatory)] [switch] $Set,
    [Parameter(ParameterSetName = 'Set', Mandatory)] [string] $Section,
    [Parameter(ParameterSetName = 'Set', Mandatory)] [string] $Key,
    [Parameter(ParameterSetName = 'Set', Mandatory)] [string] $Value,
    [Parameter(ParameterSetName = 'Stop', Mandatory)] [switch] $Stop,
    [Parameter(ParameterSetName = 'Cleanup', Mandatory)] [switch] $Cleanup,
    [string] $OutDir = (Join-Path $env:TEMP 'npx-shots')
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root 'build\bin'
$data = Join-Path $env:LOCALAPPDATA 'VTabsNotepad'
$settings = Join-Path $data 'settings.ini'
$backup = Join-Path $data 'settings.ini.devbak'
$devWindows = Join-Path $data 'dev-windows.txt'
$stateFile = Join-Path $data 'dev-test.json'
New-Item -ItemType Directory -Force $data | Out-Null

function Set-Setting([string] $Section, [string] $Key, [string] $Value) {
    $lines = [System.Collections.Generic.List[string]]::new()
    if (Test-Path $settings) { $lines.AddRange([string[]](Get-Content -LiteralPath $settings)) }
    $current = ''
    $sectionLine = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*\[(.+)\]\s*$') {
            $current = $Matches[1]
            if ($current -eq $Section) { $sectionLine = $i }
        }
        elseif ($current -eq $Section -and $lines[$i] -match "^\s*$Key\s*=") {
            $lines[$i] = "$Key=$Value"
            $lines | Set-Content -LiteralPath $settings -Encoding Unicode
            return
        }
    }
    if ($sectionLine -ge 0) { $lines.Insert($sectionLine + 1, "$Key=$Value") }
    else { $lines.Add("[$Section]"); $lines.Add("$Key=$Value") }
    $lines | Set-Content -LiteralPath $settings -Encoding Unicode
}

function Invoke-NotepadButton([long] $Hwnd, [string] $AutomationName) {
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $window = [Windows.Automation.AutomationElement]::FromHandle([IntPtr]$Hwnd)
    $condition = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::NameProperty, $AutomationName)
    $button = $window.FindFirst([Windows.Automation.TreeScope]::Descendants, $condition)
    if (-not $button) { throw "Pulsante '$AutomationName' non trovato." }
    $button.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke()
}

function Save-Shot([long] $Hwnd, [string] $FileName) {
    New-Item -ItemType Directory -Force $OutDir | Out-Null
    $path = Join-Path $OutDir "$FileName.png"
    & (Join-Path $PSScriptRoot 'Capture-Window.ps1') -Hwnd $Hwnd -OutPath $path
}

switch ($PSCmdlet.ParameterSetName) {
    'Start' {
        if (Get-Process Notepad -ErrorAction SilentlyContinue) {
            throw "C'è già un Blocco note aperto: chiudilo e riprova (serve un'istanza nuova, con la mod aggiornata)."
        }
        if ((Test-Path $settings) -and -not (Test-Path $backup)) { Copy-Item -LiteralPath $settings $backup }

        Write-Host 'Avvio il Blocco note (ripristinerà la sessione precedente)...'
        Start-Process notepad.exe
        $proc = $null
        for ($i = 0; $i -lt 40 -and -not $proc; $i++) {
            Start-Sleep -Milliseconds 250
            $proc = Get-Process Notepad -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        }
        if (-not $proc) { throw 'Il Blocco note non si è avviato.' }
        Start-Sleep -Seconds 2
        $hwnd = [int64]$proc.MainWindowHandle
        Write-Host ("Finestra di prova: PID {0}, HWND 0x{1:X}" -f $proc.Id, $hwnd)

        # La mod agirà solo su questa finestra; schede verticali spente per lo screenshot "prima".
        Set-Content -LiteralPath $devWindows -Value ([string]$hwnd) -Encoding ASCII
        Set-Setting 'VerticalTabs' 'Enabled' '0'
        Set-Setting 'VerticalTabs' 'Width' '260'
        Set-Setting 'Debug' 'VerboseLog' '1'

        & (Join-Path $bin 'VTabsNotepad.exe') --inject-pid $proc.Id
        Start-Sleep -Seconds 3

        # Scheda vuota in primo piano: negli screenshot non compaiono contenuti personali.
        Invoke-NotepadButton -Hwnd $hwnd -AutomationName 'Aggiungi nuova scheda'
        Start-Sleep -Seconds 1
        Save-Shot $hwnd '1-prima-orizzontale'

        Write-Host 'Attivo le schede verticali (hot-reload da settings.ini)...'
        Set-Setting 'VerticalTabs' 'Enabled' '1'
        Start-Sleep -Seconds 3
        Save-Shot $hwnd '2-dopo-verticale'

        @{ pid = $proc.Id; hwnd = $hwnd; outDir = $OutDir } | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8
        Write-Host ("Screenshot in {0}. Quando hai finito: .\tools\dev-test.ps1 -Cleanup" -f $OutDir)
    }
    'Shot' {
        if (-not (Test-Path $stateFile)) { throw 'Nessun test in corso (manca dev-test.json): esegui prima -Start.' }
        $state = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
        if (-not $PSBoundParameters.ContainsKey('OutDir')) { $OutDir = $state.outDir }
        Save-Shot ([int64]$state.hwnd) $Name
    }
    'Set' {
        Set-Setting $Section $Key $Value
        Write-Host "$Section.$Key=$Value"
    }
    'Stop' {
        if (-not (Test-Path $stateFile)) { throw 'Nessun test in corso (manca dev-test.json).' }
        $state = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
        $hwnd = [int64]$state.hwnd
        Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
        $AE = [Windows.Automation.AutomationElement]
        $window = $AE::FromHandle([IntPtr]$hwnd)
        $blankName = New-Object Windows.Automation.PropertyCondition($AE::NameProperty, 'Senza titolo. Non modificato.')
        $isTab = New-Object Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [Windows.Automation.ControlType]::TabItem)
        $condition = New-Object Windows.Automation.AndCondition($blankName, $isTab)
        $closeName = New-Object Windows.Automation.PropertyCondition($AE::NameProperty, 'Chiudi scheda')
        $closed = 0
        for ($i = 0; $i -lt 10; $i++) {
            # Solo schede native "Senza titolo" non modificate: vuote, nessun dato a rischio.
            $tab = $window.FindFirst([Windows.Automation.TreeScope]::Descendants, $condition)
            if (-not $tab) { break }
            $tab.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select()
            Start-Sleep -Milliseconds 400
            $close = $tab.FindFirst([Windows.Automation.TreeScope]::Descendants, $closeName)
            if (-not $close) { break }
            $close.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke()
            Start-Sleep -Milliseconds 600
            $closed++
        }
        Write-Host "Schede vuote di test chiuse: $closed"

        Add-Type -Namespace NpxStop -Name Native -MemberDefinition '[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);'
        [NpxStop.Native]::PostMessage([IntPtr]$hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        $deadline = (Get-Date).AddSeconds(25)
        while ((Get-Process -Id $state.pid -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
        if (Get-Process -Id $state.pid -ErrorAction SilentlyContinue) { Write-Warning 'Il Blocco note non si è chiuso (forse chiede di salvare): controlla la finestra.' }
        else { Write-Host 'Blocco note chiuso normalmente (sessione salvata).' }
    }
    'Cleanup' {
        Set-Setting 'VerticalTabs' 'Enabled' '0'
        Start-Sleep -Seconds 2
        Remove-Item -LiteralPath $devWindows -ErrorAction SilentlyContinue
        if (Test-Path $backup) { Move-Item -LiteralPath $backup $settings -Force }
        Remove-Item -LiteralPath $stateFile -ErrorAction SilentlyContinue
        Write-Host 'Pulizia fatta: schede verticali spente, modalità sviluppo rimossa, impostazioni ripristinate.'
        Write-Host 'Il Blocco note resta aperto; le schede vuote create dal test puoi chiuderle a mano.'
    }
}

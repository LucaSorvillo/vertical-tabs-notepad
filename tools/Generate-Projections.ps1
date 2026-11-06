<#
.SYNOPSIS
    Genera le proiezioni C++/WinRT usate dalla mod:
      - Windows SDK (namespace Windows.*)
      - Windows App SDK (Microsoft.UI.*), dai .winmd del runtime che usa il Blocco note
      - NotepadXamlUI.winmd: i metadati pubblici dei controlli XAML del Blocco note
        (TabsBar, TabItemViewModel, ...), inclusi nel pacchetto installato.
    Nessun file Microsoft viene copiato nel progetto: si leggono i .winmd installati
    e si rigenera tutto quando cambia la versione del Blocco note o del runtime.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $OutDir,
    [switch] $Force
)
$ErrorActionPreference = 'Stop'

$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdk = Get-ChildItem -LiteralPath (Join-Path $kits 'Include') -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'cppwinrt\winrt\base.h') } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if (-not $sdk) { throw 'Windows SDK con C++/WinRT non trovato (installa il Windows SDK da Visual Studio Installer).' }
$cppwinrt = Join-Path $kits "bin\$($sdk.Name)\x64\cppwinrt.exe"
if (-not (Test-Path $cppwinrt)) { throw "cppwinrt.exe non trovato: $cppwinrt" }

$notepad = Get-AppxPackage -Name Microsoft.WindowsNotepad | Sort-Object { [version]$_.Version } -Descending | Select-Object -First 1
if (-not $notepad) { throw 'Il Blocco note (Microsoft.WindowsNotepad) non risulta installato.' }
$notepadWinmd = Join-Path $notepad.InstallLocation 'NotepadXamlUI.winmd'
if (-not (Test-Path $notepadWinmd)) { throw "NotepadXamlUI.winmd non trovato in $($notepad.InstallLocation)" }

# Il runtime Windows App SDK da cui dipende il Blocco note (es. Microsoft.WindowsAppRuntime.2).
[xml] $manifest = Get-Content -LiteralPath (Join-Path $notepad.InstallLocation 'AppxManifest.xml') -Raw
$runtimeName = ($manifest.Package.Dependencies.PackageDependency |
    Where-Object { $_.Name -like 'Microsoft.WindowsAppRuntime.*' } |
    Select-Object -First 1).Name
if (-not $runtimeName) { throw 'Dipendenza Windows App Runtime non trovata nel manifest del Blocco note.' }
$runtime = Get-AppxPackage -Name $runtimeName |
    Where-Object { $_.Architecture -eq 'X64' } |
    Sort-Object { [version]$_.Version } -Descending |
    Select-Object -First 1
if (-not $runtime) { throw "Runtime $runtimeName (x64) non installato." }

$stamp = "cppwinrt=$($sdk.Name);runtime=$($runtime.PackageFullName);notepad=$($notepad.PackageFullName)"
$stampFile = Join-Path $OutDir 'projection.stamp'
if (-not $Force -and (Test-Path $stampFile) -and (Test-Path (Join-Path $OutDir 'winrt\NotepadXamlUI.h')) -and
    ((Get-Content -LiteralPath $stampFile -Raw).Trim() -eq $stamp)) {
    Write-Host "Proiezioni C++/WinRT già aggiornate."
    return
}

$runtimeWinmds = @(
    'Microsoft.Foundation.winmd',
    'Microsoft.Graphics.winmd',
    'Microsoft.UI.winmd',
    'Microsoft.UI.Text.winmd',
    'Microsoft.UI.Xaml.winmd',
    'Microsoft.Windows.ApplicationModel.Resources.winmd'
) | ForEach-Object { Join-Path $runtime.InstallLocation $_ } | Where-Object { Test-Path $_ }

Write-Host "Genero le proiezioni C++/WinRT:"
Write-Host "  SDK      $($sdk.Name)"
Write-Host "  Runtime  $($runtime.PackageFullName)"
Write-Host "  Notepad  $($notepad.PackageFullName)"

if (Test-Path $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Microsoft.UI.Xaml.winmd fa riferimento ai tipi di WebView2 (controllo WebView2), i cui metadati
# non sono installati con il runtime: si prendono dal pacchetto NuGet ufficiale (solo il .winmd).
$webView2Version = '1.0.4078.44'
$nugetDir = Join-Path (Split-Path $OutDir -Parent) 'nuget'
$webView2Winmd = Join-Path $nugetDir "Microsoft.Web.WebView2.$webView2Version\Microsoft.Web.WebView2.Core.winmd"
if (-not (Test-Path $webView2Winmd)) {
    New-Item -ItemType Directory -Force -Path (Split-Path $webView2Winmd -Parent) | Out-Null
    $nupkg = Join-Path $nugetDir "microsoft.web.webview2.$webView2Version.zip"
    Write-Host "Scarico Microsoft.Web.WebView2 $webView2Version da nuget.org (solo metadati)..."
    Invoke-WebRequest -UseBasicParsing -Uri "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$webView2Version/microsoft.web.webview2.$webView2Version.nupkg" -OutFile $nupkg
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($nupkg)
    try {
        $entry = $zip.Entries | Where-Object { $_.FullName -eq 'lib/Microsoft.Web.WebView2.Core.winmd' } | Select-Object -First 1
        if (-not $entry) { throw 'Microsoft.Web.WebView2.Core.winmd non trovato nel pacchetto NuGet.' }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $webView2Winmd, $true)
    } finally {
        $zip.Dispose()
    }
    Remove-Item -LiteralPath $nupkg -Force
}

$cppwinrtArgs = @('-in', 'sdk')
foreach ($winmd in $runtimeWinmds) { $cppwinrtArgs += @('-in', $winmd) }
$cppwinrtArgs += @('-in', $notepadWinmd, '-in', $webView2Winmd, '-out', $OutDir)
& $cppwinrt @cppwinrtArgs
if ($LASTEXITCODE -ne 0) { throw "cppwinrt.exe terminato con codice $LASTEXITCODE" }

Set-Content -LiteralPath $stampFile -Value $stamp -Encoding UTF8
Write-Host "Proiezioni generate in $OutDir"

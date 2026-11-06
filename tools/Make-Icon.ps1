<#
.SYNOPSIS
    Icona dell'app (eseguibile, installer, sito, README) dall'icona della tray: lo stesso disegno di
    src\launcher\TrayIcon.cpp (CreateNotepadOutlineIcon, griglia di 16 unità agganciata ai pixel), bianco su un
    quadrato arrotondato scuro, così si vede su qualunque sfondo.
.EXAMPLE
    .\tools\Make-Icon.ps1      # src\launcher\VTabsNotepad.ico e docs\assets\icon-*.png
#>
param(
    [string] $Ico = (Join-Path (Split-Path $PSScriptRoot -Parent) 'src\launcher\VTabsNotepad.ico'),
    [string] $PngDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'docs\assets')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$TileColor = [Drawing.Color]::FromArgb(255, 0x20, 0x20, 0x20) # come la barra delle applicazioni scura
$IcoSizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$PngSizes = 32, 128, 180, 512 # favicon, README e sito, icona per iPhone, anteprima grande

# --- Griglia e glifo: stessi conti di TrayIcon.cpp
function New-Grid([int]$Size) {
    $s = $Size / 16.0
    $width = if ($s -lt 1.4) { 1.0 } elseif ($s -lt 2.6) { 2.0 } else { [math]::Round($s) }
    [pscustomobject]@{ S = $s; Width = $width }
}
function GridLine($Grid, [double]$U) { if ([int]$Grid.Width % 2 -eq 1) { [math]::Floor($U * $Grid.S) + 0.5 } else { [math]::Round($U * $Grid.S) } }
function Add-RoundRect($Path, [double]$X, [double]$Y, [double]$W, [double]$H, [double]$R) {
    $d = 2 * $R
    $Path.AddArc([float]$X, [float]$Y, [float]$d, [float]$d, 180, 90)
    $Path.AddArc([float]($X + $W - $d), [float]$Y, [float]$d, [float]$d, 270, 90)
    $Path.AddArc([float]($X + $W - $d), [float]($Y + $H - $d), [float]$d, [float]$d, 0, 90)
    $Path.AddArc([float]$X, [float]($Y + $H - $d), [float]$d, [float]$d, 90, 90)
    $Path.CloseFigure()
}
function Draw-Glyph($G, [int]$Size, [Drawing.Color]$Color) {
    $grid = New-Grid $Size; $s = $grid.S
    $pen = New-Object Drawing.Pen $Color, ([float]$grid.Width)
    $pen.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
    $pen.StartCap = [Drawing.Drawing2D.LineCap]::Square; $pen.EndCap = [Drawing.Drawing2D.LineCap]::Square
    # Pagina
    $left = GridLine $grid 2.5; $right = GridLine $grid 13.5; $top = GridLine $grid 3.5; $bottom = GridLine $grid 14.5
    $page = New-Object Drawing.Drawing2D.GraphicsPath
    Add-RoundRect $page $left $top ($right - $left) ($bottom - $top) ([math]::Max(1.0, 1.6 * $s))
    $G.DrawPath($pen, $page)
    # Tre anelli che passano il bordo in alto
    $middle = GridLine $grid 8.0; $ringStep = [math]::Max(2.0, [math]::Round(2.5 * $s))
    $ringTop = GridLine $grid 1.5; $ringBottom = GridLine $grid 5.0
    foreach ($k in -1..1) { $x = $middle + $k * $ringStep; $G.DrawLine($pen, [float]$x, [float]$ringTop, [float]$x, [float]$ringBottom) }
    # Tre righe, l'ultima più corta
    $lineLeft = GridLine $grid 5.5; $lineRight = GridLine $grid 10.5; $shortRight = GridLine $grid 8.0
    $firstRow = GridLine $grid 7.5; $rowStep = [math]::Max(2.0, [math]::Round(2.0 * $s))
    $G.DrawLine($pen, [float]$lineLeft, [float]$firstRow, [float]$lineRight, [float]$firstRow)
    $G.DrawLine($pen, [float]$lineLeft, [float]($firstRow + $rowStep), [float]$lineRight, [float]($firstRow + $rowStep))
    $G.DrawLine($pen, [float]$lineLeft, [float]($firstRow + 2 * $rowStep), [float]$shortRight, [float]($firstRow + 2 * $rowStep))
    $pen.Dispose(); $page.Dispose()
}

# --- Quadrato arrotondato con il glifo al centro
function New-IconBitmap([int]$Size) {
    $bmp = New-Object Drawing.Bitmap $Size, $Size, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::Half
    $g.Clear([Drawing.Color]::Transparent)
    $tile = New-Object Drawing.Drawing2D.GraphicsPath
    Add-RoundRect $tile 0 0 $Size $Size ([math]::Max(2.0, $Size * 0.22))
    $brush = New-Object Drawing.SolidBrush $TileColor
    $g.FillPath($brush, $tile)
    # Il glifo nella sua griglia, spostato di pixel interi: le linee restano nitide
    $inner = if ($Size -le 24) { $Size - 2 } else { [int][math]::Round($Size * 0.8) }
    $offset = [int][math]::Floor(($Size - $inner) / 2)
    $g.TranslateTransform($offset, $offset)
    Draw-Glyph $g $inner ([Drawing.Color]::White)
    $g.Dispose(); $brush.Dispose(); $tile.Dispose()
    $bmp
}
function Get-PngBytes($Bitmap) { $ms = New-Object IO.MemoryStream; $Bitmap.Save($ms, [Drawing.Imaging.ImageFormat]::Png); $bytes = $ms.ToArray(); $ms.Dispose(); , $bytes }

# --- .ico con tutte le misure (immagini PNG, come le icone di Windows 11)
$images = @(foreach ($n in $IcoSizes) { $b = New-IconBitmap $n; , (Get-PngBytes $b); $b.Dispose() })
$stream = [IO.File]::Create($Ico)
$writer = New-Object IO.BinaryWriter $stream
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$IcoSizes.Count)
$offset = 6 + 16 * $IcoSizes.Count
for ($i = 0; $i -lt $IcoSizes.Count; $i++) {
    $n = $IcoSizes[$i]; $side = if ($n -ge 256) { 0 } else { $n }
    $writer.Write([byte]$side); $writer.Write([byte]$side); $writer.Write([byte]0); $writer.Write([byte]0)
    $writer.Write([uint16]1); $writer.Write([uint16]32)
    $writer.Write([uint32]$images[$i].Length); $writer.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($bytes in $images) { $writer.Write([byte[]]$bytes) }
$writer.Dispose(); $stream.Dispose()
"icona: $Ico ($($IcoSizes -join ', ') px)"

# --- PNG per il sito e il README
New-Item -ItemType Directory -Force $PngDir | Out-Null
foreach ($n in $PngSizes) { $b = New-IconBitmap $n; $b.Save((Join-Path $PngDir "icon-$n.png"), [Drawing.Imaging.ImageFormat]::Png); $b.Dispose() }
"immagini: $PngDir\icon-{$($PngSizes -join ',')}.png"

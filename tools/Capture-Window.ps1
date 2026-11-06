<#
.SYNOPSIS
    Cattura uno screenshot di una finestra (per HWND) in un PNG, utile per verificare il layout.
    Usa PrintWindow con PW_RENDERFULLCONTENT (cattura anche il contenuto XAML/Direct2D); se il
    risultato è vuoto ripiega su una copia dallo schermo (la finestra deve essere visibile).
.EXAMPLE
    .\Capture-Window.ps1 -Hwnd 0x8912B8 -OutPath C:\temp\notepad.png
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [long] $Hwnd,
    [Parameter(Mandatory)] [string] $OutPath
)
$ErrorActionPreference = 'Stop'

# Add-Type fallirebbe se lo script viene eseguito due volte nella stessa sessione.
if (-not ('NpxWinShot' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public static class NpxWinShot {
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
  [StructLayout(LayoutKind.Sequential)] struct RECT { public int left, top, right, bottom; }

  static bool IsBlank(Bitmap bmp) {
    // Campionamento: tutto nero o tutto trasparente = cattura non riuscita.
    for (int y = 0; y < bmp.Height; y += Math.Max(1, bmp.Height / 24))
      for (int x = 0; x < bmp.Width; x += Math.Max(1, bmp.Width / 24)) {
        Color c = bmp.GetPixel(x, y);
        if (c.A != 0 && (c.R | c.G | c.B) != 0) return false;
      }
    return true;
  }

  public static string Capture(long hwnd, string path) {
    IntPtr h = (IntPtr)hwnd;
    if (!IsWindow(h)) throw new Exception("HWND non valido: " + hwnd.ToString("X"));
    // Coordinate fisiche: senza, con lo schermo scalato (es. 125%) la finestra verrebbe ritagliata.
    SetThreadDpiAwarenessContext((IntPtr)(-4) /* PER_MONITOR_AWARE_V2 */);
    RECT r; GetWindowRect(h, out r);
    int w = r.right - r.left, ht = r.bottom - r.top;
    if (w <= 0 || ht <= 0) throw new Exception("Dimensioni finestra non valide.");
    using (var bmp = new Bitmap(w, ht)) {
      bool ok;
      using (var g = Graphics.FromImage(bmp)) {
        IntPtr hdc = g.GetHdc();
        try { ok = PrintWindow(h, hdc, 0x00000002 /* PW_RENDERFULLCONTENT */); }
        finally { g.ReleaseHdc(hdc); }
      }
      string method = "PrintWindow";
      if (!ok || IsBlank(bmp)) {
        using (var g = Graphics.FromImage(bmp)) { g.CopyFromScreen(r.left, r.top, 0, 0, new Size(w, ht)); }
        method = "CopyFromScreen";
      }
      bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
      return method;
    }
  }
}
'@
}

$method = [NpxWinShot]::Capture($Hwnd, $OutPath)
Write-Host "Screenshot salvato ($method): $OutPath"

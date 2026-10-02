Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
}
"@

$log = Join-Path $env:LOCALAPPDATA 'PagedExplorer\PagedExplorer.log'
if (Test-Path $log) { Remove-Item $log -Force }

$exe = 'd:\APrj\BinDir\bin\Release\PagedExplorer.exe'
Start-Process -FilePath $exe
Start-Sleep -Seconds 5

$p = Get-Process PagedExplorer -ErrorAction SilentlyContinue
if (-not $p) { Write-Host 'NO PROCESS'; exit 1 }
$h = $p[0].MainWindowHandle
Write-Host ("PID=" + $p[0].Id + " HWND=" + $h)
[void][Win]::ShowWindow($h, 9)
[void][Win]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 800

$r = New-Object Win+RECT
[void][Win]::GetWindowRect($h, [ref]$r)
$w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
Write-Host ("WIN RECT " + $r.Left + "," + $r.Top + "," + $r.Right + "," + $r.Bottom + " size=" + $w + "x" + $ht)
$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $ht)))
$bmp.Save('d:\APrj\BinDir\.codebuddy\app.png', [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host 'SAVED app.png'

Write-Host '=== LOG ==='
if (Test-Path $log) { Get-Content $log } else { Write-Host 'no log' }

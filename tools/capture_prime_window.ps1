param([Parameter(Mandatory=$true)][int]$TargetProcessId, [Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class PrimeWindowCapture {
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out Rect rect);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
}
"@
$taskProcess = Get-Process -Id $TargetProcessId
$taskWindow = $taskProcess.MainWindowHandle
if ($taskWindow -eq [IntPtr]::Zero) { throw 'Target has no main window handle' }
$taskRect = New-Object PrimeWindowCapture+Rect
if (-not [PrimeWindowCapture]::GetClientRect($taskWindow, [ref]$taskRect)) { throw 'GetClientRect failed' }
$taskBitmap = New-Object System.Drawing.Bitmap ($taskRect.Right - $taskRect.Left), ($taskRect.Bottom - $taskRect.Top)
$taskGraphics = [System.Drawing.Graphics]::FromImage($taskBitmap)
$taskDc = $taskGraphics.GetHdc()
try { $taskSuccess = [PrimeWindowCapture]::PrintWindow($taskWindow, $taskDc, 3) }
finally { $taskGraphics.ReleaseHdc($taskDc) }
try { $taskBitmap.Save([System.IO.Path]::GetFullPath($OutputPath), [System.Drawing.Imaging.ImageFormat]::Png) }
finally { $taskGraphics.Dispose(); $taskBitmap.Dispose() }
[ordered]@{ pid=$TargetProcessId; print_window_return=$taskSuccess; path=[System.IO.Path]::GetFullPath($OutputPath); width=$taskRect.Right; height=$taskRect.Bottom; note='A black PrintWindow capture may reflect GPU capture limitations; it does not prove an unrendered frame.' } | ConvertTo-Json

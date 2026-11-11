<#
 Minimal remote control of Corruption_3 PC.exe for unattended checks.
   drive_prime.ps1 start [-Env NAME=VALUE,...]   launch (state in .tools/drive_prime.pid)
   drive_prime.ps1 shot  <out.png>               screenshot of the game window (screen copy)
   drive_prime.ps1 key   <name> [holdMs]         press a key (Enter, Space, Shift, 1, W, Up, Esc, F1...)
   drive_prime.ps1 click <x> <y> [left|right]    click at client coordinates (0..width, 0..height)
   drive_prime.ps1 move  <x> <y>                 move the pointer
   drive_prime.ps1 log                           print the newest log folder path
   drive_prime.ps1 stop                          close the game
 The window must stay in the foreground: this sends real input.
#>
param([Parameter(Mandatory = $true)][string]$Action, [string]$A1, [string]$A2, [string]$A3, [string[]]$Env)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$dir = Join-Path $root 'recomp/build/RM3E01_00_runtime_link_probe'
$pidFile = Join-Path $root 'recomp/.tools/drive_prime.pid'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class DrivePrime {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr e);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
}
"@
function Get-Game {
    if (-not (Test-Path $pidFile)) { throw 'No game started by drive_prime.ps1' }
    $p = Get-Process -Id ([int](Get-Content $pidFile)) -ErrorAction SilentlyContinue
    if (-not $p) { throw 'The game is not running' }
    $p.Refresh(); return $p
}
function Focus($p) { [void][DrivePrime]::ShowWindow($p.MainWindowHandle, 9); [void][DrivePrime]::SetForegroundWindow($p.MainWindowHandle); try { [void](New-Object -ComObject WScript.Shell).AppActivate($p.Id) } catch {}; Start-Sleep -Milliseconds 250 }
function Client-Origin($p) {
    $r = New-Object DrivePrime+RECT; [void][DrivePrime]::GetClientRect($p.MainWindowHandle, [ref]$r)
    $pt = New-Object DrivePrime+POINT; [void][DrivePrime]::ClientToScreen($p.MainWindowHandle, [ref]$pt)
    return @{ X = $pt.X; Y = $pt.Y; W = $r.Right; H = $r.Bottom }
}
$vk = @{ Enter = 0x0D; Space = 0x20; Shift = 0x10; Ctrl = 0x11; Esc = 0x1B; Up = 0x26; Down = 0x28; Left = 0x25; Right = 0x27; Tab = 0x09
         F1 = 0x70; F6 = 0x75; F12 = 0x7B; W = 0x57; A = 0x41; S = 0x53; D = 0x44 }
switch ($Action) {
    'start' {
        foreach ($e in ($Env | ForEach-Object { $_ -split ';' })) { $k, $v = $e -split '=', 2; [Environment]::SetEnvironmentVariable($k, $v, 'Process') }
        [Environment]::SetEnvironmentVariable('WII_COMPILED_PRIME_KEYBOARD_MOUSE', '1', 'Process')
        $p = Start-Process -FilePath (Join-Path $dir 'Corruption_3 PC.exe') -WorkingDirectory $dir -PassThru
        Set-Content $pidFile $p.Id
        "started pid $($p.Id)"
    }
    'shot' {
        $p = Get-Game; Focus $p; $o = Client-Origin $p
        $bmp = New-Object System.Drawing.Bitmap $o.W, $o.H
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($o.X, $o.Y, 0, 0, $bmp.Size)
        $bmp.Save([IO.Path]::GetFullPath($A1), [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
        "saved $A1 ($($o.W)x$($o.H))"
    }
    'key' {
        $p = Get-Game; Focus $p
        $code = if ($vk.ContainsKey($A1)) { $vk[$A1] } elseif ($A1.Length -eq 1) { [int][char]$A1.ToUpper() } else { throw "unknown key $A1" }
        $scan = [byte][DrivePrime]::MapVirtualKey([uint32]$code, 0)
        [DrivePrime]::keybd_event([byte]$code, $scan, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds $(if ($A2) { [int]$A2 } else { 120 })
        [DrivePrime]::keybd_event([byte]$code, $scan, 2, [UIntPtr]::Zero)
        "key $A1"
    }
    { $_ -in 'click', 'move' } {
        $p = Get-Game; Focus $p; $o = Client-Origin $p
        [void][DrivePrime]::SetCursorPos($o.X + [int]$A1, $o.Y + [int]$A2)
        Start-Sleep -Milliseconds 200
        if ($Action -eq 'click') {
            $right = $A3 -eq 'right'
            [DrivePrime]::mouse_event($(if ($right) { 8 } else { 2 }), 0, 0, 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 120
            [DrivePrime]::mouse_event($(if ($right) { 16 } else { 4 }), 0, 0, 0, [UIntPtr]::Zero)
        }
        "$Action $A1,$A2"
    }
    'log' {
        $p = Get-Game
        (Get-ChildItem (Join-Path $dir 'UserData/Logs') -Directory | Where-Object { $_.Name -like "*pid$($p.Id)" } | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
    }
    'stop' {
        $p = Get-Process -Id ([int](Get-Content $pidFile)) -ErrorAction SilentlyContinue
        if ($p) { Stop-Process -Id $p.Id -Force; "stopped $($p.Id)" } else { 'not running' }
    }
}

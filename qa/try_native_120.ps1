# Private experiment (test build only): the game at two logic steps and two drawn frames per console frame,
# 119.88 real frames per second with a step of 1/120 s, no interpolation. Nothing in the game is adjusted
# for it: this is to see what depends on the frame count and what it costs (recomp/docs/fps_altos.md).
#   - Set "Frame rate above 60 (interpolated)" to Off in F1 > Video first.
#   - Use a window or borderless fullscreen on a 120 Hz desktop (exclusive fullscreen asks for 59.94 Hz).
#   - It uses your normal profile and save: do not save over a game you care about.
$ErrorActionPreference = 'Stop'
$build = (Resolve-Path (Join-Path $PSScriptRoot '..\build\RM3E01_00_runtime_link_probe')).Path
if (@(Get-Process | Where-Object { $_.ProcessName -like 'Corruption_3*' }).Count) { throw 'Close the game first.' }
$env:WII_COMPILED_QA_NATIVE_120 = '1'
try { Start-Process -FilePath (Join-Path $build 'Corruption_3 PC.exe') -WorkingDirectory $build }
finally { Remove-Item Env:WII_COMPILED_QA_NATIVE_120 }

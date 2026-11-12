<#
 Packages the public release zip of Corruption_3 PC.
   package_release.ps1 [-PublishDirectory publish] [-OutputDirectory dist]

 Produces dist\Corruption_3-PC-vX.Y.Z-win64\ (the staged folder), the zip and its .sha256. The version
 comes from prime_port_version.h, the only place it is written.

 What a player unzips: the executable, the runtime DLLs, the bundled Wii bootstrap and DSP data, the
 shader seed, Config.default.toml (copied to UserData/Config.toml on first run) and the documentation.
 The zip never contains UserData, so unzipping over an old folder keeps saves and settings.

 No game data ever enters the zip: the port reads the player's own disc image at run time. This script
 refuses to package if it finds disc images, extracted game data, a save or any UserData in the staged
 folder, and refuses an executable built with the private QA tools (MKW_PRIME_QA_TOOLS).
 It also refuses the developer's credentials (check_no_credentials.ps1): the RetroAchievements session
 file and token, the GitHub and VirusTotal keys, Config.toml, logs and reports, in the folder and the zip.

 Rebuilt on 2026-10-26 after the original file went missing from the working tree; the file list and the
 default configuration were reproduced from the shipped v0.1.2 package.
#>
param(
    [string]$PublishDirectory = 'publish',
    [string]$OutputDirectory = 'dist'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = Join-Path $root 'recomp\build\RM3E01_00_runtime_link_probe'
$publish = Join-Path $root $PublishDirectory

$versionHeader = Get-Content -LiteralPath (Join-Path $root 'recomp\third_party\WiiCompiled\runtime\include\prime_port_version.h') -Raw
if ($versionHeader -notmatch '#define PRIME_PORT_VERSION "([^"]+)"') { throw 'PRIME_PORT_VERSION not found' }
$version = $Matches[1]
$name = "Corruption_3-PC-v$version-win64"
$out = Join-Path $root $OutputDirectory
$stage = Join-Path $out $name
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage, (Join-Path $stage 'docs') | Out-Null

# Executable and runtime files produced by the build.
$exe = Join-Path $build 'Corruption_3 PC.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Executable missing: $exe (build the WiiCompiled target first)" }
# A test build carries the private QA tools (input record/replay); they never ship.
$exeText = [IO.File]::ReadAllText($exe, [Text.Encoding]::GetEncoding(28591))
if ($exeText.Contains('MKW_PRIME_QA_TOOLS_ENABLED_BUILD')) { throw 'This executable was built with MKW_PRIME_QA_TOOLS; rebuild with -DMKW_PRIME_QA_TOOLS=OFF before packaging.' }
# Monitoring, latency, performance and debug tools stay in the test build (user's rule, 01/11/2026).
$diagnostics = @('[host-stall]', 'others_over_ms', '[gpu-time]', '[post-ssao]', '[post-scene-dump]', '[glow-soften]',
    '[prime-defer-verify]', '[prime-gpu-thread] bursts=', '[prime-texture-invalidation]', '[dump-dl-draw]', 'Game performance [F9]', 'WII_COMPILED_PRIME_PERFORMANCE_TRACE',
    'WII_COMPILED_GPU_TIMING', 'WII_COMPILED_STACK_PROFILE_AT', 'capture_%Y%m%d',
    '[prime-uw-trace]', 'WII_COMPILED_PRIME_PROJECTION_TRACE', 'WII_COMPILED_PRIME_UW_FLUSH') | Where-Object { $exeText.Contains($_) }
if ($diagnostics) { throw ('The public executable still carries diagnostics: ' + ($diagnostics -join ', ')) }
# The player-facing tools must still be there.
$required = @('Performance', 'Effects', 'Materials', 'Diagnostics', 'Create bug report (F12)', 'Show FPS', 'rc_client') | Where-Object { -not $exeText.Contains($_) }
if ($required) { throw ('The public executable lost player features: ' + ($required -join ', ')) }
$exeText = $null
$fileVersion = (Get-Item -LiteralPath $exe).VersionInfo.FileVersion
if ($fileVersion -and $fileVersion -ne $version) {
    throw "The executable reports version $fileVersion but the header says $version; rebuild after bumping the version."
}
Copy-Item -LiteralPath $exe -Destination $stage
# Public executable: no debug sections (2.5 MB of DWARF that players do not need), and none of the
# diagnostic thread APIs (suspend, read context or memory, enumerate threads). Antivirus
# machine-learning engines associate that combination with malware (VirusTotal Trapmine flag on
# 0.1.2-0.1.4); the diagnostic build keeps them for the F8 profiler and the F9 thread list.
$llvmBin = Join-Path $root 'recomp\.tools\portable-tools\llvm-mingw\bin'
$stagedExe = Join-Path $stage 'Corruption_3 PC.exe'
& (Join-Path $llvmBin 'llvm-strip.exe') --strip-debug $stagedExe
if ($LASTEXITCODE -ne 0) { throw 'llvm-strip failed on the staged executable' }
$imports = & (Join-Path $llvmBin 'llvm-objdump.exe') -p $stagedExe | Out-String
# (CreateToolhelp32Snapshot and Module32FirstW stay: Rust's std inside libnod imports them for backtraces.)
$forbiddenImports = @('SuspendThread', 'GetThreadContext', 'ReadProcessMemory', 'Thread32First', 'Thread32Next') |
    Where-Object { $imports -match "\b$_\b" }
if ($forbiddenImports) { throw ('The public executable still imports diagnostic thread APIs: ' + ($forbiddenImports -join ', ')) }
foreach ($dll in 'SDL3.dll', 'dxcompiler.dll', 'dxil.dll', 'libc++.dll', 'libpng16.dll', 'libunwind.dll', 'libz.dll', 'webgpu_dawn.dll') {
    $path = Join-Path $build $dll
    if (-not (Test-Path -LiteralPath $path)) { throw "Runtime DLL missing: $path" }
    Copy-Item -LiteralPath $path -Destination $stage
}
foreach ($asset in 'dsp_coef.bin', 'initial_pipeline_cache.db') {
    $path = Join-Path $build $asset
    if (-not (Test-Path -LiteralPath $path)) { throw "Bundled asset missing: $path" }
    Copy-Item -LiteralPath $path -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $build 'wii_bootstrap') -Destination (Join-Path $stage 'wii_bootstrap') -Recurse
# Portable layout: UserData/ lives next to the executable instead of %LOCALAPPDATA%.
Set-Content -LiteralPath (Join-Path $stage 'portable.txt') -Value 'UserData, Reports and caches are kept in this folder.' -Encoding ASCII

# Default configuration. The runtime copies it to UserData/Config.toml on the first
# run only, so unzipping a new release over an old folder never touches player settings.
@'
# Corruption_3 PC - default configuration (copied to UserData/Config.toml on first run)
# Everything here can also be changed in-game: F1 opens the port options, F10 the renderer bar.
# The disc image path is asked once with a dialog and stored under [paths] iso_path.
# Controls and performance settings mirror the maintainer's own test sessions.

[video]
display_mode = "borderless"
graphics_api = "auto"
resolution_multiplier = 0
msaa = 1
anisotropic_filtering = 16
per_pixel_lighting = true
enhanced_metal = true
enhanced_metal_strength = 0.30
enhanced_metal_roughness = 0.25
enhanced_metal_metallic = 0.62
enhanced_metal_edge = 0.50
enhanced_metal_detail = 1.00
# Ship hulls and other identified surfaces get the same modern material (needs per-pixel lighting).
enhanced_materials = true
texture_upscale = 0
frame_interpolation_fps = 0
# V-Sync: wait for the monitor refresh (no tearing, a little more latency). F1 > Video.
vsync = false
disable_copy_filter = true
skip_unready_pipelines = true
# While a material's shader compiles, draw with a generic one that reads the material from memory
# instead of waiting (F1 > Performance). Compiled shaders and the cache keep priority.
ubershader = true
show_fps = false

[audio]
muted = false

[network]
enabled = false

[discord]
enabled = false

[paths]
# iso_path = "D:/Games/Metroid Prime 3 - Corruption (USA).iso"

[prime]
keyboard_mouse = true
keyboard_icons = true
hide_cursor = true
wiimote_battery_full = true
mouse_look = true
mouse_look_sensitivity = 0.2
mouse_look_smoothing = 0.01
mouse_look_invert_y = false
raw_pointer = true
sound_mode = "stereo"
host_audio_resampler = true
gpu_thread = true
overlap_encode = true
lazy_end_drain = true
aurora_decode_thread = true
defer_dl_expansion = false
present_queue_limit = true
present_on_copy = true
parallel_texture_decode = true
gpu_texture_decode = true
fast_disc_reads = true
async_disc_reads = true
check_updates = true

[effects]
# Aurora image effects (F1 > Effects). Ambient occlusion is on (contact shading in corners and
# where objects meet); it needs antialiasing (msaa) at 1. The other effects are off by default.
sharpen = false
fxaa = false
bloom = false
ssao = true
ssao_radius = 1.0
ssao_intensity = 0.70
ssao_softness = 2.0
ssao_samples = 8
ssao_bias = 1.2
ssao_power = 0.99
ssao_fade_distance = 0
ssao_edge_sharpness = 0.52
ssao_glow_protect = 1.0
# No ambient occlusion on Samus' face behind the visor and on her body out of the suit.
ssao_skip_face = true
# Fog, explosions and particles keep their colour: no occlusion of the surface behind them.
ssao_under_effects = true
# World lighting (experimental, each one on its own): environment material on the baked-light walls
# and floors, highlights from the room's character lights, and reflections on smooth floors (SSR).
env_material = true
env_borrowed_lights = true
ssr = true
ssr_strength = 0.7
ssr_roughness = 0.35
ssr_relief = 1.0
# More lighting improvements (experimental): material in lit rooms, smooth lightmaps, soft highlights,
# soft character shadows, occlusion on reflections. Area-coloured reflections, light shafts (medium
# GPU cost) and linear blending (changes the look of every material) start off.
lit_room_material = true
smooth_lightmaps = true
soft_highlights = true
area_reflections = false
# The game's glow without ghost copies at high resolution (softened glow source copy).
glow_ghost_fix = true
# Experimental reflections on smooth walls (screen-space, like the floor ones).
wall_reflections = true
wall_reflections_strength = 0.80
soft_shadows = true
light_shafts = false
light_shafts_intensity = 0.5
specular_occlusion = true
linear_lighting = false
color = false

[reshade]
# Loads reshade\ReShade64.dll at startup (Home opens its menu). Off by default.
enabled = false

[prime_keys]
plus = ["1"]
minus = ["2"]
one = ["Keypad 1"]
two = ["Keypad 2"]
z = ["Left Shift", "Right Shift"]
c = ["Left Ctrl", "Right Ctrl"]
twist_left = ["Z", "Keypad 7"]
twist_right = ["X", "Keypad 8"]
dpad_down = ["Mouse Middle"]
'@ | Set-Content -LiteralPath (Join-Path $stage 'Config.default.toml') -Encoding UTF8

# Standalone update check, for players who turned the in-game one off.
@"
# Corruption_3 PC - checks GitHub Releases for a newer build than the one in this folder.
`$ErrorActionPreference = 'Stop'
`$exe = Join-Path `$PSScriptRoot 'Corruption_3 PC.exe'
`$local = (Get-Item -LiteralPath `$exe).VersionInfo.FileVersion
# /releases/latest skips prereleases (all 0.x builds), so take the highest version from the list.
`$api = 'https://api.github.com/repos/bonllavier/Corruption_3-PC/releases?per_page=30'
try {
    `$releases = @(Invoke-RestMethod -Uri `$api -Headers @{ 'User-Agent' = "Corruption3PC/`$local"; Accept = 'application/vnd.github+json' })
    `$release = `$releases | ForEach-Object { `$_ } | Where-Object { -not `$_.draft } |
        Sort-Object { [version](`$_.tag_name.TrimStart('v')) } -Descending | Select-Object -First 1
    if (-not `$release) { Write-Output "No release published yet. Installed version: `$local"; exit 0 }
} catch {
    Write-Output "Could not reach GitHub (`$(`$_.Exception.Message)). Installed version: `$local"
    exit 1
}
`$latest = `$release.tag_name.TrimStart('v')
if ([version]`$latest -gt [version]`$local) {
    Write-Output "NEW VERSION AVAILABLE: v`$latest (installed: v`$local)"
    Write-Output "Download: `$(`$release.html_url)"
    `$answer = Read-Host 'Open the download page now? (y/n)'
    if (`$answer -match '^[yY]') { Start-Process `$release.html_url }
} else {
    Write-Output "Up to date: v`$local is the latest release."
}
"@ | Set-Content -LiteralPath (Join-Path $stage 'Check for updates.ps1') -Encoding UTF8

# The two start-up screens that replace the game's Wii Remote strap screen: original images, always shipped.
# The executable installs them into UserData/texture_replacements on start-up.
# RM3E01: 550dd776 (page 01/02), 0a2f3408 (02/02). Trilogy R3ME01 draws its own strap
# textures, so the same two images are shipped again under its hashes: ad15d2f8 (01/02),
# daf65c9d (02/02). The US disc also has French and Spanish strap textures (NoARAM.pak
# TXTR_StrapScreenA169FREN/SPAN, B169FREN/SPAN), drawn when [prime] language picks them;
# German and Italian use the English ones. The same two images cover them:
# ad8eb16a / 0dd8e2ea (01/02, FR/ES), f6c8146f / 9a1135bd (02/02, FR/ES).
$strapSource = Join-Path $root 'recomp\third_party\WiiCompiled\runtime\assets\texture_replacements\strap_screen'
$strapTarget = Join-Path $stage 'texture_replacements\strap_screen'
New-Item -ItemType Directory -Force -Path $strapTarget | Out-Null
foreach ($image in 'tex1_832x456_0a2f3408cab0cbcc_14.png', 'tex1_832x456_550dd77642afbb04_14.png',
                   'tex1_832x456_ad15d2f8f67a24d8_14.png', 'tex1_832x456_daf65c9d00a79c85_14.png',
                   'tex1_832x456_ad8eb16a5f40d2e2_14.png', 'tex1_832x456_0dd8e2ea1b2d67da_14.png',
                   'tex1_832x456_f6c8146f6a1af236_14.png', 'tex1_832x456_9a1135bd0715a8fe_14.png') {
    $path = Join-Path $strapSource $image
    if (-not (Test-Path -LiteralPath $path)) { throw "Start-up screen missing: $path" }
    Copy-Item -LiteralPath $path -Destination $strapTarget
}

# Bundled ReShade (BSD-3) with a curated effect set (see THIRD-PARTY-NOTICES.md); loaded only when enabled.
$reshade = Join-Path $root 'recomp/third_party/reshade'
if (-not (Test-Path -LiteralPath (Join-Path $reshade 'ReShade64.dll'))) { throw "Bundled ReShade missing: $reshade" }
Copy-Item -LiteralPath $reshade -Destination (Join-Path $stage 'reshade') -Recurse
Remove-Item -LiteralPath (Join-Path $stage 'reshade\ReShade.log') -ErrorAction SilentlyContinue
Get-ChildItem (Join-Path $stage 'reshade\Screenshots') -File -ErrorAction SilentlyContinue | Remove-Item

# Player-facing documentation, straight from the published repository.
foreach ($doc in 'README.md', 'CHANGELOG.md', 'LICENSE', 'THIRD-PARTY-NOTICES.md') {
    $path = Join-Path $publish $doc
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing document: $path" }
    Copy-Item -LiteralPath $path -Destination $stage
}
foreach ($doc in 'controls.md', 'development.md', 'troubleshooting.md') {
    $path = Join-Path $publish "docs\$doc"
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing document: $path" }
    Copy-Item -LiteralPath $path -Destination (Join-Path $stage 'docs')
}
if (-not (Select-String -Path (Join-Path $stage 'CHANGELOG.md') -Pattern "^## v$([regex]::Escape($version))\b" -Quiet)) {
    throw "CHANGELOG.md has no '## v$version' section; add one before packaging."
}

# Nothing derived from the game, and nothing of the player's, may ship.
$forbidden = Get-ChildItem -LiteralPath $stage -Recurse -Force | Where-Object {
    $_.Name -eq 'UserData' -or $_.Name -eq 'game' -or $_.Name -eq 'save.bin' -or $_.Name -eq 'banner.bin' -or
    $_.Extension -in '.iso', '.rvz', '.wbfs', '.nfs', '.gcm', '.dol', '.rel', '.rso', '.pak'
}
if ($forbidden) { throw "Refusing to package: game or player data in the staged folder: $($forbidden.FullName -join ', ')" }
$suspicious = Get-ChildItem -LiteralPath $stage -Recurse -File -Force | Where-Object { $_.Length -gt 200MB }
if ($suspicious) { throw "Refusing to package an unexpectedly large file: $($suspicious.FullName -join ', ')" }
# The developer's credentials never ship (user's instruction, 31/10/2026): RetroAchievements session and
# user name, GitHub and VirusTotal keys, Config.toml, logs, reports. In-process call: -File would split
# the path list. Checked again on the zip below.
$checkCredentials = Join-Path $PSScriptRoot 'check_no_credentials.ps1'
& $checkCredentials -Path @($stage)
if ($LASTEXITCODE -ne 0) { throw 'Refusing to package: credentials or private files in the staged folder (see above).' }

$zip = Join-Path $out "$name.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
& $checkCredentials -Path @($zip)
if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $zip -Force; throw 'Refusing to package: credentials or private files in the zip (see above); the zip was deleted.' }
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$size = [Math]::Round((Get-Item -LiteralPath $zip).Length / 1MB, 1)
Set-Content -LiteralPath "$zip.sha256" -Value "$hash  $name.zip" -Encoding ASCII
Write-Output "release: $zip ($size MB)"
Write-Output "sha256:  $hash"

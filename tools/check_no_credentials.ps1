<#
 Refuses (exit 1) when a folder or zip about to be published contains the developer's credentials.
   - Files that hold them: retroachievements.dat (RetroAchievements session), Config.toml, logs, Reports/.
   - The secrets themselves, searched as ASCII and UTF-16 in every file (and inside every zip entry):
       * RetroAchievements token and user name, decrypted here from each local retroachievements.dat
         (DPAPI, current Windows user, same entropy as hle/prime_achievements.cpp);
       * GITHUB_TOKEN and VIRUSTOTAL_API_KEY (process or user environment).
 Secrets are never printed or written anywhere: a hit names the file and the kind of secret only.
 Called by package_release.ps1 (staged folder and zip) and publish_release.ps1 (zip and publish/ repo).
 Usage: check_no_credentials.ps1 -Path <folder or .zip> [-Path ...]
#>
param([Parameter(Mandatory = $true)][string[]]$Path)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$latin1 = [System.Text.Encoding]::GetEncoding(28591)

# --- secrets known on this machine ------------------------------------------------------------
$secrets = New-Object System.Collections.Generic.List[object]
function Add-Secret([string]$kind, [string]$value, [bool]$textOnly) {
    if ([string]::IsNullOrWhiteSpace($value) -or $value.Length -lt 4) { return }
    $secrets.Add([pscustomobject]@{
        Kind = $kind; TextOnly = $textOnly
        Ascii = $latin1.GetString([System.Text.Encoding]::UTF8.GetBytes($value))
        Wide = $latin1.GetString([System.Text.Encoding]::Unicode.GetBytes($value))
    })
}

$entropy = [System.Text.Encoding]::ASCII.GetBytes('Corruption_3 PC/RetroAchievements/token')
$datFiles = @()
foreach ($dir in @((Join-Path $root 'recomp\build'), (Join-Path $root 'dist'), (Join-Path $env:LOCALAPPDATA 'WiiCompiled'))) {
    if (Test-Path -LiteralPath $dir) {
        $datFiles += Get-ChildItem -LiteralPath $dir -Recurse -Depth 4 -Filter 'retroachievements.dat' -File -Force -ErrorAction SilentlyContinue
    }
}
$raSessions = 0
foreach ($dat in $datFiles) {
    try {
        $plain = [System.Security.Cryptography.ProtectedData]::Unprotect(
            [System.IO.File]::ReadAllBytes($dat.FullName), $entropy, [System.Security.Cryptography.DataProtectionScope]::CurrentUser)
    } catch { continue }  # another Windows user's file: not ours to read
    $text = [System.Text.Encoding]::UTF8.GetString($plain)
    [Array]::Clear($plain, 0, $plain.Length)
    $split = $text.IndexOf("`n")
    if ($split -le 0) { continue }
    $user = $text.Substring(0, $split); $token = $text.Substring($split + 1)
    Add-Secret 'RetroAchievements token' $token $false
    # The user name is only searched in text files (short names could match binary data by chance),
    # and not when it is the public author name the port already ships.
    if ($user -ne 'bonllavier') { Add-Secret 'RetroAchievements user name' $user $true }
    $raSessions++
    $text = $null; $token = $null
}
foreach ($name in 'GITHUB_TOKEN', 'VIRUSTOTAL_API_KEY') {
    $values = @([Environment]::GetEnvironmentVariable($name, 'Process'), [Environment]::GetEnvironmentVariable($name, 'User')) |
        Where-Object { $_ } | Select-Object -Unique
    foreach ($v in $values) { Add-Secret $name $v $false }
}

# --- scan ---------------------------------------------------------------------------------------
$textExtensions = '.txt', '.md', '.toml', '.json', '.ps1', '.psm1', '.ini', '.cfg', '.log', '.xml', '.html', '.htm', '.yml', '.yaml', '.csv', '.bat', '.cmd', '.fx', '.fxh'
$forbiddenNames = 'retroachievements.dat', 'retroachievements.dat.tmp', 'Config.toml'
$forbiddenDirs = 'Reports', 'Logs', 'UserData'
$problems = New-Object System.Collections.Generic.List[string]
$scanned = 0

function Test-Content([string]$label, [byte[]]$bytes, [string]$extension) {
    $isText = $textExtensions -contains $extension.ToLowerInvariant()
    $haystack = $latin1.GetString($bytes)
    foreach ($s in $secrets) {
        if ($s.TextOnly -and -not $isText) { continue }
        if ($haystack.IndexOf($s.Ascii, [StringComparison]::Ordinal) -ge 0 -or
            $haystack.IndexOf($s.Wide, [StringComparison]::Ordinal) -ge 0) {
            $problems.Add("$label contains the $($s.Kind)")
        }
    }
}
function Test-Name([string]$label, [string]$relative) {
    $parts = $relative -split '[\\/]'
    if ($forbiddenNames -contains $parts[-1]) { $problems.Add("$label is a credentials/settings file that must not ship") }
    if ($parts[-1] -like '*.log') { $problems.Add("$label is a log file") }
    foreach ($p in $parts[0..([Math]::Max(0, $parts.Count - 2))]) { if ($forbiddenDirs -contains $p) { $problems.Add("$label is inside a $p folder"); break } }
}

foreach ($item in $Path) {
    if (-not (Test-Path -LiteralPath $item)) { throw "Nothing to check at $item" }
    $full = (Resolve-Path -LiteralPath $item).Path
    if ((Get-Item -LiteralPath $full).PSIsContainer) {
        $files = Get-ChildItem -LiteralPath $full -Recurse -File -Force | Where-Object { $_.FullName -notmatch '[\\/]\.git[\\/]' }
        foreach ($f in $files) {
            $relative = $f.FullName.Substring($full.Length).TrimStart('\', '/')
            Test-Name $f.FullName $relative
            Test-Content $f.FullName ([System.IO.File]::ReadAllBytes($f.FullName)) $f.Extension
            $scanned++
        }
    } else {
        $zip = [System.IO.Compression.ZipFile]::OpenRead($full)
        try {
            foreach ($entry in $zip.Entries) {
                if ($entry.FullName.EndsWith('/')) { continue }
                $label = "$full!$($entry.FullName)"
                Test-Name $label $entry.FullName
                $stream = $entry.Open(); $buffer = New-Object System.IO.MemoryStream
                try { $stream.CopyTo($buffer) } finally { $stream.Dispose() }
                Test-Content $label $buffer.ToArray() ([System.IO.Path]::GetExtension($entry.FullName))
                $buffer.Dispose()
                $scanned++
            }
        } finally { $zip.Dispose() }
    }
}
$secrets.Clear()

if ($problems.Count -gt 0) {
    Write-Output 'Refusing to publish: credentials or private files found.'
    $problems | Select-Object -Unique | ForEach-Object { Write-Output "  $_" }
    exit 1
}
Write-Output "credentials check: $scanned files clean (RetroAchievements sessions checked: $raSessions)"
exit 0

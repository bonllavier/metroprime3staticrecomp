param(
    [string]$Repository = 'bonllavier/Corruption_3-PC',
    [string]$PublishDirectory = 'publish',
    [string]$OutputDirectory = 'dist',
    [switch]$Draft,
    [switch]$SkipPackage,
    [switch]$SkipScan,
    [switch]$RequireLinux
)
# Versioned publication of Corruption_3 PC to GitHub, without the gh CLI:
#   1. reads the version from prime_port_version.h,
#   2. packages the two release zips (recomp/tools/package_release.ps1) unless -SkipPackage: the main one and
#      the Metroid Prime Trilogy add-on (decision of 16/11/2026, docs/publicacion_github.md),
#   3. commits the publish/ folder, tags it vX.Y.Z and pushes branch + tag,
#      The Linux archives (main and Trilogy add-on, .tar.gz from recomp/tools/package_release_linux.sh, copied
#      into <OutputDirectory>) are uploaded with them when present; -RequireLinux makes them mandatory.
#   4. scans every archive and executable with VirusTotal (recomp/tools/scan_release_virustotal.ps1)
#      and appends the public report to the notes, unless -SkipScan,
#   5. creates the GitHub Release for that tag (notes = the CHANGELOG section + SHA-256 + scan) and
#      uploads both zips and their .sha256 files through the REST API. An existing release gets its notes updated.
# Needs a personal access token with "contents: write" in $env:GITHUB_TOKEN and, for the scan, a
# VirusTotal API key in $env:VIRUSTOTAL_API_KEY. Both are read from the environment only.
# The executable is built locally on purpose: the build needs the translated game code
# derived from the player's disc, which cannot live on GitHub, so CI cannot produce it.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$publish = Join-Path $root $PublishDirectory
$header = Get-Content -LiteralPath (Join-Path $root 'recomp/third_party/WiiCompiled/runtime/include/prime_port_version.h') -Raw
if ($header -notmatch '#define PRIME_PORT_VERSION "([^"]+)"') { throw 'PRIME_PORT_VERSION not found' }
$version = $Matches[1]
$tag = "v$version"
$assetName = "Corruption_3-PC-$tag-win64.zip"
$zip = Join-Path $root "$OutputDirectory/$assetName"
$addonAssetName = "Corruption_3-PC-$tag-Trilogy-addon-win64.zip"
$addonZip = Join-Path $root "$OutputDirectory/$addonAssetName"
# Linux archives of the same version (built on Linux, copied here): uploaded when present.
$linuxAssetName = "Corruption_3-PC-$tag-linux-x86_64.tar.gz"
$linuxAddonAssetName = "Corruption_3-PC-$tag-Trilogy-addon-linux-x86_64.tar.gz"
$linuxArchive = Join-Path $root "$OutputDirectory/$linuxAssetName"
$linuxAddonArchive = Join-Path $root "$OutputDirectory/$linuxAddonAssetName"
$withLinux = (Test-Path -LiteralPath $linuxArchive) -and (Test-Path -LiteralPath $linuxAddonArchive)
if ($RequireLinux -and -not $withLinux) { throw "Linux archives missing in $OutputDirectory ($linuxAssetName, $linuxAddonAssetName)" }
if ($withLinux) {
    foreach ($archive in $linuxArchive, $linuxAddonArchive) {
        if (-not (Test-Path -LiteralPath "$archive.sha256")) { throw "Missing $archive.sha256" }
        $expected = ((Get-Content -LiteralPath "$archive.sha256" -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
        $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($expected -ne $actual) { throw "$archive does not match its .sha256" }
    }
}

# Secrets come from the environment: this window first, then the ones stored for this Windows user
# (setx GITHUB_TOKEN "..."). They are never written to a file or printed.
if (-not $env:GITHUB_TOKEN) { $env:GITHUB_TOKEN = [Environment]::GetEnvironmentVariable('GITHUB_TOKEN', 'User') }
if (-not $env:VIRUSTOTAL_API_KEY) { $env:VIRUSTOTAL_API_KEY = [Environment]::GetEnvironmentVariable('VIRUSTOTAL_API_KEY', 'User') }
if (-not $env:GITHUB_TOKEN) { throw 'Set GITHUB_TOKEN to a GitHub personal access token (repo contents: write), with setx or in this window.' }
if (-not $SkipScan -and -not $env:VIRUSTOTAL_API_KEY) { throw 'Set VIRUSTOTAL_API_KEY for the antivirus report in the release notes, or pass -SkipScan to publish without it.' }
if (-not $SkipPackage) { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'package_release.ps1') -PublishDirectory $PublishDirectory -OutputDirectory $OutputDirectory; if ($LASTEXITCODE -ne 0) { throw 'packaging failed' } }
if (-not (Test-Path -LiteralPath $zip)) { throw "Release asset missing: $zip" }
if (-not (Test-Path -LiteralPath $addonZip)) { throw "Release asset missing: $addonZip" }

# Gate before anything leaves this PC (VirusTotal upload, git push, GitHub upload): no credentials in the zip or in the repository folder
# (RetroAchievements session, GitHub and VirusTotal keys, Config.toml, logs, reports). Also covers
# -SkipPackage, where package_release.ps1 did not run.
$credentialPaths = @($zip, $addonZip, $publish)
if ($withLinux) { $credentialPaths += @($linuxArchive, $linuxAddonArchive) }
& (Join-Path $PSScriptRoot 'check_no_credentials.ps1') -Path $credentialPaths
if ($LASTEXITCODE -ne 0) { throw 'Refusing to publish: credentials or private files found (see above). Nothing was pushed or uploaded.' }

# Release notes: the CHANGELOG section of this version.
$changelog = Get-Content -LiteralPath (Join-Path $publish 'CHANGELOG.md') -Raw
$section = [regex]::Match($changelog, "(?s)## $([regex]::Escape($tag))[^\n]*\n(.*?)(?=\n## |\z)")
if (-not $section.Success) { throw "CHANGELOG.md has no '## $tag' section; add one before releasing." }
$notes = $section.Groups[1].Value.Trim()
$sha = (Get-Content -LiteralPath "$zip.sha256" -Raw).Trim()
$addonSha = (Get-Content -LiteralPath "$addonZip.sha256" -Raw).Trim()
$downloads = "`n`n**Downloads**`n`n| | Main (Metroid Prime 3: Corruption USA) | Optional Metroid Prime Trilogy (USA) add-on |`n|---|---|---|"
$downloads += "`n| Windows | ``$assetName`` | ``$addonAssetName`` |"
if ($withLinux) { $downloads += "`n| Linux x86_64 | ``$linuxAssetName`` | ``$linuxAddonAssetName`` |" }
$downloads += "`n`nThe add-on holds only the two Trilogy executables: extract it into the folder of the main archive of the same version."
$notes += $downloads
$shaLines = @($sha, $addonSha)
if ($withLinux) { $shaLines += @((Get-Content -LiteralPath "$linuxArchive.sha256" -Raw).Trim(), (Get-Content -LiteralPath "$linuxAddonArchive.sha256" -Raw).Trim()) }
$notes += "`n`n**SHA-256**`n``````n" + ($shaLines -join "`n") + "`n``````"

# Antivirus report: the zip players download and the executable inside it, scanned by VirusTotal.
if (-not $SkipScan) {
    $stagedExe = Join-Path $root "$OutputDirectory/Corruption_3-PC-$tag-win64/Corruption_3 PC.exe"
    if (-not (Test-Path -LiteralPath $stagedExe)) { throw "Staged executable missing: $stagedExe (run without -SkipPackage)" }
    $scanReport = Join-Path $root "$OutputDirectory/$assetName.virustotal.md"
    # In-process call: a child powershell.exe would split the two paths into separate parameters.
    $scanned = @($zip, $addonZip, $stagedExe)
    if ($withLinux) {
        # The archives and their three ELF binaries, unpacked to a temporary folder for the scan.
        $scanned += @($linuxArchive, $linuxAddonArchive)
        $unpacked = Join-Path $env:TEMP "c3_linux_scan_$version"
        if (Test-Path -LiteralPath $unpacked) { Remove-Item -LiteralPath $unpacked -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $unpacked | Out-Null
        foreach ($archive in $linuxArchive, $linuxAddonArchive) { & (Join-Path $env:SystemRoot 'System32\tar.exe') -xzf $archive -C $unpacked; if ($LASTEXITCODE -ne 0) { throw "tar failed on $archive" } }
        foreach ($binary in 'Corruption_3 PC', 'Corruption_3 Trilogy Menu', 'Corruption_3 Trilogy MP3') {
            $found = Get-ChildItem -LiteralPath $unpacked -Recurse -File -Filter $binary | Select-Object -First 1
            if (-not $found) { throw "Linux binary missing in the archives: $binary" }
            $scanned += $found.FullName
        }
    }
    foreach ($name in 'Corruption_3 Trilogy Menu.exe', 'Corruption_3 Trilogy MP3.exe') {
        $trilogyExe = Join-Path $root "$OutputDirectory/Corruption_3-PC-$tag-Trilogy-addon-win64/$name"
        if (-not (Test-Path -LiteralPath $trilogyExe)) { throw "Staged executable missing: $trilogyExe (run without -SkipPackage)" }
        $scanned += $trilogyExe
    }
    & (Join-Path $PSScriptRoot 'scan_release_virustotal.ps1') -Path $scanned -ReportPath $scanReport | Out-Null
    if (-not (Test-Path -LiteralPath $scanReport)) { throw 'VirusTotal scan produced no report; fix it or pass -SkipScan' }
    $notes += "`n`n" + (Get-Content -LiteralPath $scanReport -Raw).Trim()
}

# Git: commit publish/, tag, push.
Push-Location $publish
# git writes progress to stderr; under Windows PowerShell 5.1 with Stop that counts as an error.
# Only the exit codes decide here.
$savedPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    if (-not (Test-Path '.git')) {
        git init -b main | Out-Null
        git remote add origin "https://github.com/$Repository.git"
    }
    git add -A
    git diff --cached --quiet
    if ($LASTEXITCODE -ne 0) { git commit -m "Release $tag" | Out-Null; Write-Output "committed: Release $tag" }
    $existing = git tag -l $tag
    if (-not $existing) { git tag -a $tag -m "Corruption_3 PC $tag"; Write-Output "tagged $tag" }
    git push -u origin main
    if ($LASTEXITCODE -ne 0) { throw 'git push failed' }
    git push origin $tag
    if ($LASTEXITCODE -ne 0) { throw 'tag push failed' }
} finally { $ErrorActionPreference = $savedPreference; Pop-Location }

# GitHub Release through the REST API.
$headers = @{ Authorization = "Bearer $env:GITHUB_TOKEN"; Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28'; 'User-Agent' = 'Corruption3PC-release-script' }
$api = "https://api.github.com/repos/$Repository"
$release = $null
# Drafts are not returned by /releases/tags/{tag}; the tag-push workflow may already have made one.
try { $release = @(Invoke-RestMethod -Headers $headers -Uri "$api/releases?per_page=50") | ForEach-Object { $_ } | Where-Object { $_.tag_name -eq $tag } | Select-Object -First 1 } catch { $release = $null }
if (-not $release) {
    $body = @{ tag_name = $tag; name = "Corruption_3 PC $tag"; body = $notes; draft = [bool]$Draft; prerelease = ($version -like '0.*') } | ConvertTo-Json
    $release = Invoke-RestMethod -Method Post -Headers $headers -Uri "$api/releases" -Body $body -ContentType 'application/json'
    Write-Output "release created: $($release.html_url)"
} else {
    # Re-runs refresh the notes too, so a scan added later reaches the page.
    $body = @{ body = $notes } | ConvertTo-Json
    Invoke-RestMethod -Method Patch -Headers $headers -Uri "$api/releases/$($release.id)" -Body $body -ContentType 'application/json' | Out-Null
    Write-Output "release exists, notes updated: $($release.html_url)"
}
foreach ($asset in @($release.assets)) {
    if ($asset.name -in @($assetName, "$assetName.sha256", $addonAssetName, "$addonAssetName.sha256",
                          $linuxAssetName, "$linuxAssetName.sha256", $linuxAddonAssetName, "$linuxAddonAssetName.sha256")) { Invoke-RestMethod -Method Delete -Headers $headers -Uri $asset.url | Out-Null; Write-Output "replaced asset $($asset.name)" }
}
$uploadBase = ($release.upload_url -replace '\{\?name,label\}', '')
$uploads = @($zip, "$zip.sha256", $addonZip, "$addonZip.sha256")
if ($withLinux) { $uploads += @($linuxArchive, "$linuxArchive.sha256", $linuxAddonArchive, "$linuxAddonArchive.sha256") }
foreach ($file in $uploads) {
    $name = [IO.Path]::GetFileName($file)
    $contentType = if ($file.EndsWith('.zip')) { 'application/zip' } elseif ($file.EndsWith('.tar.gz')) { 'application/gzip' } else { 'text/plain' }
    Invoke-RestMethod -Method Post -Headers $headers -Uri "$uploadBase`?name=$name" -InFile $file -ContentType $contentType | Out-Null
    Write-Output "uploaded $name"
}
Write-Output "done: $($release.html_url)"

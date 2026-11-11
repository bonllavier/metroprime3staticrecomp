param(
    [Parameter(Mandatory = $true)][string[]]$Path,
    [string]$ReportPath = '',
    [int]$TimeoutMinutes = 20
)
# Scans release files with VirusTotal (public API v3) and writes a Markdown report meant for the
# GitHub Release notes: detections per file, the permalink anyone can check, and the engines that
# flagged something. Needs a VirusTotal API key in $env:VIRUSTOTAL_API_KEY (free account: 500 lookups
# a day, 4 a minute). The key is read from the environment only and never written anywhere.
#
# A file already known to VirusTotal is only looked up by its SHA-256. An unknown one is uploaded:
# anything uploaded becomes part of VirusTotal's corpus (its customers can download it), so only
# publishable files belong here (the release zip and the executable; never a disc image or save).
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
# Process variable first, then the one stored for this Windows user (setx VIRUSTOTAL_API_KEY "..."), so
# the script works from any window. The value is only ever used as a request header.
$key = $env:VIRUSTOTAL_API_KEY
if (-not $key) { $key = [Environment]::GetEnvironmentVariable('VIRUSTOTAL_API_KEY', 'User') }
if (-not $key) { $key = [Environment]::GetEnvironmentVariable('VIRUSTOTAL_API_KEY', 'Machine') }
if (-not $key) { throw 'Set VIRUSTOTAL_API_KEY (VirusTotal > profile > API key), with setx or in this window.' }
$headers = @{ 'x-apikey' = $key; 'User-Agent' = 'Corruption3PC-release-script' }
$api = 'https://www.virustotal.com/api/v3'

function Invoke-Vt([string]$Method, [string]$Uri, [hashtable]$Extra = @{}) {
    try { return Invoke-RestMethod -Method $Method -Headers $headers -Uri $Uri @Extra }
    catch {
        $status = $null
        try { $status = [int]$_.Exception.Response.StatusCode } catch {}
        if ($status -eq 404) { return $null }
        if ($status -eq 429) { Start-Sleep -Seconds 60; return Invoke-RestMethod -Method $Method -Headers $headers -Uri $Uri @Extra }
        throw
    }
}

function Upload-File([string]$file) {
    $size = (Get-Item -LiteralPath $file).Length
    $target = "$api/files"
    if ($size -gt 32MB) {
        # Files above 32 MB go through a one-time upload URL (up to 650 MB).
        $target = (Invoke-Vt 'Get' "$api/files/upload_url").data
        if (-not $target) { throw 'VirusTotal did not return an upload URL for a large file (the key may not allow it).' }
    }
    # PowerShell 5.1 has no multipart form support: build the body by hand.
    $boundary = [Guid]::NewGuid().ToString()
    $name = [IO.Path]::GetFileName($file)
    $head = "--$boundary`r`nContent-Disposition: form-data; name=`"file`"; filename=`"$name`"`r`nContent-Type: application/octet-stream`r`n`r`n"
    $tail = "`r`n--$boundary--`r`n"
    $bytes = [IO.File]::ReadAllBytes($file)
    $stream = New-Object IO.MemoryStream
    $enc = [Text.Encoding]::ASCII
    $stream.Write($enc.GetBytes($head), 0, $enc.GetByteCount($head))
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Write($enc.GetBytes($tail), 0, $enc.GetByteCount($tail))
    $body = $stream.ToArray()
    $response = Invoke-RestMethod -Method Post -Headers $headers -Uri $target -Body $body -ContentType "multipart/form-data; boundary=$boundary"
    return $response.data.id  # analysis id
}

function Wait-Analysis([string]$analysisId) {
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    while ((Get-Date) -lt $deadline) {
        $analysis = Invoke-Vt 'Get' "$api/analyses/$analysisId"
        if ($analysis.data.attributes.status -eq 'completed') { return $true }
        Start-Sleep -Seconds 30
    }
    return $false
}

$sections = @()
foreach ($file in $Path) {
    $file = (Resolve-Path -LiteralPath $file).Path
    $name = [IO.Path]::GetFileName($file)
    $sha = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host "== $name ($sha)"
    $report = Invoke-Vt 'Get' "$api/files/$sha"
    if (-not $report) {
        Write-Host '   unknown to VirusTotal: uploading'
        $analysisId = Upload-File $file
        if (-not (Wait-Analysis $analysisId)) { throw "VirusTotal analysis of $name did not finish within $TimeoutMinutes minutes" }
        $report = Invoke-Vt 'Get' "$api/files/$sha"
        if (-not $report) { throw "VirusTotal has no report for $name after the analysis" }
    } elseif ($report.data.attributes.last_analysis_results.PSObject.Properties.Count -eq 0) {
        throw "VirusTotal knows $name but holds no analysis; re-run later"
    }
    $a = $report.data.attributes
    $stats = $a.last_analysis_stats
    $engines = @($a.last_analysis_results.PSObject.Properties | Where-Object { $_.Value.category -in 'malicious', 'suspicious' } |
        ForEach-Object { "$($_.Name): $($_.Value.result)" })
    $total = [int]$stats.malicious + [int]$stats.suspicious + [int]$stats.undetected + [int]$stats.harmless
    $date = [DateTimeOffset]::FromUnixTimeSeconds([int64]$a.last_analysis_date).UtcDateTime.ToString('yyyy-MM-dd HH:mm') + ' UTC'
    $flagged = [int]$stats.malicious + [int]$stats.suspicious
    $line = "- **$name**: $flagged of $total engines flagged it ($date). [VirusTotal report](https://www.virustotal.com/gui/file/$sha)"
    if ($engines.Count -gt 0) { $line += "`n  - Flagged by: " + ($engines -join '; ') }
    $sections += $line
    Write-Host "   $flagged / $total flagged"
}
$markdown = "**Antivirus scan (VirusTotal)**`n`n" + ($sections -join "`n") + "`n`nAn unsigned executable built with LLVM/MinGW sometimes gets heuristic flags from a few engines; the reports above are public and can be re-checked at any time. The build contains no game data: the game loads from your own disc image."
if ($ReportPath) { [IO.File]::WriteAllText($ReportPath, $markdown + "`n", [Text.UTF8Encoding]::new($false)); Write-Host "report: $ReportPath" }
$markdown

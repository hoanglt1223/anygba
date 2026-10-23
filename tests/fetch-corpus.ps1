# Downloads the test corpus listed in corpus.lock and verifies every SHA-256.
# usage: tests/fetch-corpus.ps1 [-Dest DIR]     (default: tests/corpus)
# Works on Windows PowerShell 5.1 and PowerShell 7.
[CmdletBinding()]
param(
    [string]$Dest = ''
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # the progress bar makes 5.1 downloads crawl
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$lockFile = Join-Path $PSScriptRoot 'corpus.lock'
if (-not $Dest) { $Dest = Join-Path $PSScriptRoot 'corpus' }
$Dest = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Dest)
$archives = Join-Path $Dest '.archives'
$tmpFile = Join-Path $Dest '.download.tmp'
New-Item -ItemType Directory -Force -Path $archives | Out-Null

function Get-Sha256([string]$Path) {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

# Downloads $Url to $tmpFile and checks it against $Hash. Returns $false on any failure.
function Invoke-VerifiedDownload([string]$Url, [string]$Hash) {
    Remove-Item -LiteralPath $tmpFile -Force -ErrorAction SilentlyContinue
    $downloaded = $false
    for ($attempt = 1; $attempt -le 3 -and -not $downloaded; $attempt++) {
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $tmpFile
            $downloaded = $true
        } catch {
            Write-Warning "  attempt $attempt failed for ${Url}: $($_.Exception.Message)"
            Start-Sleep -Seconds 2
        }
    }
    if (-not $downloaded) {
        Write-Host "  download failed: $Url" -ForegroundColor Red
        return $false
    }
    $actual = Get-Sha256 $tmpFile
    if ($actual -ne $Hash) {
        Write-Host "  hash mismatch for $Url`n    expected $Hash`n    actual   $actual" -ForegroundColor Red
        Remove-Item -LiteralPath $tmpFile -Force
        return $false
    }
    return $true
}

$failures = 0
$count = 0
$lineNo = 0
try {
    foreach ($raw in Get-Content -LiteralPath $lockFile) {
        $lineNo++
        $line = $raw.Trim()
        if ($line -eq '' -or $line.StartsWith('#')) { continue }

        $fields = @($line -split '\s+')
        if ($fields.Count -ne 4) {
            Write-Host "corpus.lock:${lineNo}: expected 4 fields, got $($fields.Count)" -ForegroundColor Red
            $failures++
            continue
        }
        $kind, $hash, $path, $url = $fields
        if ([IO.Path]::IsPathRooted($path) -or $path.Contains('..')) {
            Write-Host "corpus.lock:${lineNo}: unsafe path: $path" -ForegroundColor Red
            $failures++
            continue
        }
        if ($hash -cnotmatch '^[0-9a-f]{64}$') {
            Write-Host "corpus.lock:${lineNo}: invalid sha256: $hash" -ForegroundColor Red
            $failures++
            continue
        }
        $count++
        $target = Join-Path $Dest $path

        switch ($kind) {
            'file' {
                if ((Test-Path -LiteralPath $target -PathType Leaf) -and (Get-Sha256 $target) -eq $hash) {
                    Write-Host "ok      $path"
                    break
                }
                Write-Host "fetch   $path"
                if (Invoke-VerifiedDownload $url $hash) {
                    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
                    Move-Item -LiteralPath $tmpFile -Destination $target -Force
                } else {
                    $failures++
                }
            }
            'zip' {
                $archive = Join-Path $archives "$hash.zip"
                $stamp = Join-Path $target '.corpus-sha256'
                if ((Test-Path -LiteralPath $stamp) -and (Get-Content -Raw -LiteralPath $stamp) -eq $hash) {
                    Write-Host "ok      $path/"
                    break
                }
                if (-not (Test-Path -LiteralPath $archive) -or (Get-Sha256 $archive) -ne $hash) {
                    Write-Host "fetch   $path/"
                    if (-not (Invoke-VerifiedDownload $url $hash)) {
                        $failures++
                        break
                    }
                    Move-Item -LiteralPath $tmpFile -Destination $archive -Force
                }
                if (Test-Path -LiteralPath $target) {
                    Remove-Item -LiteralPath $target -Recurse -Force
                }
                try {
                    Expand-Archive -LiteralPath $archive -DestinationPath $target -Force
                    [IO.File]::WriteAllText($stamp, $hash)
                    Write-Host "extract $path/"
                } catch {
                    Write-Host "  extraction failed: $archive ($($_.Exception.Message))" -ForegroundColor Red
                    Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction SilentlyContinue
                    $failures++
                }
            }
            default {
                Write-Host "corpus.lock:${lineNo}: unknown kind: $kind" -ForegroundColor Red
                $failures++
            }
        }
    }
} finally {
    Remove-Item -LiteralPath $tmpFile -Force -ErrorAction SilentlyContinue
}

if ($failures -ne 0) {
    Write-Host "corpus: $failures error(s) while checking $count entries" -ForegroundColor Red
    exit 1
}
Write-Host "corpus: $count entries verified in $Dest"

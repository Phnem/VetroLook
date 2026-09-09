<#
    Fetches the public RAW fixtures the benchmarks in REPORT.md use.

    They are not committed: together they are about 280 MB. They come from
    raw.pixls.us, a sample set contributed by photographers explicitly for
    software testing and published under CC0. No personal photographs are used
    for published benchmarks.

    Usage:  .\tests\get-raw-fixtures.ps1
#>
param([string]$Destination = (Join-Path $PSScriptRoot 'fixtures/raw-public'))
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Destination | Out-Null

$fixtures = @(
    @{ Name = 'Nikon-D850-12bit-uncompressed-72M.NEF'
       Url  = 'https://raw.pixls.us/data/Nikon/D850/Nikon-D850-12bit-uncompressed.NEF'
       Note = 'Nikon D850, 12-bit uncompressed, ~72 MB' }
    @{ Name = 'Nikon-D850-14bit-lossless-53M.NEF'
       Url  = 'https://raw.pixls.us/data/Nikon/D850/Nikon-D850-14bit-lossless-compressed.NEF'
       Note = 'Nikon D850, 14-bit lossless compressed, ~53 MB' }
    @{ Name = 'Leica-M10-33M.dng'
       Url  = 'https://raw.pixls.us/data/Leica/M10/f5381888.dng'
       Note = 'Leica M10, ~33 MB' }
    @{ Name = 'Pentax-645Z-69M.DNG'
       Url  = 'https://raw.pixls.us/data/Pentax/645Z/IMGP2836.DNG'
       Note = 'Pentax 645Z, ~69 MB' }
)

foreach ($f in $fixtures) {
    $path = Join-Path $Destination $f.Name
    if ((Test-Path -LiteralPath $path) -and (Get-Item -LiteralPath $path).Length -gt 1MB) {
        '{0,-42} present, {1:N1} MB' -f $f.Name, ((Get-Item -LiteralPath $path).Length / 1MB)
        continue
    }
    Write-Host ('fetching {0} ({1})...' -f $f.Name, $f.Note)
    try {
        Invoke-WebRequest -Uri $f.Url -OutFile $path -TimeoutSec 900 -UseBasicParsing
        '{0,-42} {1:N1} MB' -f $f.Name, ((Get-Item -LiteralPath $path).Length / 1MB)
    } catch {
        Write-Warning ('{0}: {1}' -f $f.Name, $_.Exception.Message)
    }
}

@"
Public RAW fixtures, downloaded from https://raw.pixls.us/ by
tests/get-raw-fixtures.ps1.

The raw.pixls.us sample set is contributed by photographers explicitly for
software testing and is published under CC0 (public domain dedication).
See https://raw.pixls.us/ for the collection's terms.

These files exist so the RAW benchmarks in REPORT.md can be reproduced on
another machine. No personal photographs are used for published benchmarks.

They are NOT committed to the repository.
"@ | Set-Content (Join-Path $Destination 'PROVENANCE.txt') -Encoding UTF8

Write-Host ''
Write-Host 'Benchmarks that use these:'
Write-Host '  build\Release\VetroBench.exe --raw <files>            RAW ladder, p50/p95'
Write-Host '  dist\VetroLook.exe --nav-bench <log> <folder> 32 200  navigation percentiles'

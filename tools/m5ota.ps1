$ErrorActionPreference = "Stop"

if ($env:PROCESSOR_ARCHITECTURE -ne "AMD64") {
    throw "m5ota: no binary for Windows $env:PROCESSOR_ARCHITECTURE"
}

$projectDir = Split-Path -Parent $PSScriptRoot
$cacheRoot = Join-Path $projectDir ".cache\m5ota"
$current = Join-Path $cacheRoot "current"

try {
    $version = (Invoke-RestMethod "https://api.github.com/repos/mk124/m5ota/releases/latest").tag_name
} catch {
    if (-not (Test-Path $current)) {
        throw "m5ota: could not find the latest release"
    }
    $version = (Get-Content -Raw $current).Trim()
    Write-Warning "m5ota: using cached $version"
}

$asset = "m5ota-$version-windows-amd64.zip"
$cacheDir = Join-Path $cacheRoot $version
$binary = Join-Path $cacheDir "m5ota.exe"

if (-not (Test-Path $binary)) {
    $archive = Join-Path $cacheDir $asset
    $checksums = Join-Path $cacheDir "SHA256SUMS"
    New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
    Remove-Item -Force -ErrorAction SilentlyContinue $archive, $checksums

    Write-Host "Downloading m5ota $version..."
    $release = "https://github.com/mk124/m5ota/releases/download/$version"
    Invoke-WebRequest "$release/SHA256SUMS" -OutFile $checksums
    $entry = Get-Content $checksums | Where-Object { $_.EndsWith("  $asset") } | Select-Object -First 1
    if (-not $entry) {
        throw "m5ota: $asset is missing from SHA256SUMS"
    }
    $checksum = $entry.Substring(0, 64)
    Invoke-WebRequest "$release/$asset" -OutFile $archive

    $actual = (Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant()
    if ($actual -ne $checksum) {
        Remove-Item -Force $archive
        throw "m5ota: checksum mismatch for $asset"
    }

    Expand-Archive -Force -Path $archive -DestinationPath $cacheDir
    Remove-Item -Force $archive, $checksums
}

Set-Content -Encoding ASCII -Path $current -Value $version
& $binary @args
exit $LASTEXITCODE

# Get-FFmpeg.ps1
# Ensures ffmpeg.exe is present in $OutDir for a self-contained Windows build.
# Called as a POST_BUILD step by CMake.
#
# Resolution order:
#   1. ffmpeg.exe already in $OutDir - done, nothing to do
#   2. ffmpeg.exe found on current process PATH - copy it to $OutDir
#   3. ffmpeg.exe on registry PATH (avoids stale inherited env) - copy
#   4. ffmpeg.exe found in WinGet packages folder - copy
#   5. Download from official GyanD/codexffmpeg essentials build

param(
    [Parameter(Mandatory = $false)]
    [string]$OutDir = $PSScriptRoot
)

$ErrorActionPreference = "Stop"

$dest = Join-Path $OutDir "ffmpeg.exe"
if (Test-Path $dest) {
    Write-Host "[Get-FFmpeg] ffmpeg.exe already present: $dest"
    exit 0
}

# Helper: search a semicolon-separated PATH string for ffmpeg.exe
function Find-InPath([string]$pathVar) {
    if (-not $pathVar) { return $null }
    foreach ($dir in ($pathVar -split ";")) {
        $d = $dir.Trim()
        if (-not $d) { continue }
        $candidate = Join-Path $d "ffmpeg.exe"
        if (Test-Path $candidate) { return $candidate }
    }
    return $null
}

# 1. Try current process PATH first
$found = Find-InPath $env:PATH
if ($found) {
    Copy-Item $found $dest -Force
    Write-Host "[Get-FFmpeg] Copied from process PATH: $found"
    exit 0
}

# 2. Try registry PATH (avoids stale inherited environment)
$sysPath  = [Environment]::GetEnvironmentVariable("PATH", "Machine")
$userPath = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($sysPath -or $userPath) {
    $found = Find-InPath ($sysPath + ";" + $userPath)
    if ($found) {
        Copy-Item $found $dest -Force
        Write-Host "[Get-FFmpeg] Copied from registry PATH: $found"
        exit 0
    }
}

# 3. Check well-known WinGet package location
$wingetBase = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
if (Test-Path $wingetBase) {
    $found = Get-ChildItem -Path $wingetBase -Filter "ffmpeg.exe" -Recurse -ErrorAction SilentlyContinue |
             Where-Object { $_.FullName -match "\\bin\\ffmpeg\.exe$" } |
             Select-Object -First 1 -ExpandProperty FullName
    if ($found) {
        Copy-Item $found $dest -Force
        Write-Host "[Get-FFmpeg] Copied from WinGet packages: $found"
        exit 0
    }
}

# 4. Download FFmpeg essentials build (~80 MB, ffmpeg/ffprobe/ffplay only)
$version = "8.1"
$url  = "https://github.com/GyanD/codexffmpeg/releases/download/$version/ffmpeg-$version-essentials_build.zip"
$zip  = Join-Path $env:TEMP "mc-ffmpeg-essentials.zip"
$extr = Join-Path $env:TEMP "mc-ffmpeg-extract"

Write-Host "[Get-FFmpeg] ffmpeg.exe not found locally - downloading essentials build v$version..."
Write-Host "[Get-FFmpeg] Source: $url"
Write-Host "[Get-FFmpeg] This only happens once; the binary is kept in the build output."

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# Clean up any previous partial download
Remove-Item $zip  -Force -ErrorAction SilentlyContinue
Remove-Item $extr -Recurse -Force -ErrorAction SilentlyContinue

Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
Write-Host "[Get-FFmpeg] Download complete. Extracting..."

Expand-Archive -Path $zip -DestinationPath $extr -Force

$exe = Get-ChildItem -Path $extr -Filter "ffmpeg.exe" -Recurse |
       Select-Object -First 1 -ExpandProperty FullName

if (-not $exe) {
    Remove-Item $zip, $extr -Recurse -Force -ErrorAction SilentlyContinue
    Write-Error "[Get-FFmpeg] ffmpeg.exe not found in the extracted archive - unexpected archive layout."
    exit 1
}

Copy-Item $exe $dest -Force
Remove-Item $zip, $extr -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "[Get-FFmpeg] ffmpeg.exe placed at: $dest"


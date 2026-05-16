# MultiCoder Windows Install / Uninstall PowerShell Script
# Run as Administrator.
# Usage:
#   .\windows-install.ps1 -InstallDir "C:\MultiCoder" [-EncoderCount 5]
#   .\windows-install.ps1 -Uninstall -InstallDir "C:\MultiCoder"

param(
    [string]$InstallDir = "C:\MultiCoder",
    [int]$EncoderCount  = 5,
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

if ($Uninstall) {
    Write-Host "=== MultiCoder Uninstall ===" -ForegroundColor Yellow
    # Stop services
    foreach ($svc in @("MultiCoder-Supervisor") + (1..$EncoderCount | ForEach-Object { "MultiCoder-Worker$_" })) {
        if (Get-Service -Name $svc -ErrorAction SilentlyContinue) {
            Stop-Service -Name $svc -Force -ErrorAction SilentlyContinue
            sc.exe delete $svc | Out-Null
            Write-Host "  Removed service: $svc"
        }
    }
    if (Test-Path $InstallDir) {
        Remove-Item -Recurse -Force $InstallDir
        Write-Host "  Removed: $InstallDir"
    }
    Write-Host "Uninstall complete." -ForegroundColor Green
    exit 0
}

Write-Host "=== MultiCoder Install ===" -ForegroundColor Cyan
Write-Host "  Install directory : $InstallDir"
Write-Host "  Encoder count     : $EncoderCount"

# ---- Create directory tree ----
$dirs = @(
    "$InstallDir\logs",
    "$InstallDir\config"
)
for ($i = 1; $i -le $EncoderCount; $i++) {
    $dirs += "$InstallDir\encoder$i\logs"
    $dirs += "$InstallDir\encoder$i\hls"
    $dirs += "$InstallDir\encoder$i\hls\segments"
}
foreach ($d in $dirs) {
    if (-not (Test-Path $d)) {
        New-Item -ItemType Directory -Path $d | Out-Null
        Write-Host "  Created: $d"
    }
}

# ---- Write default system.json ----
$systemJson = @{
    uiPort           = 8050
    logLevel         = "info"
    logRotSize       = 10
    logRetention     = 14
    encoderCount     = $EncoderCount
    adminUser        = "Admin"
    adminPass        = "change-me"
    firstLoginRequired = $true
    scteGlobalEnabled = $true
    scteGlobalRateLimitCount = 5
    scteGlobalRateLimitWindowSec = 10
    scteGlobalDedupeSeconds = 30
    scteLogLevel = "info"
    iceURL           = "http://localhost:8000"
    iceMountAAC      = "/stream-aac"
    iceMountMP3      = "/stream-mp3"
    interfaces       = @{}
} | ConvertTo-Json -Depth 5
$sysPath = "$InstallDir\system.json"
if (-not (Test-Path $sysPath)) {
    $systemJson | Set-Content -Path $sysPath -Encoding UTF8
    Write-Host "  Created: $sysPath"
}

# ---- Write per-encoder default configs ----
for ($i = 1; $i -le $EncoderCount; $i++) {
    $cfgDir = "$InstallDir\encoder$i"

    $inputJson = @{
        inputType   = "rtp"; rtpAddress = "239.192.0.0"; rtpPort = 5004
        rtpInterface= "Ethernet"; rtpGain = 0.0; bitrate = 128000; sampleRate = 48000
    } | ConvertTo-Json
    if (-not (Test-Path "$cfgDir\input.json")) { $inputJson | Set-Content "$cfgDir\input.json" -Encoding UTF8 }

    $aacJson = @{ url = "http://localhost:8000/stream$i-aac"; user = "source"; pass = "hackme"
                  icyMetaInt = 8192; stationId = "Encoder-$i"; metaEnabled = $true } | ConvertTo-Json
    if (-not (Test-Path "$cfgDir\aac.json")) { $aacJson | Set-Content "$cfgDir\aac.json" -Encoding UTF8 }

    $ctlJson = @{
        controlPort = 9010 + ($i - 1) * 10; controlEnabled = $true
        commands = @{ startAAC="StartAAC"; stopAAC="StopAAC"; startMP3="StartMP3"; stopMP3="StopMP3"
                      startHLS="StartHLS"; stopHLS="StopHLS"; startSRT="StartSRT"; stopSRT="StopSRT" }
    } | ConvertTo-Json -Depth 5
    if (-not (Test-Path "$cfgDir\control.json")) { $ctlJson | Set-Content "$cfgDir\control.json" -Encoding UTF8 }
}

Write-Host ""
Write-Host "=== Directory tree created ===" -ForegroundColor Green

# ---- Firewall rules ----
Write-Host ""
Write-Host "=== Configuring Windows Firewall ===" -ForegroundColor Cyan
$fwRules = @(
    @{ Name = "MultiCoder Supervisor UI (8050)"; Port = 8050 },
    @{ Name = "MultiCoder Control Base (9010)";  Port = 9010 }
)
# Add one HLS playback port per encoder (9015, 9025, 9035, ...)
for ($i = 1; $i -le $EncoderCount; $i++) {
    $hlsPort = 9010 + ($i - 1) * 10 + 5
    $fwRules += @{ Name = "MultiCoder HLS Port $hlsPort (Encoder $i)"; Port = $hlsPort }
}
foreach ($rule in $fwRules) {
    $existing = netsh advfirewall firewall show rule name="$($rule.Name)" 2>&1
    if ($existing -match "No rules match") {
        netsh advfirewall firewall add rule `
            name="$($rule.Name)" protocol=TCP dir=in localport="$($rule.Port)" action=allow | Out-Null
        Write-Host "  Added firewall rule: $($rule.Name)"
    } else {
        Write-Host "  Firewall rule already exists: $($rule.Name)"
    }
}

# Axia Livewire / generic RTP multicast input (UDP 5004 inbound)
# Required for Axia Livewire and any multicast RTP audio source to reach FFmpeg.
$rtpRuleName = "MultiCoder Axia RTP In (UDP 5004)"
$existing = netsh advfirewall firewall show rule name="$rtpRuleName" 2>&1
if ($existing -match "No rules match") {
    netsh advfirewall firewall add rule `
        name="$rtpRuleName" protocol=UDP dir=in localport=5004 action=allow | Out-Null
    Write-Host "  Added firewall rule: $rtpRuleName"
} else {
    Write-Host "  Firewall rule already exists: $rtpRuleName"
}

Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Copy multicoder-supervisor.exe and multicoder-worker.exe to $InstallDir\bin\"
Write-Host "  2. Copy the www\ folder to $InstallDir\www\"
Write-Host "  3. Run: $InstallDir\bin\multicoder-supervisor.exe"
Write-Host "     (Or install as a Windows Service — see README.md for sc.exe commands)"
Write-Host ""
Write-Host "Access UI at: http://localhost:8050"

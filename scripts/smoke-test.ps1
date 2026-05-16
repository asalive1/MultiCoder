param(
    [string]$HostName = "localhost",
    [int]$Port = 8050
)

$ErrorActionPreference = "Stop"
$base = "http://$HostName`:$Port"
$pass = 0
$fail = 0

function Check([string]$Desc, [string]$Expected, [string]$Actual) {
    if ($Actual -like "*$Expected*") {
        Write-Host "  PASS: $Desc"
        $script:pass++
    } else {
        Write-Host "  FAIL: $Desc - expected '$Expected' got '$Actual'"
        $script:fail++
    }
}

function Invoke-JsonPost([string]$Url, $Body) {
    $json = if ($Body -is [string]) { $Body } else { $Body | ConvertTo-Json -Depth 8 -Compress }
    return Invoke-RestMethod -Method Post -Uri $Url -ContentType "application/json" -Body $json
}

Write-Host "=== MultiCoder Smoke Tests (Windows) ==="
Write-Host "    Target: $base"
Write-Host ""

Write-Host "--- Health ---"
$healthRaw = Invoke-WebRequest -UseBasicParsing -Uri "$base/health"
Check "/health returns ok" '"status":"ok"' $healthRaw.Content

Write-Host "--- Encoder list ---"
$encRaw = Invoke-WebRequest -UseBasicParsing -Uri "$base/api/encoders"
Check "/api/encoders includes encoder 1" '"id":1' $encRaw.Content

Write-Host "--- Cue + SCTE ---"
$adminCfg = @{
    uiPort = 8050
    logLevel = "info"
    logRotSize = 10
    logRetention = 14
    encoderCount = 5
    adminUser = "Admin"
    adminPass = "change-me"
    firstLoginRequired = $false
    scteGlobalEnabled = $true
    scteGlobalRateLimitCount = 5
    scteGlobalRateLimitWindowSec = 10
    scteGlobalDedupeSeconds = 30
    scteLogLevel = "info"
}
Invoke-JsonPost "$base/api/admin/config" $adminCfg | Out-Null

$metaCfg = @{
    mode = "listen"
    listenPort = 9000
    dataConnectHost = ""
    dataConnectPort = $null
    scte = @{
        enabled = $true
        listenEnabled = $true
        listenTransport = "http"
        listenPort = 9041
        cueDeliveryType = "json"
        passthroughMode = "off"
        requireEventId = $false
        requireToken = $false
        token = ""
        watchTags = @("SCTE", "Event_ID", "Event_Duration")
        commandRows = @(@{ match = "BREAK"; action = "START_BREAK" })
        whitelistEnabled = $false
        whitelistEntries = @()
        overrideRateLimit = $false
        rateLimitCount = 5
        rateLimitWindowSec = 10
        overrideDedupe = $false
        dedupeSeconds = 30
    }
}
Invoke-JsonPost "$base/api/encoder/1/config/metadata" $metaCfg | Out-Null

$cueResp = Invoke-JsonPost "$base/api/encoder/1/cue" @{ command = "BREAK"; eventId = "evt-1" } | ConvertTo-Json -Compress
Check "Cue endpoint returns matched=true" '"matched":true' $cueResp
Check "Cue endpoint returns sent=false with passthrough OFF" '"sent":false' $cueResp

Write-Host "--- SRT input delay validation ---"
$badInput = @{
    inputType = "srt"
    srtMode = "caller"
    srtHost = "127.0.0.1"
    srtPort = 9250
    srtLatency = 95000
    srtPass = ""
    srtStreamId = ""
    srtPbkeylen = 0
    rtpGain = 0
    sampleRate = 48000
    bitrate = 128000
}
try {
    Invoke-JsonPost "$base/api/encoder/1/input/connect" $badInput | Out-Null
    Check "SRT latency above max rejected" "400" "200"
} catch {
    $code = $_.Exception.Response.StatusCode.value__
    Check "SRT latency above max rejected" "400" "$code"
}

$goodInput = $badInput.Clone()
$goodInput.srtLatency = 500
$okResp = Invoke-JsonPost "$base/api/encoder/1/input/connect" $goodInput | ConvertTo-Json -Compress
Check "SRT latency in range accepted" '"ok":true' $okResp

$inputStatusRaw = Invoke-WebRequest -UseBasicParsing -Uri "$base/api/encoder/1/input/status"
Check "Input status includes effective latency" 'latency=500 ms' $inputStatusRaw.Content
Invoke-JsonPost "$base/api/encoder/1/input/disconnect" @{} | Out-Null

Write-Host ""
Write-Host "=== Results: $pass passed, $fail failed ==="
if ($fail -gt 0) { exit 1 }

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

Write-Host "--- Critical stability checks ---"

# TEST 1 (Windows only): worker startup creates firewall rules.
$workerExeCandidates = @(
    (Join-Path $PSScriptRoot "..\build-debug\src\worker\multicoder-worker.exe"),
    (Join-Path $PSScriptRoot "..\build-debug\multicoder-worker.exe"),
    (Join-Path (Get-Location) "multicoder-worker.exe")
)
$workerExe = $workerExeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($workerExe) {
    $workerProc = $null
    try {
        $workerProc = Start-Process -FilePath $workerExe -ArgumentList "1" -NoNewWindow -PassThru
        Start-Sleep -Seconds 3
        $ruleOutput = netsh advfirewall firewall show rule name=MultiCoder-Enc1-Port* | Out-String
        if (($ruleOutput | Select-String "Enabled:\s*Yes" -Quiet)) {
            Write-Host "  PASS: Windows firewall rules created at startup"
            $pass++
        } else {
            Write-Host "  FAIL: Windows firewall rules not found"
            $fail++
        }
    } catch {
        Write-Host "  FAIL: Firewall startup check failed - $($_.Exception.Message)"
        $fail++
    } finally {
        if ($workerProc -and -not $workerProc.HasExited) {
            Stop-Process -Id $workerProc.Id -Force -ErrorAction SilentlyContinue
        }
    }
} else {
    Write-Host "  WARN: multicoder-worker.exe not found; skipping startup firewall rule check"
}

# TEST 2 (both platforms): StartHLS ACK should return within 3 seconds.
try {
    Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$base/api/encoder/1/hls/stop" | Out-Null
} catch {}
$ackTiming = Measure-Command {
    Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$base/api/encoder/1/hls/start" | Out-Null
}
if ($ackTiming.TotalSeconds -le 3.0) {
    Write-Host "  PASS: StartHLS ACK within 3 seconds ($([Math]::Round($ackTiming.TotalSeconds, 3)) s)"
    $pass++
} else {
    Write-Host "  FAIL: StartHLS ACK exceeded 3 seconds ($([Math]::Round($ackTiming.TotalSeconds, 3)) s)"
    $fail++
}

# TEST 3 (both platforms): optional delayed StartHLS ACK validation.
# Enable by setting MC_ENABLE_SLOW_ACK_TEST=1 and using a test build that injects ~5s delay at startHLS().
if ($env:MC_ENABLE_SLOW_ACK_TEST -eq "1") {
    try {
        Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$base/api/encoder/1/hls/stop" | Out-Null
    } catch {}
    $slowAckTiming = Measure-Command {
        $resp = Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$base/api/encoder/1/hls/start"
        if ($resp.StatusCode -lt 200 -or $resp.StatusCode -ge 300) {
            throw "Non-success status code $($resp.StatusCode)"
        }
    }
    if ($slowAckTiming.TotalSeconds -ge 4.0 -and $slowAckTiming.TotalSeconds -le 8.0) {
        Write-Host "  PASS: Slow StartHLS ACK succeeded within extended timeout ($([Math]::Round($slowAckTiming.TotalSeconds, 3)) s)"
        $pass++
    } else {
        Write-Host "  FAIL: Slow StartHLS ACK validation out of expected range ($([Math]::Round($slowAckTiming.TotalSeconds, 3)) s)"
        $fail++
    }
} else {
    Write-Host "  WARN: Skipping delayed-ACK test (set MC_ENABLE_SLOW_ACK_TEST=1 with delay-instrumented build)"
}

# TEST 4 (both platforms): HLS proxy timeout should return 504 around recv timeout window.
try {
    $adminPath = "C:\etc\encoder1\encoder_admin.json"
    if (-not (Test-Path $adminPath)) {
        throw "Missing $adminPath"
    }

    $backupPath = "$adminPath.bak.smoke"
    Copy-Item -Path $adminPath -Destination $backupPath -Force

    $dummyPort = 18991
    $adminObj = Get-Content $adminPath -Raw | ConvertFrom-Json
    $adminObj.hlsPlaybackPort = $dummyPort
    $adminObj | ConvertTo-Json -Depth 20 | Set-Content -Path $adminPath -Encoding UTF8

    $dummyListener = Start-Job -ArgumentList $dummyPort -ScriptBlock {
        param($Port)
        $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, [int]$Port)
        try {
            $listener.Start()
            $client = $listener.AcceptTcpClient()
            Start-Sleep -Seconds 12
            $client.Close()
        } finally {
            $listener.Stop()
        }
    }

    Start-Sleep -Seconds 1
    $proxyStopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    Invoke-WebRequest -UseBasicParsing -Uri "$base/encoder/1/hls/index.m3u8" -TimeoutSec 15
    $proxyStopwatch.Stop()
    # If we reached here without exception, status was 2xx and this is a failure for timeout test.
    Write-Host "  FAIL: HLS proxy timeout test returned success unexpectedly"
    $fail++
} catch {
    if ($proxyStopwatch -and $proxyStopwatch.IsRunning) {
        $proxyStopwatch.Stop()
    }
    $elapsed = if ($proxyStopwatch) { [Math]::Round($proxyStopwatch.Elapsed.TotalSeconds, 3) } else { -1 }
    $statusCode = $null
    if ($_.Exception.Response -and $_.Exception.Response.StatusCode) {
        $statusCode = [int]$_.Exception.Response.StatusCode
    }

    if ($statusCode -eq 504 -and $elapsed -ge 0 -and $elapsed -le 10.0) {
        Write-Host "  PASS: HLS proxy timeout returned 504 in ${elapsed}s"
        $pass++
    } else {
        Write-Host "  FAIL: HLS proxy timeout check failed (status=$statusCode elapsed=${elapsed}s)"
        $fail++
    }
} finally {
    if ($dummyListener) {
        Stop-Job -Job $dummyListener -ErrorAction SilentlyContinue | Out-Null
        Remove-Job -Job $dummyListener -Force -ErrorAction SilentlyContinue | Out-Null
    }
    if ($backupPath -and (Test-Path $backupPath)) {
        Copy-Item -Path $backupPath -Destination $adminPath -Force
        Remove-Item -Path $backupPath -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ""
Write-Host "=== Results: $pass passed, $fail failed ==="
if ($fail -gt 0) { exit 1 }

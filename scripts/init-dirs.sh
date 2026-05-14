#!/usr/bin/env bash
# init-dirs.sh — Create the MultiCoder directory tree if missing.
# Safe to run multiple times.

set -euo pipefail

echo "[init-dirs] Ensuring /etc/MC directory tree..."
mkdir -p /etc/MC

for i in 1 2 3 4 5; do
    mkdir -p "/etc/encoder${i}/logs"
    mkdir -p "/etc/encoder${i}/hls/segments"
    # Write default configs if missing
    if [ ! -f "/etc/encoder${i}/input.json" ]; then
        cat > "/etc/encoder${i}/input.json" <<EOF
{
    "inputType": "rtp",
    "rtpAddress": "239.192.0.0",
    "rtpPort": 5004,
    "rtpInterface": "eth0",
    "rtpGain": 0.0,
    "bitrate": 128000,
    "sampleRate": 48000
}
EOF
    fi
    if [ ! -f "/etc/encoder${i}/aac.json" ]; then
        cat > "/etc/encoder${i}/aac.json" <<EOF
{
    "url": "http://icecast:8000/stream${i}-aac",
    "user": "source",
    "pass": "hackme",
    "icyMetaInt": 8192,
    "stationId": "Encoder-${i}",
    "metaEnabled": true,
    "iface": ""
}
EOF
    fi
    if [ ! -f "/etc/encoder${i}/mp3.json" ]; then
        cat > "/etc/encoder${i}/mp3.json" <<EOF
{
    "url": "http://icecast:8000/stream${i}-mp3",
    "user": "source",
    "pass": "hackme",
    "icyMetaInt": 8192,
    "stationId": "Encoder-${i}",
    "metaEnabled": true,
    "iface": ""
}
EOF
    fi
    if [ ! -f "/etc/encoder${i}/hls.json" ]; then
        cat > "/etc/encoder${i}/hls.json" <<EOF
{
    "segmentSeconds": 5,
    "window": 5,
    "startTimeOffset": -25,
    "lowLatency": false,
    "metaEnabled": true,
    "iface": "",
    "metaParser": {"method": "id3", "scope": "current"}
}
EOF
    fi
    if [ ! -f "/etc/encoder${i}/srt.json" ]; then
        cat > "/etc/encoder${i}/srt.json" <<EOF
{
    "transport": "mpeg-ts",
    "mode": "caller",
    "host": "",
    "port": 9150,
    "streamId": "Encoder-${i}",
    "timestamp": true,
    "latency": 120,
    "buffer": 1024,
    "encryption": "none",
    "passphrase": "",
    "pbkeylen": 16
}
EOF
    fi
    if [ ! -f "/etc/encoder${i}/control.json" ]; then
        cat > "/etc/encoder${i}/control.json" <<EOF
{
    "controlPort": $((9100 + (i - 1) * 10)),
    "controlEnabled": true,
    "commands": {
        "startAAC": "StartAAC", "stopAAC": "StopAAC",
        "startMP3": "StartMP3", "stopMP3": "StopMP3",
        "startHLS": "StartHLS", "stopHLS": "StopHLS",
        "startSRT": "StartSRT", "stopSRT": "StopSRT"
    }
}
EOF
    fi
    if [ ! -f "/etc/encoder${i}/metadata.json" ]; then
        cat > "/etc/encoder${i}/metadata.json" <<EOF
{
    "listenPort": $((9000 + (i - 1) * 10)),
    "dataConnectPort": null
}
EOF
    fi
    if [ ! -f "/etc/encoder${i}/runtime_state.json" ]; then
        cat > "/etc/encoder${i}/runtime_state.json" <<EOF
{
    "controlListenerRunning": false,
    "metadataListenerRunning": false
}
EOF
    fi
done

# Write default system.json if missing
if [ ! -f /etc/MC/system.json ]; then
    cat > /etc/MC/system.json <<EOF
{
    "uiPort": 8050,
    "logLevel": "info",
    "logRotSize": 10,
    "logRetention": 14,
    "encoderCount": 5,
    "adminUser": "Admin",
    "adminPass": "change-me",
    "firstLoginRequired": true,
    "iceURL": "http://icecast:8000",
    "iceMountAAC": "/stream-aac",
    "iceMountMP3": "/stream-mp3",
    "interfaces": {}
}
EOF
fi

echo "[init-dirs] Done."

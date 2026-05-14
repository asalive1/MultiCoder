#!/usr/bin/env bash
# entrypoint.sh — Docker container entry point for MultiCoder
# Initialises directory tree then starts supervisor + worker processes.

set -euo pipefail

echo "=== MultiCoder v2.0 starting ==="

# Initialise directory layout
/opt/multicoder/bin/init-dirs.sh

ENCODER_COUNT=${ENCODER_COUNT:-5}

# Start each worker in background
for i in $(seq 1 "$ENCODER_COUNT"); do
  echo "Starting worker for encoder $i..."
  /opt/multicoder/bin/multicoder-worker "$i" &
done

echo "Starting supervisor on port ${UI_PORT:-8050}..."
exec /opt/multicoder/bin/multicoder-supervisor

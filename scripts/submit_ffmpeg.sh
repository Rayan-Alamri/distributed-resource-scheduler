#!/bin/sh
# Submit one CMD_FFMPEG_SEGMENT task per segment found in /videos/segments/.
# Works both inside a Docker container (submit is in PATH) and on the host
# (uses ./bin/submit relative to the project root).
#
# Usage (host):   MASTER_HOST=127.0.0.1 VIDEO_DIR=./videos ./scripts/submit_ffmpeg.sh
# Usage (Docker): docker exec <master> sh -c "MASTER_HOST=master ./scripts/submit_ffmpeg.sh"

# Resolve submit binary: prefer ./bin/submit (native), fall back to PATH (Docker)
if [ -x "./bin/submit" ]; then
    SUBMIT_BIN="./bin/submit"
else
    SUBMIT_BIN="submit"
fi

VIDEO_DIR="${VIDEO_DIR:-/videos}"
SEGMENTS="$VIDEO_DIR/segments"
MASTER_HOST="${MASTER_HOST:-master}"
MASTER_PORT="${MASTER_PORT:-9090}"
START_TASK_ID="${START_TASK_ID:-300}"

COUNT=$(ls "$SEGMENTS"/part_*.mp4 2>/dev/null | wc -l)
if [ "$COUNT" -eq 0 ]; then
    echo "ERROR: no segments found in $SEGMENTS/"
    echo "  Run scripts/split_video.sh first."
    exit 1
fi

echo "[submit_ffmpeg] Found $COUNT segment(s). Submitting CMD_FFMPEG_SEGMENT tasks."
$SUBMIT_BIN \
    -h "$MASTER_HOST" \
    -p "$MASTER_PORT" \
    -c 6 \
    -n "$COUNT" \
    -a 0 \
    -s 1 \
    -i "$START_TASK_ID"

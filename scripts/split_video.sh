#!/bin/sh
# Split a video from /videos/input/ into segments under /videos/segments/
# Run inside the master container:  docker exec <master> /videos/scripts/split_video.sh
# Or on host with videos/ mounted:  ./scripts/split_video.sh (set VIDEO_DIR=./videos)

VIDEO_DIR="${VIDEO_DIR:-/videos}"
INPUT_NAME="${1:-${INPUT_FILE:-input.mp4}}"
case "$INPUT_NAME" in
    */*) INPUT="$INPUT_NAME" ;;
    *)   INPUT="$VIDEO_DIR/input/$INPUT_NAME" ;;
esac
SEGMENTS="$VIDEO_DIR/segments"
PROCESSED="$VIDEO_DIR/processed"
SEGMENT_SECS="${SEGMENT_SECS:-10}"

if [ ! -f "$INPUT" ]; then
    echo "ERROR: input video not found at $INPUT"
    echo "  Place your video at $VIDEO_DIR/input/$INPUT_NAME and try again."
    exit 1
fi

mkdir -p "$SEGMENTS"
mkdir -p "$PROCESSED"
rm -f "$SEGMENTS"/part_*.mp4
rm -f "$PROCESSED"/part_*.mp4 "$PROCESSED"/.part_*.tmp.*.mp4
basename "$INPUT" > "$VIDEO_DIR/.last_input_name"

echo "[split] Splitting $INPUT into ${SEGMENT_SECS}s segments -> $SEGMENTS/"
ffmpeg -y -i "$INPUT" \
    -c copy \
    -map 0 \
    -segment_time "$SEGMENT_SECS" \
    -f segment \
    "$SEGMENTS/part_%03d.mp4"

COUNT=$(ls "$SEGMENTS"/part_*.mp4 2>/dev/null | wc -l)
echo "[split] Done. Created $COUNT segments in $SEGMENTS/"

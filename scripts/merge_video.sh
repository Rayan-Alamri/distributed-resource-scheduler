#!/bin/sh
# Merge all processed segments from /videos/processed/ into a modified output.
# Run inside the master container or on the host with the videos/ directory mounted.

VIDEO_DIR="${VIDEO_DIR:-/videos}"
if [ -n "${1:-}" ]; then
    INPUT_NAME="$1"
elif [ -n "${INPUT_FILE:-}" ]; then
    INPUT_NAME="$INPUT_FILE"
elif [ -f "$VIDEO_DIR/.last_input_name" ]; then
    INPUT_NAME=$(cat "$VIDEO_DIR/.last_input_name")
else
    INPUT_NAME="input.mp4"
fi

INPUT_BASE=$(basename "$INPUT_NAME")
INPUT_STEM=${INPUT_BASE%.*}

# Use absolute paths so the concat list works regardless of cwd
PROCESSED=$(readlink -f "$VIDEO_DIR/processed" 2>/dev/null || realpath "$VIDEO_DIR/processed")
FINAL=$(readlink -f "$VIDEO_DIR/final"     2>/dev/null || realpath "$VIDEO_DIR/final")
LIST="$FINAL/list.txt"
OUTPUT="$FINAL/${INPUT_STEM}_modified.mp4"

COUNT=$(ls "$PROCESSED"/part_*.mp4 2>/dev/null | wc -l)
if [ "$COUNT" -eq 0 ]; then
    echo "ERROR: no processed segments found in $PROCESSED/"
    echo "  Wait for all FFmpeg tasks to complete first."
    exit 1
fi

mkdir -p "$FINAL"

echo "[merge] Building concat list from $COUNT segment(s)..."
for f in "$PROCESSED"/part_*.mp4; do
    if ! ffprobe -v error "$f" >/dev/null 2>&1; then
        echo "ERROR: invalid processed segment: $f"
        exit 1
    fi
done

ls "$PROCESSED"/part_*.mp4 | sort | while IFS= read -r f; do
    abs=$(readlink -f "$f" 2>/dev/null || realpath "$f")
    printf "file '%s'\n" "$abs"
done > "$LIST"

echo "[merge] Merging to $OUTPUT ..."
ffmpeg -y \
    -f concat \
    -safe 0 \
    -i "$LIST" \
    -c copy \
    "$OUTPUT" || {
        echo "ERROR: ffmpeg merge failed"
        exit 1
    }

echo "[merge] Done. Output: $OUTPUT"

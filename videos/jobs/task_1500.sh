#!/bin/sh
VDIR="${VIDEO_DIR:-/videos}"
SEG=$(printf "%03d" "$1")
ffmpeg -y -i "$VDIR/segments/part_${SEG}.mp4" -vf scale=640:360 -c:v libx264 -preset ultrafast -c:a copy "$VDIR/processed/script_${SEG}.mp4"

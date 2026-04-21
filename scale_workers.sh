#!/bin/bash

# Script to scale workers dynamically by type
# Usage: ./scale_workers.sh <rpi3_count> <rpi4_count> <rpi5_count>

if [ $# -ne 3 ]; then
    echo "Usage: $0 <rpi3_count> <rpi4_count> <rpi5_count>"
    exit 1
fi

RPI3_COUNT=$1
RPI4_COUNT=$2
RPI5_COUNT=$3

echo "Stopping current containers..."
docker-compose down

echo "Starting with $RPI3_COUNT RPi3, $RPI4_COUNT RPi4, $RPI5_COUNT RPi5 workers..."
docker-compose up --scale worker_rpi3=$RPI3_COUNT --scale worker_rpi4=$RPI4_COUNT --scale worker_rpi5=$RPI5_COUNT
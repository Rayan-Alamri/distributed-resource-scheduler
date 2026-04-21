#!/bin/bash

# Script to dynamically add or remove workers
# Usage: ./manage_workers.sh add <type> <count> | remove <container_name>

ACTION=$1
TYPE=$2
COUNT=$3

if [ "$ACTION" = "add" ]; then
    if [ -z "$TYPE" ] || [ -z "$COUNT" ]; then
        echo "Usage: $0 add <type> <count>"
        echo "Types: rpi3, rpi4, rpi5"
        exit 1
    fi

    IMAGE_NAME="distributed-resource-scheduler-worker"
    NETWORK_NAME="distributed-resource-scheduler_scheduler-net"

    for i in $(seq 1 $COUNT); do
        CONTAINER_NAME="${TYPE}_worker_$i"
        echo "Starting $CONTAINER_NAME..."
        docker run -d --network $NETWORK_NAME --name $CONTAINER_NAME $IMAGE_NAME worker master 9090
    done

elif [ "$ACTION" = "remove" ]; then
    CONTAINER_NAME=$2
    if [ -z "$CONTAINER_NAME" ]; then
        echo "Usage: $0 remove <container_name>"
        exit 1
    fi

    echo "Stopping $CONTAINER_NAME..."
    docker stop $CONTAINER_NAME
    docker rm $CONTAINER_NAME

else
    echo "Usage: $0 add <type> <count> | remove <container_name>"
    exit 1
fi
#!/bin/sh
# Start multiple workers against the master.
# Usage: ./start-workers.sh [count] [host] [port]
#   count  — number of workers (default: 3)
#   host   — master hostname/IP (default: 127.0.0.1)
#   port   — master port (default: 9090)

COUNT=${1:-3}
HOST=${2:-127.0.0.1}
PORT=${3:-9090}

PIDS=""
trap 'kill $PIDS 2>/dev/null; wait 2>/dev/null; echo "All workers stopped."' INT TERM

echo "Starting $COUNT workers connecting to $HOST:$PORT ..."

i=1
while [ "$i" -le "$COUNT" ]; do
    ./bin/worker "$HOST" "$PORT" &
    PIDS="$PIDS $!"
    i=$((i + 1))
done

echo "$COUNT workers running. Press Ctrl+C to stop all."
wait

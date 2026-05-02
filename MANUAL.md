# Distributed Resource Scheduler — Run & Test Manual

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Build](#2-build)
3. [Run the Test Suite](#3-run-the-test-suite)
4. [Running with Docker Compose (Recommended)](#4-running-with-docker-compose-recommended)
5. [Running Natively (No Docker)](#5-running-natively-no-docker)
6. [Submitting Tasks](#6-submitting-tasks)
7. [Workload Reference](#7-workload-reference)
8. [FFmpeg Video Processing Demo](#8-ffmpeg-video-processing-demo)
9. [Fault Tolerance Demo](#9-fault-tolerance-demo)
10. [Reading the Output](#10-reading-the-output)

---

## 1. Prerequisites

### For native (non-Docker) runs

| Tool | Purpose |
|------|---------|
| `gcc` | Compiler |
| `make` | Build system |
| `ffmpeg` | Required only for FFmpeg workloads (cmd 6 & 7) |

Install on Ubuntu/Debian:
```bash
sudo apt install gcc make ffmpeg
```

Install on Alpine:
```bash
apk add gcc musl-dev make ffmpeg
```

### For Docker runs

| Tool | Minimum version |
|------|----------------|
| Docker Engine | 20.x |
| Docker Compose v2 | (`docker compose`, not `docker-compose`) |

---

## 2. Build

From the project root:

```bash
make all
```

This produces three binaries in `bin/`:

| Binary | Role |
|--------|------|
| `bin/master` | Master node — accepts workers, schedules tasks |
| `bin/worker` | Worker node — connects to master, executes tasks |
| `bin/submit` | Client — sends tasks to the master |

To clean and rebuild:

```bash
make clean && make all
```

---

## 3. Run the Test Suite

```bash
make test
```

This runs two test suites:

**Worker executor tests** (`bin/test_worker_executor`):
- Prime counting (cmd 1)
- Matrix checksum (cmd 2)
- Prime range (cmd 3) — range [10,20] → 4 primes
- Monte Carlo Pi (cmd 4) — 1M samples, result ≈ 785 000
- Mandelbrot checksum (cmd 5) — determinism check
- Protocol byte-order roundtrip
- Prime-range payload survives network encoding

**Scheduler tests** (`bin/test_scheduler`):
- FIFO queue ordering
- Round-robin / least-loaded worker selection
- Worker offline / task requeue

Expected output (`test_scheduler` runs first, then `test_worker_executor`):
```
=== Master unit tests ===
[PASS] queue_fifo
[PASS] queue_empty
[PASS] queue_requeue
[PASS] registry_add_and_idle
[PASS] registry_round_robin
[PASS] registry_set_offline
[PASS] registry_busy_not_selected
[PASS] payload_byte_order_roundtrip
All tests passed.
=== Worker executor tests ===
[PASS] executor_prime_task
[PASS] executor_matrix_task
[PASS] executor_prime_range
[PASS] executor_prime_range_full
[PASS] executor_monte_carlo
[PASS] executor_mandelbrot
[PASS] executor_mandelbrot_deterministic
[PASS] protocol_socketpair_roundtrip
[PASS] protocol_prime_range_payload
All worker tests passed.
```

---

## 4. Running with Docker Compose (Recommended)

Docker Compose starts one master and three simulated workers (rpi3 / rpi4 / rpi5) with CPU and memory limits that mimic a heterogeneous Raspberry Pi cluster.

### 4.1 Start the cluster

```bash
docker compose up --build
```

The master auto-seeds 6 demo prime tasks after 3 seconds (controlled by `DEMO_TASKS=6`).

Watch tasks flow through the scheduler:
```
master  | [master] listening on port 9090
master  | [demo] will enqueue 6 tasks in 3 seconds...
worker1 | [worker] connected to master at master:9090
master  | [master] worker 1 registered from 172.x.x.x
master  | [scheduler] task=1 cmd=1 arg=5000000 -> worker=1
master  | [master] result: worker=1 task=1 result=348513  [done=1 avg=1823ms]
```

### 4.2 Stop the cluster

```bash
docker compose down
```

### 4.3 Change the number of auto-seeded demo tasks

Edit `DEMO_TASKS` in `docker-compose.yml`, or override at runtime:

```bash
DEMO_TASKS=12 docker compose up --build
```

To start with an empty queue and submit tasks manually, either remove `DEMO_TASKS` entirely or set it to `0`:

```bash
DEMO_TASKS=0 docker compose up --build
```

### 4.4 Submit tasks manually from another terminal

```bash
# Submit 4 prime tasks using env vars
docker compose run --rm -e SUBMIT_CMD=1 -e SUBMIT_COUNT=4 -e SUBMIT_ARG=50000000 submit

# Or use CLI flags directly (entrypoint is already 'submit', do not repeat it)
docker compose run --rm submit -c 1 -n 4 -a 50000000
```

---

## 5. Running Natively (No Docker)

Open **three or four terminals** (Terminal 3 is optional but recommended).

### Terminal 1 — Master

```bash
./bin/master
# Listens on port 9090 by default

# Custom port:
MASTER_PORT=9091 ./bin/master

# Auto-seed 6 tasks after 3 s:
DEMO_TASKS=6 ./bin/master
```

### Terminal 2 — Worker 1

```bash
./bin/worker 127.0.0.1 9090

# Or using env vars:
MASTER_HOST=127.0.0.1 MASTER_PORT=9090 ./bin/worker

# With FFmpeg workloads, also set VIDEO_DIR so workers find segments:
VIDEO_DIR=./videos ./bin/worker 127.0.0.1 9090
```

### Terminal 3 — Worker 2 (optional)

```bash
./bin/worker 127.0.0.1 9090
```

### Terminal 4 — Submit client

```bash
./bin/submit -h 127.0.0.1 -c 1 -n 4 -a 10000000
```

---

## 6. Submitting Tasks

The `submit` binary connects to the master, sends N task packets, then disconnects.

### Full flag reference

```
./bin/submit [OPTIONS]

  -h HOST     master hostname        (default: master)
  -p PORT     master port            (default: 9090)
  -n COUNT    number of tasks        (default: 1)
  -c CMD      command code 1-7       (default: 1)
  -a ARG      primary argument       (default: 5000000)
  -s STEP     increment ARG by STEP per task (default: 0)
  -e END      explicit range_end for prime_range (cmd 3, single task)
  -i ID       starting task ID       (default: 100)
```

### Quick examples

```bash
# 4 prime tasks, each counting primes up to 50M
./bin/submit -c 1 -n 4 -a 50000000

# 2 matrix checksum tasks, matrix size 500
./bin/submit -c 2 -n 2 -a 500

# 10 parallel prime-range tasks covering [1, 100 000 000]
./bin/submit -c 3 -n 10 -a 1 -s 10000000

# 4 Monte Carlo Pi tasks, 10M samples each
./bin/submit -c 4 -n 4 -a 10000000

# 6 Mandelbrot tasks, 100 rows each (covers full 600-row image)
./bin/submit -c 5 -n 6 -a 0 -s 100

# 12 FFmpeg segment tasks (segment IDs 0-11)
./bin/submit -c 6 -n 12 -a 0 -s 1 -i 300
```

---

## 7. Workload Reference

| cmd | Name | `argument` | `result` (in task) | Worker returns |
|-----|------|------------|-------------------|----------------|
| 1 | `prime` | Upper bound | — | Count of primes ≤ argument |
| 2 | `matrix` | Matrix size N | — | Checksum of N×N matrix |
| 3 | `prime_range` | Range start | Range end | Count of primes in [start, end] |
| 4 | `monte_carlo` | Sample count | — | Points inside unit circle |
| 5 | `mandelbrot` | Row start | — | Iteration checksum for 100 rows |
| 6 | `ffmpeg_segment` | Segment ID | — | 1=success 2=timeout 3=missing 4=error |
| 7 | `ffmpeg_script` | Segment ID | — | 1=success 2=timeout 3=missing 4=error |

### Aggregating parallel results

**Prime range** — sum all worker results:
```
total_primes = result_task0 + result_task1 + ... + result_taskN
```

**Monte Carlo Pi** — sum inside counts across all tasks:
```
pi ≈ 4 × (sum of all results) / (COUNT × SAMPLES_PER_TASK)
```
Example: 4 tasks × 10M samples, total inside = 31 415 000 → π ≈ 4×31415000/40000000 ≈ 3.1415

**Mandelbrot** — 6 tasks (rows 0,100,200,300,400,500), each returns a checksum for its 100-row strip; combine checksums as needed for verification.

---

## 8. FFmpeg Video Processing Demo

### 8.1 Directory layout

Place your input video before starting:

```
videos/
├── input/
│   └── input.mp4        ← place your video here
├── segments/            ← created by split_video.sh
├── processed/           ← written by workers
├── final/               ← created by merge_video.sh
└── jobs/                ← trusted scripts for cmd 7
```

### 8.2 Step-by-step (Docker)

**Step 1 — Start the cluster with no auto-tasks:**

```bash
# Remove DEMO_TASKS from docker-compose.yml or set to 0, then:
docker compose up --build
```

**Step 2 — Place input video:**

```bash
cp /path/to/your/video.mp4 videos/input/input.mp4
```

**Step 3 — Split into 10-second segments:**

Run `split_video.sh` on the **host** (scripts are not copied into the container image):

```bash
VIDEO_DIR=./videos ./scripts/split_video.sh
```

The `videos/` directory is bind-mounted into all containers, so segments written here are immediately visible to workers.

Segments appear in `videos/segments/part_000.mp4`, `part_001.mp4`, etc.

**Step 4 — Submit one task per segment:**

```bash
# Count segments first
ls videos/segments/part_*.mp4 | wc -l   # e.g. 12

# Submit 12 ffmpeg_segment tasks
./bin/submit -h 127.0.0.1 -c 6 -n 12 -a 0 -s 1 -i 300
```

Or use the helper script (auto-detects `./bin/submit` on host, falls back to `submit` in PATH inside Docker):

```bash
MASTER_HOST=127.0.0.1 VIDEO_DIR=./videos ./scripts/submit_ffmpeg.sh
```

**Step 5 — Watch workers process in parallel:**

```
[scheduler] task=300 cmd=6 arg=0 -> worker=1
[scheduler] task=301 cmd=6 arg=1 -> worker=2
[scheduler] task=302 cmd=6 arg=2 -> worker=3
[master]    result: worker=2 task=301 result=1  [done=1 avg=8400ms]
[scheduler] task=303 cmd=6 arg=3 -> worker=2   ← worker 2 got more work first
```

`result=1` means FFmpeg success. Processed segments appear in `videos/processed/`.

**Step 6 — Merge segments:**

```bash
VIDEO_DIR=./videos ./scripts/merge_video.sh
```

Final video: `videos/final/output.mp4`

### 8.3 VIDEO_DIR environment variable

Workers default to `/videos` (the Docker container path).
For native runs, set `VIDEO_DIR` to point to your local `videos/` directory:

```bash
VIDEO_DIR=./videos ./bin/worker 127.0.0.1 9090
```

In Docker Compose this is unnecessary because the bind mount already places the files at `/videos` inside every container.

### 8.4 Step-by-step (native, all-in-one)

```bash
# Terminal 1
./bin/master

# Terminal 2 & 3
./bin/worker 127.0.0.1 9090 &
./bin/worker 127.0.0.1 9090 &

# Terminal 4
cp /path/to/video.mp4 videos/input/input.mp4
VIDEO_DIR=./videos ./scripts/split_video.sh

SEG_COUNT=$(ls videos/segments/part_*.mp4 | wc -l)
./bin/submit -h 127.0.0.1 -c 6 -n $SEG_COUNT -a 0 -s 1 -i 300

# After all tasks complete:
VIDEO_DIR=./videos ./scripts/merge_video.sh
vlc videos/final/output.mp4    # or mpv / any player
```

### 8.5 Using a custom FFmpeg script (cmd 7)

Write a script to `videos/jobs/task_400.sh`.  
Use `${VIDEO_DIR:-/videos}` so the script works both natively and in Docker:

```bash
#!/bin/sh
# $1 = segment ID
VDIR="${VIDEO_DIR:-/videos}"
ffmpeg -y \
    -i "$VDIR/segments/part_$(printf "%03d" "$1").mp4" \
    -vf "vflip" \
    "$VDIR/processed/part_$(printf "%03d" "$1").mp4"
```

Make it executable:
```bash
chmod +x videos/jobs/task_400.sh
```

Submit one cmd=7 task referencing this script (task_id=400 → worker reads `jobs/task_400.sh`):
```bash
./bin/submit -c 7 -n 1 -a 0 -i 400
```

For native runs, workers must also have `VIDEO_DIR` set when they start so they can find the script:
```bash
VIDEO_DIR=./videos ./bin/worker 127.0.0.1 9090
```

---

## 9. Fault Tolerance Demo

### Kill a worker mid-task

1. Start the cluster and submit several long-running tasks (e.g. prime up to 100M):

    ```bash
    ./bin/submit -c 1 -n 6 -a 100000000
    ```

2. While tasks are running, find and kill one worker:

    ```bash
    # Docker
    docker compose stop worker_rpi3

    # Native
    kill <worker_pid>
    ```

3. Observe the master requeue the orphaned task:

    ```
    [master] worker 2 disconnected
    [master] requeueing orphaned task 3
    [scheduler] task=3 cmd=1 arg=100000000 -> worker=1
    ```

The task is automatically reassigned to an available worker. No manual intervention needed.

### Reconnect a worker

```bash
docker compose start worker_rpi3
# or
./bin/worker 127.0.0.1 9090
```

The master registers it as a new worker and begins assigning tasks to it.

---

## 10. Reading the Output

### Master scheduler line

```
[scheduler] task=301 cmd=6 arg=1 -> worker=2
```

| Field | Meaning |
|-------|---------|
| `task=301` | Task ID |
| `cmd=6` | Command code (6 = ffmpeg_segment) |
| `arg=1` | Segment ID |
| `worker=2` | Worker assigned (least-loaded selection) |

### Master result line

```
[master] result: worker=2 task=301 result=1  [done=3 avg=8200ms]
```

| Field | Meaning |
|-------|---------|
| `result=1` | Return value (1 = FFmpeg success for cmd 6/7; prime count for cmd 1) |
| `done=3` | Total tasks this worker has completed so far |
| `avg=8200ms` | Average task runtime for this worker |

### Worker task line

```
[worker] received task=301 command=6 argument=1
[worker] sent result: task=301 result=1
```

### FFmpeg result codes (cmd 6 & 7)

| result | Meaning |
|--------|---------|
| `0` | Failure (generic) |
| `1` | Success |
| `2` | Timeout (killed after 120 s) |
| `3` | Input segment missing |
| `4` | FFmpeg command error (non-zero exit) |

---

## Quick Reference Card

```bash
# Build
make all

# Test
make test

# Start cluster (Docker)
docker compose up --build

# Submit tasks (host → Docker cluster)
./bin/submit -h 127.0.0.1 -c <CMD> -n <COUNT> -a <ARG> [-s <STEP>]

# Parallel prime [1, 100M] across 10 workers
./bin/submit -c 3 -n 10 -a 1 -s 10000000

# Monte Carlo Pi — 4 workers × 10M samples
./bin/submit -c 4 -n 4 -a 10000000

# Mandelbrot — full 600-row image in 6 chunks
./bin/submit -c 5 -n 6 -a 0 -s 100

# FFmpeg — split, submit, merge
VIDEO_DIR=./videos ./scripts/split_video.sh
./bin/submit -c 6 -n $(ls videos/segments/part_*.mp4 | wc -l) -a 0 -s 1 -i 300
VIDEO_DIR=./videos ./scripts/merge_video.sh
```

# UI Status API

This project exposes a read-only snapshot API for UI code. The UI should use
this instead of reading `MasterState`, `WorkerRegistry`, or `TaskQueue`
directly.

## Header

```c
#include "src/master/status.h"
```

## Snapshot

```c
MasterSnapshot snapshot;
master_get_snapshot(&master_state, &snapshot);
```

The snapshot contains:

- `queue_depth`
- `worker_count`
- `workers[]`
- `job`

Each worker snapshot contains:

- `worker_id`
- `status`
- `current_task_id`
- `current_command_code`
- `current_argument`
- `tasks_completed`
- `total_runtime_ms`
- `avg_runtime_ms`
- `current_runtime_ms`
- `last_heartbeat_age_sec`

The job snapshot contains:

- `active`
- `job_id`
- `command_code`
- `expected`
- `completed`
- `failed`
- `sum_results`
- `sum_arguments`

## JSON

For a web UI or debug output:

```c
char json[8192];
master_snapshot_to_json(&snapshot, json, sizeof(json));
```

Example shape:

```json
{
  "generated_at": 1710000000,
  "queue_depth": 3,
  "workers": [
    {
      "worker_id": 1,
      "status": "busy",
      "current_task_id": 200,
      "current_command": "matrix",
      "current_argument": 500,
      "tasks_completed": 4,
      "total_runtime_ms": 12000,
      "avg_runtime_ms": 3000,
      "current_runtime_ms": 900,
      "last_heartbeat_age_sec": 2
    }
  ],
  "job": {
    "active": true,
    "job_id": 1,
    "command": "matrix",
    "expected": 10,
    "completed": 4,
    "failed": 0,
    "sum_results": 748000,
    "sum_arguments": 5000
  }
}
```

## Notes For UI Work

- `master_get_snapshot()` handles locking internally.
- UI code should poll snapshots, for example every 250-1000 ms.
- Do not hold scheduler or registry locks from UI code.
- For ncurses, render `MasterSnapshot` directly.
- For a web UI, expose `master_snapshot_to_json()` from a small HTTP handler.

## Terminal Job Submission

The existing terminal endpoint for creating work is `bin/submit`. It connects
to the master, sends one batch of tasks, then exits.

```sh
./bin/submit -h 127.0.0.1 -p 9090 -c 2 -n 10 -a 500 -i 200
```

Arguments:

- `-h`: master host
- `-p`: master port
- `-c`: command code
- `-n`: number of tasks
- `-a`: first task argument
- `-s`: step added to the argument for each task
- `-e`: explicit range end for `prime_range`
- `-i`: starting task ID

Command codes:

- `1`: prime counting
- `2`: matrix checksum
- `3`: prime range
- `4`: Monte Carlo Pi
- `5`: Mandelbrot row chunks
- `6`: FFmpeg segment
- `7`: FFmpeg script

Useful examples:

```sh
# Matrix demo: 10 independent matrix tasks
./bin/submit -h 127.0.0.1 -p 9090 -c 2 -n 10 -a 500 -i 200

# Prime range: 10 chunks covering roughly 1..100,000,000
./bin/submit -h 127.0.0.1 -p 9090 -c 3 -n 10 -a 1 -s 10000000 -i 300

# Monte Carlo Pi: 4 tasks with 10M samples each
./bin/submit -h 127.0.0.1 -p 9090 -c 4 -n 4 -a 10000000 -i 400
```

The UI owner can use this in two ways:

- Keep `bin/submit` as the supported terminal workflow.
- Reuse the same protocol from UI code later by sending `MSG_SUBMIT` followed
  by one or more `MSG_TASK` payloads.

# Proposed Features: Distributed Resource Scheduler

## Overview

The next phase of the Distributed Resource Scheduler will extend the system from a fixed numerical workload executor into a more flexible distributed job runner. The current system already supports a master-worker architecture, TCP-based task assignment, worker heartbeats, process-isolated execution, task results, and basic failure recovery. The proposed features build on that foundation by adding real-world parallel workloads, controlled FFmpeg remote execution, shared storage for large data, and improved scheduling behavior.

The goal is not to turn the project into a production cloud platform. Instead, the goal is to demonstrate important distributed-system concepts clearly:

- parallel task decomposition
- remote worker execution
- fair worker utilization
- control-plane and data-plane separation
- fault handling during distributed workloads
- real-world batch processing using multiple machines

The main demonstration target is a small LAN cluster where the master runs on a laptop or desktop machine and the workers run on Raspberry Pis or Docker containers.

## Final Proposed Feature Set

The proposed feature set includes:

1. Distributed FFmpeg video processing
2. Controlled FFmpeg remote execution
3. Shared storage data plane
4. Parallel workload decomposition
5. New parallel algorithmic workloads
6. Improved scheduling for fairness and utilization
7. Timeout and failure handling for remote execution

Worker capability metadata is intentionally excluded from the current scope. Since this is a controlled project demo, the system can assume that all participating workers have the required dependencies installed and can access the shared workload directory.

## 1. Distributed FFmpeg Video Processing

### Purpose

Distributed FFmpeg processing will provide a practical and visual demonstration of the scheduler. Instead of only running numerical tasks such as prime counting or matrix checksums, the system will process a video file by splitting it into segments and assigning those segments to different workers.

This workload is useful because video processing is:

- CPU-intensive
- easy to divide into independent chunks
- easy to demonstrate visually
- close to real-world distributed batch processing

### High-Level Workflow

The video processing workflow will use a map-reduce-like model:

```text
Input video
  -> split into segments
  -> distribute segment tasks to workers
  -> workers process segments with FFmpeg
  -> master collects task results
  -> master merges processed segments
  -> final output video
```

Each segment is treated as an independent task. For example:

```text
Task 300: process segment 0
Task 301: process segment 1
Task 302: process segment 2
Task 303: process segment 3
```

Workers process different segments in parallel, allowing the workload to complete faster than if one machine processed the entire video sequentially.

### Example Operations

The system can support several FFmpeg operations:

- resize video to 720p
- convert video to grayscale
- add a watermark
- change bitrate
- extract audio
- generate thumbnails
- convert file format
- apply brightness, contrast, or sharpening filters

For the project demo, grayscale conversion or resizing is recommended because the result is visible and easy to explain.

### Example FFmpeg Commands

Splitting the input video:

```bash
ffmpeg -i videos/input/input.mp4 \
  -c copy \
  -map 0 \
  -segment_time 10 \
  -f segment \
  videos/segments/part_%03d.mp4
```

Processing one segment:

```bash
ffmpeg -y \
  -i videos/segments/part_003.mp4 \
  -vf "scale=1280:720,hue=s=0" \
  -c:v libx264 \
  -preset veryfast \
  -c:a copy \
  videos/processed/part_003.mp4
```

Merging the processed segments:

```bash
ffmpeg -f concat \
  -safe 0 \
  -i videos/final/list.txt \
  -c copy \
  videos/final/output.mp4
```

### Expected Demo Value

This feature clearly shows that the scheduler can distribute real work across multiple machines. It also gives a visible final result, which makes it stronger for presentation than purely numeric workloads.

## 2. Controlled FFmpeg Remote Execution

### Purpose

Controlled remote execution will allow the master to send trusted FFmpeg commands or scripts to workers at runtime. This avoids modifying and recompiling the worker binary every time a new FFmpeg operation is needed.

Without remote execution, the worker would need a hardcoded command for every operation:

```c
case 3:
    run_grayscale(segment_id);
case 4:
    run_resize(segment_id);
case 5:
    run_watermark(segment_id);
```

With controlled remote execution, the worker can receive a trusted script or command template and execute it for the assigned segment.

### Scope

This feature should be limited to trusted local demonstration use. It is not intended to support unrestricted arbitrary code execution in a production environment.

The intended scope is:

- FFmpeg commands only, or
- trusted scripts stored in a known directory, or
- command templates submitted by the master for video segment processing

The system should not present this as secure production RCE. A correct description is:

> The system supports controlled remote execution of trusted FFmpeg workloads for demonstration purposes.

### Example Execution Model

The master sends a task containing:

```text
task_id = 301
command_code = RCE_FFMPEG
argument = 1
script/command = process segment 1 with grayscale filter
```

The worker then executes:

```bash
ffmpeg -y \
  -i /mnt/drs/videos/segments/part_001.mp4 \
  -vf "hue=s=0" \
  /mnt/drs/videos/processed/part_001.mp4
```

The worker returns:

```text
task_id = 301
result = success or failure
```

### Why This Is Useful

Controlled FFmpeg remote execution makes the scheduler more flexible. New video-processing operations can be demonstrated without changing the worker source code.

Examples:

```text
Demo 1: resize all segments
Demo 2: grayscale all segments
Demo 3: add watermark to all segments
Demo 4: extract thumbnails from all segments
```

Each demo can use the same worker binary.

### Important Limitations

Workers still need:

- FFmpeg installed
- access to the shared video directory
- permission to read input segments
- permission to write processed outputs

Remote execution changes how the command is selected. It does not remove the need for shared data access.

## 3. Shared Storage Data Plane

### Purpose

Large video files should not be transferred through the existing TCP scheduling protocol. The current protocol is designed for small fixed-size messages such as task IDs, command codes, arguments, worker IDs, and results. Video files are large and variable-sized, so sending them through the control socket would add unnecessary complexity.

The proposed design separates the system into two planes:

```text
Control plane:
  TCP sockets for task assignment, status, heartbeats, and results

Data plane:
  Shared filesystem for large workload files
```

### Directory Layout

The shared directory can use this structure:

```text
videos/
├── input/
│   └── input.mp4
├── segments/
│   ├── part_000.mp4
│   ├── part_001.mp4
│   └── part_002.mp4
├── processed/
│   ├── part_000.mp4
│   ├── part_001.mp4
│   └── part_002.mp4
└── final/
    ├── list.txt
    └── output.mp4
```

The master is responsible for:

- placing the original input video in `videos/input/`
- splitting the input into `videos/segments/`
- submitting one task per segment
- tracking task completion
- generating the concat list
- merging processed segments into `videos/final/output.mp4`

Workers are responsible for:

- reading assigned segments from `videos/segments/`
- writing processed outputs to `videos/processed/`
- returning success or failure to the master

### Raspberry Pi Setup

For Raspberry Pi deployment, the best option is a shared network folder mounted on all workers.

Recommended setup:

```text
Master machine:
  /srv/drs/videos

Each Raspberry Pi:
  /mnt/drs/videos
```

The worker commands should use the same path on every worker, such as:

```text
/mnt/drs/videos/segments/part_003.mp4
/mnt/drs/videos/processed/part_003.mp4
```

NFS is the recommended Linux-to-Linux option. Samba can also work, especially if the master is a Windows machine.

### Docker Setup

For Docker simulation, the same design can be implemented using a shared volume:

```yaml
volumes:
  - ./videos:/videos
```

Then the master and all worker containers can read and write files under:

```text
/videos
```

### Why Not Send Files Through TCP?

Sending video files through the existing socket protocol would require:

- variable-size payloads
- chunked file transfer
- buffering
- checksums
- retry logic
- disk write management
- much larger protocol changes

That would distract from the main goal of the project, which is distributed scheduling and execution.

## 4. Parallel Workload Decomposition

### Purpose

Parallel workload decomposition means splitting one large job into many smaller independent tasks. This is the most important technique for improving utilization.

Instead of assigning one large task to one worker:

```text
Process full video
```

the master creates many smaller tasks:

```text
Process segment 0
Process segment 1
Process segment 2
Process segment 3
```

This keeps the cluster busy and allows faster workers to receive more tasks as soon as they finish.

### Benefits

Parallel decomposition improves:

- throughput
- worker utilization
- fault recovery
- fairness
- demo clarity

If a worker fails while processing one segment, only that segment needs to be retried. The entire video does not need to be restarted.

### Recommended Chunk Size

For FFmpeg demos, a segment length between 5 and 15 seconds is reasonable.

Smaller chunks:

- improve load balancing
- reduce lost work on failure
- increase scheduling overhead

Larger chunks:

- reduce scheduling overhead
- may leave some workers idle near the end
- make failure recovery slower

For the project demo, 10-second chunks are a good default.

### General Pattern

The same decomposition pattern applies to numerical workloads:

```text
Large problem
  -> split into chunks
  -> schedule chunks
  -> collect partial results
  -> aggregate result
```

This pattern is useful for both video processing and algorithmic workloads.

## 5. New Parallel Algorithmic Workloads

In addition to FFmpeg, the system can add parallel numerical workloads. These workloads help demonstrate that the scheduler can handle both real-world media processing and classic distributed algorithms.

### 5.1 Parallel Prime Counting

The existing prime workload can be upgraded into a true parallel algorithm.

Instead of one task:

```text
count primes from 1 to 100,000,000
```

the master creates chunks:

```text
Task 1: count primes from 1 to 10,000,000
Task 2: count primes from 10,000,001 to 20,000,000
Task 3: count primes from 20,000,001 to 30,000,000
```

Each worker returns a partial count. The master sums the partial counts:

```text
total_primes = result1 + result2 + result3 + ...
```

This is a strong workload because it builds directly on the existing implementation.

### 5.2 Parallel Monte Carlo Pi Estimation

Monte Carlo Pi estimation is naturally parallel. Each worker simulates a portion of the total random samples.

Example:

```text
Total samples: 40,000,000
Task 1: 10,000,000 samples
Task 2: 10,000,000 samples
Task 3: 10,000,000 samples
Task 4: 10,000,000 samples
```

Each worker returns the number of points that landed inside the unit circle. The master aggregates:

```text
pi_estimate = 4 * total_inside / total_samples
```

Since the current protocol returns integers, the result can be scaled:

```text
314159 means 3.14159
```

### 5.3 Parallel Mandelbrot Checksum

The Mandelbrot workload can be divided by image rows or tiles.

Example:

```text
Task 1: rows 0-99
Task 2: rows 100-199
Task 3: rows 200-299
```

Each worker computes a checksum for its assigned region. The master combines the checksums.

This workload is useful because it represents rendering-style parallel computation without requiring the scheduler to transfer image files.

### 5.4 Parallel Matrix Multiplication or Checksum

The current matrix checksum workload can be extended into a chunked matrix workload.

Possible approaches:

- split matrix rows across workers
- split matrix blocks across workers
- compute partial checksums
- combine partial results

This workload is good for demonstrating CPU-heavy numerical computation.

### Recommended Algorithmic Workload Set

The best set for the final demo is:

```text
1. Parallel prime counting
2. Parallel Monte Carlo Pi estimation
3. Parallel Mandelbrot checksum
4. Distributed FFmpeg video processing
```

This gives a strong mix of:

- existing functionality
- scientific computation
- rendering-style computation
- real-world media processing

## 6. Improved Scheduling for Fairness and Utilization

### Current Scheduler Behavior

The current scheduler uses:

```text
Task order: FIFO
Worker selection: round-robin among idle workers
Priority: none
```

This is a good baseline because it prevents the scheduler from always selecting the same worker first.

### Proposed Scheduling Behavior

The improved approach keeps the current round-robin design but relies on small independent tasks to improve utilization.

The scheduler should:

1. Keep tasks in the queue.
2. Select the next idle worker using round-robin.
3. Assign one task to that worker.
4. Mark the worker as busy.
5. When the worker returns a result, mark it idle again.
6. Assign another task when work is available.

This design naturally handles heterogeneous workers:

```text
Faster worker finishes earlier -> becomes idle earlier -> receives more tasks
Slower worker finishes later -> receives fewer tasks
```

This gives practical utilization without adding worker capability metadata.

### Worker Runtime Metrics

The scheduler can be improved by tracking lightweight runtime metrics for each worker. This does not require automatic capability discovery. Instead, the master observes how workers perform over time.

Suggested fields in `WorkerInfo`:

```c
uint32_t tasks_completed;
uint64_t total_runtime_ms;
uint32_t current_load;
time_t   task_started_at;
```

The master can update these values when a task is assigned and when a result is received:

```text
on task assignment:
  current_load = 1
  task_started_at = current time

on task result:
  runtime_ms = current time - task_started_at
  total_runtime_ms += runtime_ms
  tasks_completed += 1
  current_load = 0
```

Then the scheduler can calculate:

```text
average_runtime_ms = total_runtime_ms / tasks_completed
```

These metrics are useful for logs, dashboards, and scheduling decisions. For example, the master can show:

```text
Worker 1: completed 8 tasks, average runtime 4200 ms
Worker 2: completed 5 tasks, average runtime 6700 ms
Worker 3: completed 12 tasks, average runtime 3100 ms
```

This gives evidence that faster workers are contributing more work without requiring the workers to report detailed hardware information.

### Least-Loaded Scheduling

A simple improvement over pure round-robin is least-loaded scheduling. Since the current worker model assigns one active task per worker, `current_load` is usually either `0` or `1`. This still helps if the project later allows multiple concurrent tasks per worker.

The scheduler can choose an eligible worker using:

```text
1. prefer idle workers
2. among idle workers, prefer the worker with fewer completed tasks
3. if tied, prefer lower average runtime
4. if still tied, use round-robin as the final tie-breaker
```

This keeps scheduling fair while still favoring workers that have demonstrated better throughput.

A practical policy for this project is:

```text
Task selection: FIFO
Worker selection: idle worker with best scheduling score
Tie-breaker: round-robin
```

Example scheduling score:

```text
score = tasks_completed + current_load * 1000
```

Lower score means the worker is preferred. This simple version prevents one worker from receiving too many tasks early in the run. A more advanced version can include average runtime.

### Manual Weighted Scheduling

For a known Raspberry Pi cluster, manual worker weights can improve utilization without adding full worker capability metadata.

Example:

```text
Pi 3 weight = 1
Pi 4 weight = 2
Pi 5 weight = 3
```

Over time, the scheduler should roughly assign:

```text
Pi 5: about 3x the work of Pi 3
Pi 4: about 2x the work of Pi 3
Pi 3: baseline share
```

This can be implemented with a static configuration file or command-line option on the master, for example:

```text
worker 1 weight 1
worker 2 weight 2
worker 3 weight 3
```

Weighted scheduling is useful when the team already knows the worker devices. It avoids the overhead of automatic capability discovery while still letting stronger workers receive more tasks.

For the current project, weighted scheduling should be considered optional. The simpler and safer default remains dynamic scheduling with many small chunks. Faster workers naturally become idle sooner and receive more tasks.

### Fairness

Fairness is provided by:

- round-robin selection among idle workers
- avoiding repeated preference for one worker
- using small tasks so work is spread across the cluster
- optional runtime metrics to detect imbalance
- optional manual weights for known heterogeneous workers

The system does not need to assign the same number of tasks to every worker. In a heterogeneous cluster, equal task count is not always fair. A faster worker can reasonably complete more tasks.

### Utilization

Utilization is improved by:

- splitting large jobs into many chunks
- scheduling tasks dynamically as workers become idle
- requeueing failed or orphaned tasks
- avoiding one large task that blocks a single worker while others wait

For the demo, the most important rule is:

```text
Use more tasks than workers.
```

For example, with 3 Raspberry Pis, use at least 9 to 12 video segments. This makes the scheduling behavior visible and keeps all workers busy.

## 7. Timeout and Failure Handling for Remote Execution

### Purpose

Remote execution introduces new failure cases. An FFmpeg command may fail, hang, or run longer than expected. The worker process may also disconnect during execution.

The system should add basic execution controls to keep the demo stable.

### Timeout Handling

Each remote execution task should have a maximum runtime. If the child process exceeds the timeout, the worker should terminate it and report failure.

Example behavior:

```text
Worker receives task 301
Worker forks child process
Child runs FFmpeg command
Parent waits with timeout
If child finishes: return success/failure
If timeout expires: kill child and return failure
```

### Failure Handling

The system should handle:

- FFmpeg command exits with non-zero status
- input segment is missing
- output segment cannot be written
- worker disconnects mid-task
- child process times out

The master can respond in one of two ways:

```text
Option 1: mark task as failed
Option 2: requeue task for another worker
```

For the demo, requeueing disconnected-worker tasks is useful because it shows fault tolerance. For command failures caused by missing files or invalid scripts, marking the task as failed is safer because retrying would likely fail again.

### Output Control

For controlled RCE, the worker should avoid sending large command output through the protocol. Instead:

- return only a numeric success/failure result
- write detailed FFmpeg logs to a local or shared log file
- optionally return a short error code

Example result values:

```text
0 = failure
1 = success
2 = timeout
3 = missing input
4 = command error
```

## Proposed Protocol Direction

The current `NetworkPayload` is fixed-size and numeric:

```c
typedef struct {
    uint32_t type;
    uint32_t worker_id;
    uint32_t task_id;
    uint32_t command_code;
    uint32_t argument;
    uint32_t result;
} NetworkPayload;
```

This is enough for fixed workloads and simple segment-based FFmpeg tasks:

```text
command_code = 3
argument = segment_id
```

For controlled remote execution, the protocol may need a small extension. A practical approach is to keep the existing payload for scheduling metadata and use task files or script IDs for the command details.

### Simple RCE Approach

Use:

```text
command_code = RCE_FFMPEG
argument = segment_id
task_id = script or task identifier
```

The worker reads the trusted script/template from shared storage:

```text
/mnt/drs/jobs/task_301.sh
```

This avoids sending large variable-sized script content through the socket.

### More Advanced RCE Approach

A later version could add variable-size payloads:

```c
typedef struct {
    uint32_t type;
    uint32_t task_id;
    uint32_t command_code;
    uint32_t argument;
    uint32_t payload_size;
} TaskHeader;
```

The master would then send a header followed by the script or command payload. This is more flexible but requires more protocol work.

For the current project, the simple shared-script approach is recommended.

## Recommended Implementation Plan

### Phase 1: Fixed FFmpeg Workload

Add one built-in FFmpeg workload:

```text
command_code = 3
argument = segment_id
```

The worker runs a known command template:

```text
process videos/segments/part_%03u.mp4
write videos/processed/part_%03u.mp4
```

This proves distributed video processing before adding controlled RCE.

### Phase 2: Shared Storage Scripts

Add controlled script execution:

```text
command_code = 4
argument = segment_id
task_id = script ID
```

The worker executes a trusted script from shared storage:

```text
videos/jobs/task_301.sh
```

The script receives the segment ID as an argument.

### Phase 3: Aggregation Tools

Add helper scripts to:

- split input video
- submit one task per segment
- generate concat list
- merge processed output
- clean old segment/output files

### Phase 4: Parallel Algorithmic Workloads

Add chunked versions of:

- prime counting
- Monte Carlo Pi
- Mandelbrot checksum
- matrix checksum or multiplication

Each workload should follow the same pattern:

```text
master creates chunks
workers compute partial results
master aggregates final result
```

## Example Demo Scenario

### Setup

```text
Master machine:
  runs ./bin/master
  stores shared videos directory
  submits tasks
  merges final output

Workers:
  Raspberry Pi 1 runs ./bin/worker <master-ip> 9090
  Raspberry Pi 2 runs ./bin/worker <master-ip> 9090
  Raspberry Pi 3 runs ./bin/worker <master-ip> 9090
```

### Demo Flow

1. Start the master.
2. Start three Raspberry Pi workers.
3. Show workers connecting to the master.
4. Split `input.mp4` into 10-second segments.
5. Submit one FFmpeg task per segment.
6. Show the scheduler assigning segment tasks to different workers.
7. Show workers processing segments in parallel.
8. Optionally disconnect one worker and show task recovery.
9. Merge processed segments.
10. Play or inspect the final output video.

### Expected Logs

Example master logs:

```text
[scheduler] task=300 cmd=3 arg=0 -> worker=1
[scheduler] task=301 cmd=3 arg=1 -> worker=2
[scheduler] task=302 cmd=3 arg=2 -> worker=3
[master] result: worker=2 task=301 result=1
[scheduler] task=303 cmd=3 arg=3 -> worker=2
```

This shows dynamic scheduling: worker 2 finished first and received another task.

## Scope Decisions

### Included

The proposed implementation includes:

- distributed FFmpeg segment processing
- controlled FFmpeg remote execution
- shared storage for video files
- chunk-based workload scheduling
- new parallel algorithmic workloads
- timeout handling for remote execution
- task failure reporting

### Excluded

The following are excluded from the current scope:

- full production-grade arbitrary RCE
- authentication and encryption
- automatic worker capability discovery
- sending large video files through TCP
- GPU scheduling
- WAN deployment
- complex priority scheduling

These are reasonable future extensions, but they are not required for a strong final project demo.

## Final Report Summary

The proposed features extend the Distributed Resource Scheduler into a practical distributed job runner. The system will support FFmpeg-based video processing by splitting videos into segments, scheduling segment tasks across workers, and merging the processed outputs. A controlled remote execution mode will allow trusted FFmpeg scripts or commands to be selected at runtime, reducing the need to recompile worker binaries for each new video operation. Large media files will be stored in shared storage, while the existing TCP protocol will continue to manage task assignment, worker status, and results.

The system will also support additional parallel algorithmic workloads such as parallel prime counting, Monte Carlo Pi estimation, Mandelbrot checksums, and matrix computations. These workloads will demonstrate general distributed computation beyond video processing. Fairness and utilization will be achieved through round-robin assignment among idle workers and by decomposing large jobs into many small independent tasks.

Overall, these features preserve the original master-worker architecture while making the project more flexible, demonstrable, and closer to real-world distributed batch-processing systems.

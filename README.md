# Distributed Resource Scheduler (Pure C)

A localized micro-cloud system that pools underutilized computing power across a Local Area Network (LAN) using an asynchronous **Master-Worker** architecture.

## Overview

This project is a distributed resource scheduler built in **Pure C** for **POSIX-compliant Unix systems** such as Linux, macOS, and WSL. Its purpose is to coordinate multiple worker nodes across a LAN and assign computational tasks efficiently.

The design uses:

- **POSIX threads (`pthreads`)** on the Master node
- **Process duplication (`fork()`)** on Worker nodes
- **TCP sockets** for communication
- **POSIX pipes** for inter-process communication
- **`ncurses`** for a terminal-based monitoring dashboard

---

## Architecture

The system follows a **Master-Worker topology**:

- The **Master Node** acts as the central controller
- The **Worker Nodes** register with the Master, receive tasks, execute them, and return results
- A **Terminal Dashboard** provides live system monitoring
- A **Serialization Layer** ensures safe and consistent communication across machines

---

## Core Components

### 1. Master Node (Central Controller)

**Owner:** Rayan Alamri

The Master node is responsible for managing system state and dispatching tasks using a **Round-Robin scheduling algorithm**.

#### TCP Server
- Binds a `SOCK_STREAM` socket
- Handles worker registration and task dispatching
- Uses an infinite `accept()` loop
- Spawns a dedicated `pthread` for each worker connection

#### State Manager
- Maintains a thread-safe structure for connected worker states
- Can be implemented as an array or linked list
- Synchronization is enforced using:
  - `pthread_mutex_lock()`
  - `pthread_mutex_unlock()`

#### Task Queue
- Implemented as a dynamically allocated **FIFO linked list**
- Enqueue operations involve:
  - `malloc()`
  - mutex locking
  - node appending

#### Scheduler Thread
- Runs as a background `pthread`
- Polls the task queue continuously
- Assigns tasks to available `"idle"` workers

---

### 2. Worker Execution Engine

**Owner:** Mohammed Alsuhaibani

The Worker node receives tasks from the Master and executes them in isolated processes.

#### Network Client
- Connects to the Master using its IP address
- Runs a background thread to send periodic heartbeat messages

#### Process Spawner
- Uses `fork()` when a task is received
- The **child process** performs the computational work
- The **parent process** remains responsive for networking and heartbeat handling

#### IPC Bridge
- Uses `pipe()` for parent-child communication
- Streams task results from the child back to the parent
- Parent forwards the result to the Master

---

### 3. Serialization & Communication Layer

**Owner:** Ali Almuzain

This layer ensures memory-safe and architecture-safe data transmission.

#### Fixed-Size Structs
- Uses predefined C structs for network payloads
- Prevents stream fragmentation
- Avoids variable-length string parsing issues

#### Byte Order Conversion
- Uses:
  - `htonl()`
  - `ntohl()`
- Ensures compatibility across machines with different endianness

#### Robust `recv()` Handling
- Uses a loop to keep receiving data until the exact payload size is read
- Prevents partial-read errors

Example approach:

```c
while (bytes_received < sizeof(NetworkPayload)) {
    // recv() until full payload is accumulated
}
```

#### Shared Data Models

##### Message Type Enumeration

```c
typedef enum {
    MSG_HEARTBEAT = 0,
    MSG_TASK = 1,
    MSG_RESULT = 2
} MessageType;
```

##### Universal Network Payload

```c
typedef struct {
    uint32_t type;         // See MessageType enum
    uint32_t worker_id;    // Unique identifier for the worker
    uint32_t task_id;      // Unique ID for the job
    uint32_t command_code; // E.g., 1 for Prime Calc, 2 for Hash
    uint32_t argument;     // Input value for the task
    uint32_t result;       // Computed output (0 if not a result msg)
} NetworkPayload;
```

---

### 4. Terminal Dashboard

**Owner:** Mohammed Yar

The dashboard provides a live, non-blocking terminal UI for monitoring the distributed system.

#### Live Layout
- Built using `<ncurses.h>`
- Renders directly to the terminal buffer
- Uses separate `WINDOW` objects for:
  - task queue
  - node states
  - logs

#### Polling Thread
- Runs as a background `pthread`
- Safely locks the shared state table
- Copies metrics for display
- Calls `wrefresh()` without blocking networking operations

---

## Concurrency Model

### Master Node
- Multi-threaded using `pthreads`
- Separate threads handle:
  - worker connections
  - task scheduling
  - dashboard polling

### Worker Node
- Hybrid thread/process design
- Background thread handles heartbeats
- `fork()` isolates task execution from networking logic

---

## Fault Tolerance

The system should handle worker or socket failure gracefully.

### Required behavior
- Catch socket communication failures
- Mark disconnected workers as `"Offline"`
- Mark workers offline after 15 seconds without a heartbeat
- Re-queue orphaned or incomplete tasks
- Preserve scheduler stability under partial node failure

---

## Project Scope

### Included
- LAN-based distributed task scheduling
- TCP-based communication
- Worker heartbeat monitoring
- Task queue and dispatching
- Result collection
- Terminal-based monitoring UI

### Out of Scope
- GPU pooling
- WAN / internet-based deployment
- Advanced cryptographic security

---

## Development Constraints

- **Timeline:** 4 weeks
- **Target:** Minimum Viable Product (MVP)

---

## Suggested Project Structure

```text
.
├── src/
│   ├── master/
│   │   ├── queue_mgr.c
│   │   ├── queue_mgr.h
│   │   ├── scheduler.c
│   │   ├── scheduler.h
│   │   ├── server.c
│   │   └── server.h
│   ├── worker/
│   │   ├── client.c
│   │   ├── client.h
│   │   ├── executor.c
│   │   └── executor.h
│   ├── shared/
│   │   ├── protocol.c
│   │   ├── protocol.h
│   │   └── models.h
│   ├── ui/
│   │   ├── dashboard.c
│   │   └── dashboard.h
│   └── workloads/
│       ├── matrix_math.c
│       └── prime_calc.c
├── tests/
│   ├── test_forking.c
│   ├── test_protocol.c
│   └── test_scheduler.c
├── .gitignore
├── Makefile
└── README.md
```

---

## Running and Testing the Project

These commands match the execution workflow documented in the final report.

### Requirements
- Linux, WSL, macOS, or another POSIX-like environment
- `gcc`
- `make`
- `pthread` support
- `ncurses` development package
- `ffmpeg` and `ffprobe` for video workloads
- `cifs-utils` on Raspberry Pi workers that mount the shared Windows `videos/` folder
- Docker and Docker Compose for container simulation
- For small clones on worker devices, use a shallow clone: `git clone --depth 1 <repo-url>`

Generated video files should not be committed. Keep large input videos and generated outputs under `videos/` locally; `.gitignore` keeps `videos/input`, `videos/processed`, and `videos/final` contents out of Git while preserving their `.gitkeep` files.

### Native Run Commands

Build the binaries once from the project root:

```bash
make
```

Start the master in the first terminal:

```bash
./bin/master
```

Start one worker in a second terminal:

```bash
./bin/worker 127.0.0.1 9090
```

Start more workers by running the same worker command in additional terminals. In an interactive master terminal, the ncurses dashboard opens automatically. Press `n` to submit a non-video task batch, or press `v` to process a video from `videos/input`. The video form lets you choose either segment-file processing or time-range processing before merging the final output.

### Raspberry Pi Worker over LAN from WSL

If the master runs inside WSL and a Raspberry Pi connects over the same LAN, the Pi should connect to the Windows Wi-Fi/Ethernet IP, not the WSL IP. Find the Windows IP with `ipconfig` in PowerShell, then forward port `9090` from Windows to the WSL IP.

In WSL, get the WSL IP:

```bash
hostname -I
```

Use the WSL `eth0` address, not Docker bridge addresses such as `172.17.0.1` or `172.19.0.1`. Then run PowerShell as Administrator:

```powershell
netsh interface portproxy delete v4tov4 listenaddress=0.0.0.0 listenport=9090
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=9090 connectaddress=<wsl-ip> connectport=9090
New-NetFirewallRule -DisplayName "DRS Master 9090" -Direction Inbound -Protocol TCP -LocalPort 9090 -Action Allow
```

From the Raspberry Pi, test the TCP port and then start the worker:

```bash
nc -vz <windows-lan-ip> 9090
./bin/worker <windows-lan-ip> 9090
```

For FFmpeg video processing with a real Raspberry Pi worker, the Pi must also see the same `videos/` directory as the master. Share the Windows folder `distributed-resource-scheduler/videos` as `drs-videos`, then mount it on the Pi:

```bash
sudo apt update
sudo apt install cifs-utils ffmpeg
sudo mkdir -p /mnt/drs-videos
sudo mount -t cifs //<windows-lan-ip>/drs-videos /mnt/drs-videos -o username=<windows-user>,uid=$(id -u),gid=$(id -g),rw,vers=3.0
```

Verify that the Pi can see the shared folders:

```bash
ls /mnt/drs-videos
ls /mnt/drs-videos/segments
```

Launch the master with the local repo video directory, and launch the Pi worker with the mounted video directory:

```bash
# WSL/laptop
VIDEO_DIR=./videos ./bin/master

# Raspberry Pi
VIDEO_DIR=/mnt/drs-videos ./bin/worker <windows-lan-ip> 9090
```

Run the unit tests with:

```bash
make test
```

### Docker Fixed-Core Simulation

Build the image and create a Docker network:

```bash
docker build -t drs .
docker network create drs-net
```

Start the master in one terminal:

```bash
docker run --rm -it --name drs-master \
  --network drs-net \
  -p 9090:9090 \
  -e MASTER_PORT=9090 \
  -e MASTER_DASHBOARD=1 \
  -e TERM=xterm-256color \
  -v "$PWD/videos:/videos" \
  -v "$PWD/logs:/logs" \
  drs master
```

Launch each worker in a separate terminal. Use a unique container name and CPU set for each worker:

```bash
docker run --rm -it --name drs-worker-1 \
  --network drs-net \
  --cpuset-cpus="0,1" \
  -e VIDEO_DIR=/videos \
  -v "$PWD/videos:/videos" \
  -v "$PWD/logs:/logs" \
  drs worker drs-master 9090
```

Worker container names cannot be reused while containers are running. If you start more workers, change the number in `--name drs-worker-1` for each new worker, such as `drs-worker-2`, `drs-worker-3`, and so on. If two workers use the same name, Docker will fail because that container name is already in use.

The `--cpuset-cpus` option pins that worker container to specific host CPU cores. The example uses cores `0` and `1`. If your machine does not have enough CPU cores, change the value to a smaller set such as `--cpuset-cpus="0"`, or remove the `--cpuset-cpus` line entirely so Docker can schedule the container on any available core.

Submit work from another terminal:

```bash
docker run --rm -it \
  --network drs-net \
  drs submit -h drs-master -p 9090 -c 8 -n 10 -a 0 -s 100 -e 1000
```

Stop the simulation by pressing `Ctrl+C` in the master and worker terminals, then remove the network:

```bash
docker network rm drs-net
```

---

## License
[Add license if applicable] 

## Contributors
- Rayan Alamri (Master Node)
- Mohammed Alsuhaibani (Worker Execution)
- Ali Almuzain (Serialization)
- Mohammed Yar (Dashboard) 

---

---

## Build Requirements

- C compiler (`gcc` or `clang`)
- POSIX environment
- `pthread`
- BSD sockets / POSIX sockets
- `ncurses`

Example build flags may include:

```bash
gcc -pthread -lncurses -o program source.c
```

---

## Summary

This project delivers a lightweight distributed scheduler in Pure C for Unix-like systems. By combining socket networking, multithreading, process isolation, and terminal visualization, it provides a practical foundation for experimenting with distributed systems concepts in a controlled LAN environment.

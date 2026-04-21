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

This section provides step-by-step instructions to build, run, and test the distributed resource scheduler.

### Prerequisites
- **Linux, macOS, or WSL** (POSIX-compliant system).
- **GCC** and **Make** for building.
- **Docker** and **Docker Compose** for containerized testing (optional but recommended).
- Basic knowledge of terminal commands.

### Building the Project
1. Clone or navigate to the project directory.
2. Run `make all` to compile the binaries:
   ```
   make all
   ```
   - This generates `bin/master`, `bin/worker`, and `bin/submit` executables.

### Running Locally (Without Containers)
#### Start the Master
1. Run the master on a specific port (default: 9090):
   ```
   ./bin/master
   ```
   - The master listens for worker connections and dispatches demo tasks (6 tasks by default).

#### Start Workers
1. In separate terminals, run workers connecting to the master:
   ```
   ./bin/worker <master_host> <port>
   ```
   - Example: `./bin/worker 127.0.0.1 9090`
   - Start multiple workers for distributed execution.

2. Alternatively, use the provided script to start multiple workers:
   ```
   ./start-workers.sh <count> <host> <port>
   ```
   - Example: `./start-workers.sh 3 127.0.0.1 9090` (starts 3 workers).

#### Monitor and Test
- The master logs task assignments and results.
- Workers execute tasks (e.g., prime calculations or matrix checksums) and send results back.
- Test fault tolerance: Kill a worker process—the master should requeue tasks and mark it offline.

### Running with Docker (Containerized Simulation)

Docker lets you run the entire distributed system on a single machine. Each component runs in its own isolated container connected through a virtual network — no physical Raspberry Pis needed.

#### What runs inside Docker

| Container | Role | Simulates |
|-----------|------|-----------|
| `master` | Receives tasks, schedules them to workers | Central controller |
| `worker_rpi3` | Executes tasks (1 CPU, 512 MB) | Raspberry Pi 3 |
| `worker_rpi4` | Executes tasks (2 CPU, 1 GB) | Raspberry Pi 4 |
| `worker_rpi5` | Executes tasks (4 CPU, 2 GB) | Raspberry Pi 5 |
| `submit` | One-shot client to inject tasks | External user/client |

---

#### Prerequisites

Install Docker Desktop from https://www.docker.com/products/docker-desktop and make sure it is running. Verify with:

```bash
docker --version
docker compose version
```

You should see version numbers printed for both. If not, Docker is not installed or not running.

---

#### Step 1 — Build the images

This compiles the C code and packages it into Docker images. Run this once, and again any time you change the source code.

```bash
docker compose build
```

---

#### Step 2 — Start the cluster

```bash
docker compose up -d
```

The `-d` flag runs everything in the background. This starts 4 containers: master + 3 workers.

To confirm all containers are running:

```bash
docker compose ps
```

You should see `master`, `worker_rpi3`, `worker_rpi4`, and `worker_rpi5` all with status `running`.

---

#### Step 3 — Watch the logs

Open a terminal and run:

```bash
docker compose logs -f
```

This streams live output from all containers, color-coded by service name. You will see:
- Workers connecting and registering with the master
- 6 demo tasks automatically submitted after 3 seconds
- The scheduler assigning each task to a worker (e.g. `task=1 -> worker=2`)
- Results coming back (e.g. `result: worker=2 task=1 result=348513`)

To watch only one container:

```bash
docker compose logs -f master
docker compose logs -f worker_rpi3
docker compose logs -f worker_rpi4
docker compose logs -f worker_rpi5
```

Press `Ctrl+C` to stop following logs. The containers keep running.

---

#### Step 4 — Submit tasks

Open a second terminal and use the `submit` client to send tasks to the running cluster.

**Option A: run submit directly from your terminal**

```bash
# 1 prime task with default argument (5 million)
docker compose run --rm submit

# 5 prime tasks, count primes up to 100 million (heavier load)
docker compose run --rm submit -c 1 -n 5 -a 100000000

# 2 matrix tasks
docker compose run --rm submit -c 2 -a 1000 -n 2
```

`--rm` automatically removes the submit container after it exits (it is a one-shot tool).

**Option B: go inside a container and submit interactively**

```bash
# open a shell inside the master container
docker exec -it distributed-resource-scheduler-master-1 sh

# now run submit as many times as you want
submit -c 1 -n 5 -a 100000000
submit -c 2 -a 500 -n 3
submit -a 4000000000 -n 1

# leave the container (master keeps running)
exit
```

**Submit flags reference:**

| Flag | Description | Default |
|------|-------------|---------|
| `-n COUNT` | Number of tasks to send | `1` |
| `-c CMD` | Workload type: `1`=prime counting, `2`=matrix checksum | `1` |
| `-a ARG` | Input to the computation (see note below) | `5000000` |
| `-i ID` | Starting task ID (to avoid collisions with demo tasks) | `100` |
| `-h HOST` | Master hostname | `master` |
| `-p PORT` | Master port | `9090` |

**What `-a ARG` means per workload:**
- `-c 1` (prime): counts all prime numbers from 2 up to `ARG`. Larger = slower. `5000000` takes ~1s, `100000000` takes ~1 min per worker.
- `-c 2` (matrix): computes a checksum over an `ARG × ARG` matrix. `ARG=1000` means a 1000×1000 matrix.

The maximum value for `-a` is `4294967295` (~4.3 billion).

---

#### Step 5 — Test fault tolerance

You can simulate a worker failure by stopping a container while tasks are running:

```bash
# submit some slow tasks first so workers stay busy
docker compose run --rm submit -c 1 -n 10 -a 100000000

# then kill a worker
docker stop distributed-resource-scheduler-worker_rpi3-1
```

Watch the logs — the master will detect the disconnection and requeue the orphaned task so another worker picks it up.

To bring the worker back:

```bash
docker start distributed-resource-scheduler-worker_rpi3-1
```

---

#### Step 6 — Stop everything

```bash
docker compose down
```

This stops and removes all containers and the virtual network. Your images are kept so you do not need to rebuild next time, only run `docker compose up -d` again.

---

#### Full command reference

```bash
docker compose build                  # build/rebuild images
docker compose up -d                  # start the cluster in background
docker compose ps                     # check container status
docker compose logs -f                # stream all logs
docker compose logs -f master         # stream master logs only
docker compose run --rm submit [flags]  # submit tasks
docker exec -it <container> sh        # open a shell inside a container
docker compose down                   # stop and remove everything
```

---

#### Troubleshooting

**"Cannot connect to the Docker daemon"**
Docker Desktop is not running. Open it and wait for it to start.

**"port is already allocated"**
Something on your machine is already using port 9090. Either stop that process or change `MASTER_PORT` in `docker-compose.yml` and rebuild.

**Submit says "cannot resolve master"**
The submit container started before the master was ready. Wait a few seconds and try again.

**Want to start fresh?**
```bash
docker compose down
docker system prune -f
docker compose build
docker compose up -d
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

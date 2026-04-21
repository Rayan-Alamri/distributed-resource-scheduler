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
   - This generates `bin/master` and `bin/worker` executables.

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
For easier testing and simulation of multiple devices (e.g., Raspberry Pis), use Docker.

#### Build Images
```
docker-compose build
```

#### Run with Scaling
- Start the master and scale workers:
  ```
  ./scale_workers.sh <rpi3_count> <rpi4_count> <rpi5_count>
  ```
  - Example: `./scale_workers.sh 2 1 1` (2 RPi3, 1 RPi4, 1 RPi5 workers with resource limits).

- Or manually:
  ```
  docker-compose up master
  ./manage_workers.sh add rpi3 2
  ```

#### Dynamic Connect/Disconnect Simulation
- Add workers: `./manage_workers.sh add <type> <count>`
- Remove workers: `./manage_workers.sh remove <container_name>`
- Check logs: `docker-compose logs -f master`
- The master detects disconnections and requeues tasks.

#### Stop Everything
```
docker-compose down
```

### Testing Specific Features
- **Task Execution**: Monitor logs for task completion (e.g., prime counts or checksums).
- **Fault Tolerance**: Disconnect workers and verify requeuing.
- **Performance**: Run with different worker types and measure task distribution.
- **Unit Tests**: Run test files in `tests/` (e.g., `./test_protocol`).

### Troubleshooting
- **Port Issues**: Ensure port 9090 is free.
- **Connection Errors**: Check firewall and network settings.
- **Build Errors**: Ensure GCC and dependencies are installed.
- **Docker Issues**: Run `docker system prune` to clean up.

For more details, refer to the architecture sections above. If you encounter issues, check the logs or open an issue. 

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

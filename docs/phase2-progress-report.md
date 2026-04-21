# Phase 2 Progress Report: Distributed Resource Scheduler

**Project Title:** Distributed Resource Scheduler (Pure C)  
**Date:** April 22, 2026  
**Team Members:** Rayan Alamri, Mohammed Alsuhaibani, Ali Almuzain, Mohammed Yar  

## 1. Brief Recap of Project Idea

The Distributed Resource Scheduler is a localized micro-cloud system designed to pool underutilized computing power across a Local Area Network (LAN) using an asynchronous Master-Worker architecture. The goal is to coordinate multiple worker nodes (e.g., Raspberry Pis) to execute computational tasks efficiently, such as prime number calculations and matrix operations. Built entirely in Pure C for POSIX-compliant systems, it emphasizes fault tolerance, concurrency, and scalability without external dependencies.

Key features include:
- **Master Node:** Central controller for task dispatching and worker management.
- **Worker Nodes:** Execute tasks in isolated processes and report results.
- **Communication:** TCP sockets with a custom binary protocol.
- **Fault Tolerance:** Handles worker disconnections by requeueing tasks.

## 2. What Has Been Implemented So Far

Approximately 60-70% of the core system is implemented, focusing on the essential modules for a Minimum Viable Product (MVP). The implementation includes working code for the main components:

### Core Modules Implemented:
- **Master Node (server.c, scheduler.c, queue_mgr.c):**
  - TCP server for accepting worker connections.
  - Round-robin task scheduling.
  - Worker registry with thread-safe operations (using pthreads and mutexes).
  - Task queue management (FIFO linked list).
  - Heartbeat monitoring for liveness detection.
  - Fault tolerance: Requeues orphaned tasks on worker disconnection.

- **Worker Node (client.c, executor.c):**
  - Network client for connecting to the master.
  - Background heartbeat thread.
  - Process spawning (fork) for task isolation.
  - IPC via pipes for parent-child communication.
  - Execution of predefined workloads: Prime counting and matrix checksum.

- **Shared Components (protocol.c, models.h):**
  - Binary serialization for network payloads.
  - Byte-order conversion (htonl/ntohl) for cross-platform compatibility.
  - Robust recv handling to prevent partial reads.

- **Build System (Makefile):**
  - Compiles all modules into `bin/master` and `bin/worker` executables.

### Additional Features:
- **Docker Support:** Containerized deployment with `docker-compose.yml` for easy testing and simulation of heterogeneous devices (e.g., RPi3, RPi4, RPi5 with resource limits).
- **Scripts:** `start-workers.sh` for local multi-worker setup, `scale_workers.sh` and `manage_workers.sh` for dynamic scaling in containers.
- **Testing Infrastructure:** Basic unit tests in `tests/` (e.g., protocol, forking, scheduler).
- **Documentation:** Updated `README.md` with running instructions.

### Current Functionality:
- Master can dispatch demo tasks (6 by default) to connected workers.
- Workers execute tasks and return results.
- System logs task assignments, completions, and disconnections.
- Supports up to MAX_WORKERS (defined in models.h) concurrent workers.

## 3. System Architecture (Updated)

The architecture remains largely unchanged from Phase 1, with refinements for concurrency and fault tolerance.

### High-Level Diagram:
```
[Master Node]
├── TCP Server (Accepts workers, spawns threads)
├── Scheduler (Round-robin assignment)
├── Task Queue (FIFO linked list)
├── Worker Registry (Thread-safe array)
└── Heartbeat Monitor (Detects offline workers)

[Worker Nodes]
├── Network Client (Connects to master)
├── Heartbeat Thread (Periodic updates)
├── Executor (Forks child process for tasks)
└── IPC Bridge (Pipes for result communication)

[Communication Protocol]
├── Fixed-size NetworkPayload structs
├── Message Types: HEARTBEAT, TASK, RESULT
└── Endianness handling
```

### Key Updates:
- Enhanced thread safety with mutexes in the registry.
- Added requeuing logic for failed tasks.
- Docker integration for scalable testing.

## 4. Screenshots of Working System

*(Placeholders: Run the commands below to generate logs/screenshots for inclusion in the PDF.)*

### Screenshot 1: Master Starting and Listening
- **Command to Run:** `cd /path/to/project && ./bin/master`
- **Expected Output:** Logs showing "[master] listening on port 9090"
- **Placeholder:** [Insert screenshot of terminal with master startup logs]

### Screenshot 2: Workers Connecting and Executing Tasks
- **Command to Run:** 
  1. Start master in one terminal: `./bin/master`
  2. In another terminal: `./start-workers.sh 2 127.0.0.1 9090`
- **Expected Output:** Master logs task assignments (e.g., "[master] assigned task 1 to worker 1"), worker executions, and results (e.g., "[master] result: worker=1 task=1 result=...").
- **Placeholder:** [Insert screenshot of master terminal showing task dispatch and results]

### Screenshot 3: Fault Tolerance (Worker Disconnection)
- **Command to Run:**
  1. Start master and 2 workers as above.
  2. Kill one worker process (Ctrl+C in its terminal).
- **Expected Output:** Master logs "[master] worker X disconnected" and "[master] requeueing orphaned task Y".
- **Placeholder:** [Insert screenshot of master logs showing disconnection and requeuing]

### Screenshot 4: Docker Simulation
- **Command to Run:** `docker-compose build && ./scale_workers.sh 1 1 1`
- **Expected Output:** Containers start, master listens, workers connect and process tasks.
- **Placeholder:** [Insert screenshot of docker-compose logs or `docker ps` showing running containers]

## 5. Challenges Faced

- **Concurrency Issues:** Ensuring thread safety in the worker registry required careful use of pthreads and mutexes to avoid race conditions during reads/writes.
- **Network Reliability:** Handling partial socket reads and endianness differences across machines (e.g., big-endian vs. little-endian).
- **Process Isolation:** Implementing fork-based task execution while maintaining IPC via pipes, ensuring child processes don't interfere with networking.
- **Testing in Containers:** Simulating heterogeneous devices (e.g., RPi variants) with Docker resource limits, and managing dynamic scaling without rebuilding.
- **Debugging:** Pure C with manual memory management led to segmentation faults; extensive use of gdb and valgrind was necessary.
- **Time Constraints:** Balancing implementation depth with breadth across modules (e.g., prioritizing core scheduling over advanced features like the dashboard).

## 6. Plan for Completing Remaining Work

The remaining 30-40% focuses on enhancements, UI, and robustness for Phase 3/final submission.

### Short-Term (Next 2 Weeks):
- **Complete Dashboard (UI):** Implement the ncurses-based terminal UI for live monitoring (task queue, worker states, logs).
- **Enhanced Fault Tolerance:** Add worker timeout detection and automatic removal of stale connections.
- **Testing:** Expand unit tests (e.g., full integration tests for master-worker communication) and add performance benchmarks.

### Medium-Term (Phase 3):
- **Scalability Improvements:** Support for more workers (increase MAX_WORKERS) and load balancing algorithms beyond round-robin.
- **Security:** Add basic authentication or encryption for network payloads.
- **Web Interface:** Optional REST API for external monitoring (using a simple HTTP server).
- **Documentation:** Complete API docs and user manual.

### Long-Term (Post-Submission):
- **Real Device Deployment:** Test on actual Raspberry Pis, integrating with GPIO or sensors.
- **Advanced Features:** Remote code execution (with sandboxing), GPU support, or WAN extension.

### Milestones:
- Week 1: Finish dashboard and basic UI.
- Week 2: Comprehensive testing and bug fixes.
- Week 3: Performance optimization and documentation.
- Week 4: Final integration and real-device testing.

## 7. Test Cases

*(Run the commands below to generate outputs for the report. Include logs/screenshots as evidence.)*

### Test Case 1: Basic Task Execution
- **Steps:** Start master, connect 1 worker, observe task completion.
- **Expected:** Tasks assigned and results logged.
- **Command:** `./bin/master` (terminal 1), `./bin/worker 127.0.0.1 9090` (terminal 2).

### Test Case 2: Multi-Worker Load Balancing
- **Steps:** Start master, connect 3 workers.
- **Expected:** Tasks distributed round-robin.
- **Command:** `./start-workers.sh 3 127.0.0.1 9090`.

### Test Case 3: Fault Tolerance
- **Steps:** Start master and workers, disconnect one worker mid-task.
- **Expected:** Task requeued and completed by another worker.
- **Command:** As in Screenshot 3.

### Test Case 4: Docker Scaling
- **Steps:** Use scripts to scale workers dynamically.
- **Expected:** Containers start/stop, tasks processed.
- **Command:** `./manage_workers.sh add rpi4 2`, then `./manage_workers.sh remove rpi4_worker_1`.

### Test Case 5: Unit Tests
- **Steps:** Run test executables.
- **Expected:** Pass/fail reports.
- **Command:** `./test_protocol`, `./test_scheduler`, etc.

## Conclusion

Phase 2 has established a solid foundation with core scheduling, networking, and fault tolerance implemented. The system successfully demonstrates distributed task execution and dynamic worker management. Challenges in concurrency and testing have been addressed, paving the way for UI completion and advanced features in Phase 3. The Docker setup enables realistic simulation for future Pi deployments.

For submission, attach the code ZIP/Github link and this PDF report.
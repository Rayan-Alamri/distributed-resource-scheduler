# Distributed Resource Scheduler (MVP)

This project is a localized Distributed Resource Scheduler designed to pool computing resources across a network grid[cite: 2, 5]. It utilizes a Master-Worker topology over raw POSIX TCP/IP sockets[cite: 8, 15].

## ?? Prerequisites
* **C Compiler:** GCC (`build-essential`)
* **Build Tool:** Make
* **UI Library:** ncurses
* **Environment:** Unix-based (Linux, macOS, or WSL)[cite: 23].

## ?? Setup Instructions
1. **Clone the repo:** `git clone <url>`
2. **Prepare:** `mkdir bin`
3. **Compile:** `make`

## ??? Development Tracks
* **Track 1 (Rayan Alamri):** Master node logic, Round Robin scheduling, and FIFO task queue[cite: 9, 10, 37].
* **Track 2 (Mohammed Alsuhaibani):** Worker execution engine and child process management via `fork()`[cite: 11, 38].
* **Track 3 (Ali Almuzain):** IPC protocol and data serialization[cite: 8, 39].
* **Track 4 (Mohammed Yar):** Live terminal dashboard and fault-tolerance handling[cite: 13, 17, 40].

## ?? Git Workflow
1. `git pull origin main` (Start of day)
2. `git add .`
3. `git commit -m "Your message"`
4. `git push origin main`

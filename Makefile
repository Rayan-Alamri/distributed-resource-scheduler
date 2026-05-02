CC     = gcc
CFLAGS = -Wall -Wextra -g -pthread
# -lncurses only needed once the dashboard UI is implemented (Mohammed Yar's scope)
LDFLAGS_NCURSES = -lncurses

MASTER_SRCS = src/master/server.c \
              src/master/scheduler.c \
              src/master/queue_mgr.c \
              src/master/status.c \
              src/shared/protocol.c \
              src/ui/dashboard.c

WORKER_SRCS = src/worker/client.c \
              src/worker/executor.c \
              src/shared/protocol.c

SUBMIT_SRCS = src/client/submit.c \
              src/shared/protocol.c

TEST_SCHED_SRCS = tests/test_scheduler.c \
                  src/master/queue_mgr.c \
                  src/master/scheduler.c \
                  src/shared/protocol.c

TEST_WORKER_SRCS = tests/test_worker_executor.c \
                   src/worker/executor.c \
                   src/shared/protocol.c

VIDEO_DIR  ?= ./videos
INPUT_FILE ?= input.mp4
MASTER_HOST ?= 127.0.0.1
MASTER_PORT ?= 9090

.PHONY: all master worker submit test_scheduler test_worker test clean \
        up down logs \
        video-split video-submit video-merge video-pipeline \
        demo-prime demo-matrix demo-monte-carlo demo-mandelbrot

all: master worker submit

master: $(MASTER_SRCS) | bin
	$(CC) $(CFLAGS) -o bin/master $(MASTER_SRCS)

worker: $(WORKER_SRCS) | bin
	$(CC) $(CFLAGS) -o bin/worker $(WORKER_SRCS)

submit: $(SUBMIT_SRCS) | bin
	$(CC) $(CFLAGS) -o bin/submit $(SUBMIT_SRCS)

test_scheduler: $(TEST_SCHED_SRCS) | bin
	$(CC) $(CFLAGS) -o bin/test_scheduler $(TEST_SCHED_SRCS)
	./bin/test_scheduler

test_worker: $(TEST_WORKER_SRCS) | bin
	$(CC) $(CFLAGS) -o bin/test_worker_executor $(TEST_WORKER_SRCS)
	./bin/test_worker_executor

test: test_scheduler test_worker

bin:
	mkdir -p bin

clean:
	rm -rf bin/*

# ── Docker shortcuts ──────────────────────────────────────────────────────────

up:
	docker compose up -d --build

down:
	docker compose down

logs:
	docker compose logs -f

# ── FFmpeg video pipeline ─────────────────────────────────────────────────────

video-split:
	VIDEO_DIR=$(VIDEO_DIR) INPUT_FILE=$(INPUT_FILE) ./scripts/split_video.sh

video-submit: bin/submit
	@SEG_COUNT=$$(ls $(VIDEO_DIR)/segments/part_*.mp4 2>/dev/null | wc -l); \
	if [ "$$SEG_COUNT" -eq 0 ]; then \
		echo "No segments found in $(VIDEO_DIR)/segments/ — run 'make video-split' first"; \
		exit 1; \
	fi; \
	echo "Submitting $$SEG_COUNT ffmpeg_segment tasks..."; \
	./bin/submit -h $(MASTER_HOST) -p $(MASTER_PORT) -c 6 -n $$SEG_COUNT -a 0 -s 1 -i 300

video-wait:
	@SEG_COUNT=$$(ls $(VIDEO_DIR)/segments/part_*.mp4 2>/dev/null | wc -l); \
	if [ "$$SEG_COUNT" -eq 0 ]; then echo "No segments found"; exit 1; fi; \
	echo "Waiting for $$SEG_COUNT segment(s) to finish processing..."; \
	START=$$(date +%s); \
	while true; do \
		DONE=$$(ls $(VIDEO_DIR)/processed/part_*.mp4 2>/dev/null | wc -l); \
		printf "  processed: $$DONE / $$SEG_COUNT\r"; \
		if [ "$$DONE" -ge "$$SEG_COUNT" ]; then \
			SIZE1=$$(du -sb $(VIDEO_DIR)/processed/ 2>/dev/null | cut -f1); \
			sleep 3; \
			SIZE2=$$(du -sb $(VIDEO_DIR)/processed/ 2>/dev/null | cut -f1); \
			if [ "$$SIZE1" = "$$SIZE2" ]; then \
				BAD=0; \
				for f in $(VIDEO_DIR)/processed/part_*.mp4; do \
					ffprobe -v error "$$f" >/dev/null 2>&1 || BAD=1; \
				done; \
				if [ "$$BAD" -eq 0 ]; then \
					echo ""; echo "All segments done."; break; \
				fi; \
				echo ""; echo "Processed segment validation failed. Check worker logs."; exit 1; \
			fi; \
		fi; \
		NOW=$$(date +%s); \
		if [ $$((NOW - START)) -gt 300 ]; then \
			echo ""; echo "Timed out waiting for processed segments. Check master/worker logs."; exit 1; \
		fi; \
		sleep 5; \
	done

video-merge:
	VIDEO_DIR=$(VIDEO_DIR) INPUT_FILE=$(INPUT_FILE) ./scripts/merge_video.sh

video-pipeline: video-split video-submit video-wait video-merge

# ── Quick demo workloads ──────────────────────────────────────────────────────

demo-prime: bin/submit
	./bin/submit -h $(MASTER_HOST) -p $(MASTER_PORT) -c 1 -n 4 -a 10000000

demo-matrix: bin/submit
	./bin/submit -h $(MASTER_HOST) -p $(MASTER_PORT) -c 2 -n 10 -a 500 -i 200

demo-monte-carlo: bin/submit
	./bin/submit -h $(MASTER_HOST) -p $(MASTER_PORT) -c 4 -n 4 -a 10000000

demo-mandelbrot: bin/submit
	./bin/submit -h $(MASTER_HOST) -p $(MASTER_PORT) -c 5 -n 6 -a 0 -s 100

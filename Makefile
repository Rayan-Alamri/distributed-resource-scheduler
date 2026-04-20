CC     = gcc
CFLAGS = -Wall -Wextra -g -pthread
# -lncurses only needed once the dashboard UI is implemented (Mohammed Yar's scope)
LDFLAGS_NCURSES = -lncurses

MASTER_SRCS = src/master/server.c \
              src/master/scheduler.c \
              src/master/queue_mgr.c \
              src/shared/protocol.c \
              src/ui/dashboard.c

WORKER_SRCS = src/worker/client.c \
              src/worker/executor.c \
              src/shared/protocol.c

TEST_SCHED_SRCS = tests/test_scheduler.c \
                  src/master/queue_mgr.c \
                  src/master/scheduler.c \
                  src/shared/protocol.c

.PHONY: all master worker test_scheduler test clean

all: master worker

master: $(MASTER_SRCS) | bin
	$(CC) $(CFLAGS) -o bin/master $(MASTER_SRCS)

worker: $(WORKER_SRCS) | bin
	$(CC) $(CFLAGS) -o bin/worker $(WORKER_SRCS)

test_scheduler: $(TEST_SCHED_SRCS) | bin
	$(CC) $(CFLAGS) -o bin/test_scheduler $(TEST_SCHED_SRCS)
	./bin/test_scheduler

test: test_scheduler

bin:
	mkdir -p bin

clean:
	rm -rf bin/*

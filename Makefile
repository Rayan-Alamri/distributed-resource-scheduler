CC = gcc
CFLAGS = -Wall -Wextra -g -pthread
LDFLAGS = -lncurses

all: master worker

master: src/master/server.c src/master/scheduler.c src/shared/protocol.c src/ui/dashboard.c
$(CC) $(CFLAGS) -o bin/master $^ $(LDFLAGS)

worker: src/worker/client.c src/worker/executor.c src/shared/protocol.c
$(CC) $(CFLAGS) -o bin/worker $^

clean:
rm -rf bin/*

#include "protocol.h"
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

int recv_full(int fd, void *buf, size_t len) {
    size_t total = 0;
    char *p = (char *)buf;
    /*
     * TCP is a byte stream, not a message stream. A single recv() may return a
     * partial NetworkPayload, so keep reading until the requested size arrives.
     */
    while (total < len) {
        ssize_t n = recv(fd, p + total, len - total, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        total += (size_t)n;
    }
    return 0;
}

int send_full(int fd, const void *buf, size_t len) {
    size_t total = 0;
    const char *p = (const char *)buf;
    /*
     * send() can also write only part of the buffer. Looping here centralizes
     * reliable fixed-size frame transmission for master and worker code.
     */
    while (total < len) {
        ssize_t n = send(fd, p + total, len - total, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        total += (size_t)n;
    }
    return 0;
}

void payload_to_net(NetworkPayload *p) {
    /* Normalize all integer fields before crossing the network boundary. */
    p->type         = htonl(p->type);
    p->worker_id    = htonl(p->worker_id);
    p->task_id      = htonl(p->task_id);
    p->command_code = htonl(p->command_code);
    p->argument     = htonl(p->argument);
    p->result       = htonl(p->result);
}

void payload_to_host(NetworkPayload *p) {
    /* Convert wire-format integers back to the host CPU's native byte order. */
    p->type         = ntohl(p->type);
    p->worker_id    = ntohl(p->worker_id);
    p->task_id      = ntohl(p->task_id);
    p->command_code = ntohl(p->command_code);
    p->argument     = ntohl(p->argument);
    p->result       = ntohl(p->result);
}

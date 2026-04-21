#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "../shared/protocol.h"
#include "../shared/models.h"

#define DEFAULT_HOST     "master"
#define DEFAULT_PORT     9090
#define DEFAULT_COUNT    1
#define DEFAULT_CMD      1          /* 1=prime, 2=matrix */
#define DEFAULT_ARG      5000000u
#define DEFAULT_START_ID 100u

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  -h HOST     master hostname          (default: %s)\n"
        "  -p PORT     master port              (default: %d)\n"
        "  -n COUNT    number of tasks to send  (default: %d)\n"
        "  -c CMD      1=prime  2=matrix        (default: %d)\n"
        "  -a ARG      argument (0..4294967295) (default: %u)\n"
        "  -i ID       starting task ID         (default: %u)\n"
        "\n"
        "Environment variables (overridden by flags):\n"
        "  SUBMIT_HOST, SUBMIT_PORT, SUBMIT_COUNT,\n"
        "  SUBMIT_CMD,  SUBMIT_ARG,  SUBMIT_START_ID\n"
        "\n"
        "Examples:\n"
        "  %s -a 100000000 -n 4          # 4 prime tasks up to 100 million\n"
        "  %s -c 2 -a 1000 -n 2          # 2 matrix tasks (size 1000)\n",
        prog,
        DEFAULT_HOST, DEFAULT_PORT, DEFAULT_COUNT, DEFAULT_CMD,
        DEFAULT_ARG,  DEFAULT_START_ID,
        prog, prog);
}

static uint32_t parse_u32(const char *s) {
    unsigned long v = strtoul(s, NULL, 10);
    if (v > 0xFFFFFFFFUL) {
        fprintf(stderr, "submit: value %lu exceeds uint32 max, clamping to 4294967295\n", v);
        v = 0xFFFFFFFFUL;
    }
    return (uint32_t)v;
}

int main(int argc, char *argv[]) {
    const char *host     = DEFAULT_HOST;
    int         port     = DEFAULT_PORT;
    int         count    = DEFAULT_COUNT;
    uint32_t    cmd      = DEFAULT_CMD;
    uint32_t    arg      = DEFAULT_ARG;
    uint32_t    start_id = DEFAULT_START_ID;

    /* env var fallbacks — useful when running as a docker compose service */
    if (getenv("SUBMIT_HOST"))     host     = getenv("SUBMIT_HOST");
    if (getenv("SUBMIT_PORT"))     port     = atoi(getenv("SUBMIT_PORT"));
    if (getenv("SUBMIT_COUNT"))    count    = atoi(getenv("SUBMIT_COUNT"));
    if (getenv("SUBMIT_CMD"))      cmd      = parse_u32(getenv("SUBMIT_CMD"));
    if (getenv("SUBMIT_ARG"))      arg      = parse_u32(getenv("SUBMIT_ARG"));
    if (getenv("SUBMIT_START_ID")) start_id = parse_u32(getenv("SUBMIT_START_ID"));

    int opt;
    while ((opt = getopt(argc, argv, "h:p:n:c:a:i:")) != -1) {
        switch (opt) {
        case 'h': host     = optarg;            break;
        case 'p': port     = atoi(optarg);      break;
        case 'n': count    = atoi(optarg);      break;
        case 'c': cmd      = parse_u32(optarg); break;
        case 'a': arg      = parse_u32(optarg); break;
        case 'i': start_id = parse_u32(optarg); break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (count <= 0 || count > 1000) {
        fprintf(stderr, "submit: -n must be between 1 and 1000\n");
        return 1;
    }
    if (cmd < 1 || cmd > 2) {
        fprintf(stderr, "submit: -c must be 1 (prime) or 2 (matrix)\n");
        return 1;
    }

    struct hostent *he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "submit: cannot resolve '%s'\n", host);
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect");
        close(fd);
        return 1;
    }
    printf("[submit] connected to %s:%d\n", host, port);

    /* Identify as a submit client so the master does not add us to the worker registry */
    NetworkPayload reg;
    memset(&reg, 0, sizeof(reg));
    reg.type = MSG_SUBMIT;
    payload_to_net(&reg);
    if (send_full(fd, &reg, sizeof(reg)) != 0) {
        fprintf(stderr, "submit: handshake send failed\n");
        close(fd);
        return 1;
    }

    const char *cmd_name = (cmd == 1) ? "prime" : "matrix";
    printf("[submit] submitting %d task(s)  cmd=%u(%s)  arg=%u  start_id=%u\n",
           count, cmd, cmd_name, arg, start_id);

    for (int i = 0; i < count; i++) {
        NetworkPayload task;
        memset(&task, 0, sizeof(task));
        task.type         = MSG_TASK;
        task.task_id      = start_id + (uint32_t)i;
        task.command_code = cmd;
        task.argument     = arg;
        payload_to_net(&task);
        if (send_full(fd, &task, sizeof(task)) != 0) {
            fprintf(stderr, "submit: failed sending task %d\n", i + 1);
            close(fd);
            return 1;
        }
        printf("[submit] queued  task_id=%-4u  cmd=%u(%s)  arg=%u\n",
               start_id + (uint32_t)i, cmd, cmd_name, arg);
    }

    printf("[submit] all tasks submitted\n");
    close(fd);
    return 0;
}

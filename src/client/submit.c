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

/*
 * Command-line submit client.
 *
 * Normal users can submit tasks from the dashboard, but this tool is useful
 * for scripted tests, Docker one-shot clients, and reproducible workload runs.
 * It connects as MSG_SUBMIT, sends a batch of MSG_TASK frames, then exits.
 */

#define DEFAULT_HOST     "master"
#define DEFAULT_PORT     9090
#define DEFAULT_COUNT    1
#define DEFAULT_CMD      1
#define DEFAULT_ARG      5000000u
#define DEFAULT_START_ID 100u

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  -h HOST     master hostname           (default: %s)\n"
        "  -p PORT     master port               (default: %d)\n"
        "  -n COUNT    number of tasks to send   (default: %d)\n"
        "  -c CMD      workload command code      (default: %d)\n"
        "              1=prime          argument = upper bound\n"
        "              2=matrix         argument = matrix size\n"
        "              3=prime_range    (parallel) argument = range_start  (use -s for chunk size)\n"
        "              4=monte_carlo    (parallel) argument = sample count\n"
        "              5=mandelbrot     (parallel) argument = row_start    (use -s 100)\n"
        "              6=ffmpeg_segment (parallel) argument = segment_id   (use -s 1 for sequential)\n"
        "              7=ffmpeg_time_range (parallel) argument = start_second, -s/-e = duration seconds\n"
        "              8=matrix_parallel (parallel) argument = row_start, -e = matrix size\n"
        "  -a ARG      primary argument           (default: %u)\n"
        "  -s STEP     increment argument by STEP per task (default: 0)\n"
        "              For cmd=3: also derives range_end = arg + step - 1 per chunk\n"
        "  -e END      range_end for cmd=3, duration for cmd=7, matrix size for cmd=8\n"
        "  -i ID       starting task ID           (default: %u)\n"
        "\n"
        "Environment variables (overridden by flags):\n"
        "  SUBMIT_HOST, SUBMIT_PORT, SUBMIT_COUNT, SUBMIT_CMD,\n"
        "  SUBMIT_ARG,  SUBMIT_STEP, SUBMIT_RANGE_END, SUBMIT_START_ID\n"
        "\n"
        "Examples:\n"
        "  %s -c 1 -a 100000000 -n 4             # 4 prime tasks up to 100M\n"
        "  %s -c 2 -a 1000 -n 2                  # 2 matrix tasks (size 1000)\n"
        "  %s -c 3 -n 10 -a 1 -s 10000000        # parallel prime: 10 chunks [1,100M]\n"
        "  %s -c 4 -n 4 -a 10000000              # Monte Carlo Pi: 4 x 10M samples\n"
        "  %s -c 5 -n 6 -a 0 -s 100              # Mandelbrot: 6 x 100-row chunks\n"
        "  %s -c 6 -n 12 -a 0 -s 1 -i 300        # FFmpeg: 12 segments (0-11)\n"
        "  %s -c 7 -n 4  -a 0 -s 10 -i 400       # FFmpeg ranges: 0s,10s,20s,30s\n"
        "  %s -c 8 -n 10 -a 0 -s 100 -e 1000     # parallel matrix: size 1000\n",
        prog,
        DEFAULT_HOST, DEFAULT_PORT, DEFAULT_COUNT, DEFAULT_CMD,
        DEFAULT_ARG, DEFAULT_START_ID,
        prog, prog, prog, prog, prog, prog, prog, prog);
}

static uint32_t parse_u32(const char *s) {
    unsigned long v = strtoul(s, NULL, 10);
    if (v > 0xFFFFFFFFUL) {
        fprintf(stderr, "submit: value %lu exceeds uint32 max, clamping\n", v);
        v = 0xFFFFFFFFUL;
    }
    return (uint32_t)v;
}

static const char *cmd_name(uint32_t cmd) {
    switch (cmd) {
    case CMD_PRIME:          return "prime";
    case CMD_MATRIX:         return "matrix";
    case CMD_PRIME_RANGE:    return "prime_range";
    case CMD_MONTE_CARLO:    return "monte_carlo";
    case CMD_MANDELBROT:     return "mandelbrot";
    case CMD_FFMPEG_SEGMENT: return "ffmpeg_segment";
    case CMD_FFMPEG_TIME_RANGE: return "ffmpeg_time_range";
    case CMD_MATRIX_PARALLEL: return "matrix_parallel";
    default:                 return "unknown";
    }
}

int main(int argc, char *argv[]) {
    const char *host     = DEFAULT_HOST;
    int         port     = DEFAULT_PORT;
    int         count    = DEFAULT_COUNT;
    uint32_t    cmd      = DEFAULT_CMD;
    uint32_t    arg      = DEFAULT_ARG;
    uint32_t    step     = 0;
    uint32_t    range_end = 0;
    uint32_t    start_id = DEFAULT_START_ID;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /*
     * Environment variables make Docker and Makefile demos convenient, while
     * command-line flags below remain the final override for manual runs.
     */
    if (getenv("SUBMIT_HOST"))      host      = getenv("SUBMIT_HOST");
    if (getenv("SUBMIT_PORT"))      port      = atoi(getenv("SUBMIT_PORT"));
    if (getenv("SUBMIT_COUNT"))     count     = atoi(getenv("SUBMIT_COUNT"));
    if (getenv("SUBMIT_CMD"))       cmd       = parse_u32(getenv("SUBMIT_CMD"));
    if (getenv("SUBMIT_ARG"))       arg       = parse_u32(getenv("SUBMIT_ARG"));
    if (getenv("SUBMIT_STEP"))      step      = parse_u32(getenv("SUBMIT_STEP"));
    if (getenv("SUBMIT_RANGE_END")) range_end = parse_u32(getenv("SUBMIT_RANGE_END"));
    if (getenv("SUBMIT_START_ID"))  start_id  = parse_u32(getenv("SUBMIT_START_ID"));

    int opt;
    while ((opt = getopt(argc, argv, "h:p:n:c:a:s:e:i:")) != -1) {
        switch (opt) {
        case 'h': host      = optarg;            break;
        case 'p': port      = atoi(optarg);      break;
        case 'n': count     = atoi(optarg);      break;
        case 'c': cmd       = parse_u32(optarg); break;
        case 'a': arg       = parse_u32(optarg); break;
        case 's': step      = parse_u32(optarg); break;
        case 'e': range_end = parse_u32(optarg); break;
        case 'i': start_id  = parse_u32(optarg); break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (count <= 0 || count > 1000) {
        fprintf(stderr, "submit: -n must be between 1 and 1000\n");
        return 1;
    }
    if (cmd < 1 || cmd > 8) {
        fprintf(stderr, "submit: -c must be 1-8 (see -? for details)\n");
        return 1;
    }
    if (cmd == CMD_MATRIX_PARALLEL && range_end == 0) {
        fprintf(stderr, "submit: cmd=8 requires -e MATRIX_SIZE\n");
        return 1;
    }
    if (cmd == CMD_FFMPEG_TIME_RANGE && step == 0 && range_end == 0) {
        fprintf(stderr, "submit: cmd=7 requires -s or -e duration seconds\n");
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

    NetworkPayload reg;
    memset(&reg, 0, sizeof(reg));
    reg.type = MSG_SUBMIT;
    /* The initial frame tells the master this socket is not a worker. */
    payload_to_net(&reg);
    if (send_full(fd, &reg, sizeof(reg)) != 0) {
        fprintf(stderr, "submit: handshake send failed\n");
        close(fd);
        return 1;
    }

    printf("[submit] submitting %d task(s)  cmd=%u(%s)  arg=%u  step=%u  start_id=%u\n",
           count, cmd, cmd_name(cmd), arg, step, start_id);

    for (int i = 0; i < count; i++) {
        NetworkPayload task;
        memset(&task, 0, sizeof(task));
        task.type         = MSG_TASK;
        task.task_id      = start_id + (uint32_t)i;
        task.command_code = cmd;
        task.argument     = arg + (uint32_t)i * step;
        if (cmd == CMD_FFMPEG_TIME_RANGE && step == 0 && range_end > 0)
            task.argument = arg + (uint32_t)i * range_end;

        /*
         * Some workloads use `result` as a second task argument before the
         * worker overwrites it with the computed result.
         */
        if (cmd == CMD_PRIME_RANGE) {
            task.result = (step > 0)
                ? task.argument + step - 1
                : range_end;
        } else if (cmd == CMD_MATRIX_PARALLEL) {
            task.result = range_end;
        } else if (cmd == CMD_FFMPEG_TIME_RANGE) {
            task.result = step > 0 ? step : range_end;
        }

        payload_to_net(&task);
        if (send_full(fd, &task, sizeof(task)) != 0) {
            fprintf(stderr, "submit: failed sending task %d\n", i + 1);
            close(fd);
            return 1;
        }
        /* Print in host order for readability */
        payload_to_host(&task);
        if (cmd == CMD_PRIME_RANGE)
            printf("[submit] queued  task_id=%-4u  cmd=%u(%s)  arg=%u..%u\n",
                   task.task_id, cmd, cmd_name(cmd), task.argument, task.result);
        else if (cmd == CMD_MATRIX_PARALLEL)
            printf("[submit] queued  task_id=%-4u  cmd=%u(%s)  row_start=%u  matrix_size=%u\n",
                   task.task_id, cmd, cmd_name(cmd), task.argument, task.result);
        else if (cmd == CMD_FFMPEG_TIME_RANGE)
            printf("[submit] queued  task_id=%-4u  cmd=%u(%s)  start=%us duration=%us\n",
                   task.task_id, cmd, cmd_name(cmd), task.argument, task.result);
        else
            printf("[submit] queued  task_id=%-4u  cmd=%u(%s)  arg=%u\n",
                   task.task_id, cmd, cmd_name(cmd), task.argument);
    }

    printf("[submit] all tasks submitted\n");
    close(fd);
    return 0;
}

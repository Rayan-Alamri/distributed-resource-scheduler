#ifndef MODELS_H
#define MODELS_H

#include <stdint.h>
#include <time.h>

/* Message types identify how the master should treat each fixed-size frame. */
typedef enum {
    MSG_HEARTBEAT = 0,
    MSG_TASK      = 1,
    MSG_RESULT    = 2,
    MSG_REGISTER  = 3,
    MSG_SUBMIT    = 4   /* identifies a submit client — never added to worker registry */
} MessageType;

typedef enum {
    WORKER_IDLE    = 0,
    WORKER_BUSY    = 1,
    WORKER_OFFLINE = 2
} WorkerStatus;

/* Workload codes are shared by submit clients, the master scheduler, and workers. */
typedef enum {
    CMD_PRIME          = 1,  /* argument = upper bound; result = prime count */
    CMD_MATRIX         = 2,  /* argument = matrix size; result = checksum */
    CMD_PRIME_RANGE    = 3,  /* argument = range_start; result(task) = range_end */
    CMD_MONTE_CARLO    = 4,  /* argument = sample count; result = inside-circle count */
    CMD_MANDELBROT     = 5,  /* argument = row_start (100-row chunks); result = checksum */
    CMD_FFMPEG_SEGMENT = 6,  /* argument = segment_id; result = FFMPEG_RESULT_* */
    CMD_FFMPEG_TIME_RANGE = 7,  /* argument = start_second; result(task) = duration_seconds */
    CMD_MATRIX_PARALLEL = 8  /* argument = row_start; result(task) = matrix size */
} CommandCode;

/* Result codes for FFmpeg tasks (CMD_FFMPEG_SEGMENT / CMD_FFMPEG_TIME_RANGE) */
#define FFMPEG_RESULT_FAILURE   0u
#define FFMPEG_RESULT_SUCCESS   1u
#define FFMPEG_RESULT_TIMEOUT   2u
#define FFMPEG_RESULT_MISSING   3u
#define FFMPEG_RESULT_CMD_ERROR 4u

/*
 * Fixed-size network payload — all fields in network byte order on the wire.
 *
 * When used as MSG_TASK, the `result` field carries an optional second
 * argument.  Currently used by CMD_PRIME_RANGE where result = range_end,
 * CMD_FFMPEG_TIME_RANGE where result = duration_seconds, and
 * CMD_MATRIX_PARALLEL where result = matrix size.
 */
typedef struct {
    uint32_t type;          /* MessageType */
    uint32_t worker_id;
    uint32_t task_id;
    uint32_t command_code;  /* CommandCode */
    uint32_t argument;      /* primary workload argument */
    uint32_t result;        /* computed result (MSG_RESULT) or extra arg (MSG_TASK) */
} NetworkPayload;

#define MAX_WORKERS 32

#endif /* MODELS_H */

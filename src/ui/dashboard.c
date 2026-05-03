#include "dashboard.h"

#include "../master/status.h"

#include <dirent.h>
#include <locale.h>
#include <ncurses.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define REFRESH_MS 250
#define MIN_ROWS 20
#define MIN_COLS 78
#define SUBMIT_FIELD_COUNT 6
#define VIDEO_FIELD_COUNT 3
#define MAX_VIDEO_FILES 32
#define VIDEO_NAME_LEN 256

enum {
    COLOR_BORDER = 1,
    COLOR_TITLE,
    COLOR_IDLE,
    COLOR_BUSY,
    COLOR_OFFLINE,
    COLOR_WARN,
    COLOR_ACCENT,
    COLOR_MUTED
};

static MasterState *dashboard_master;
static int dashboard_running;
static int dashboard_started;
static SCREEN *dashboard_screen;
static FILE *dashboard_tty_in;
static FILE *dashboard_tty_out;

typedef enum {
    DASHBOARD_MODE_MONITOR,
    DASHBOARD_MODE_SUBMIT,
    DASHBOARD_MODE_VIDEO
} DashboardMode;

typedef enum {
    SUBMIT_FIELD_COMMAND,
    SUBMIT_FIELD_COUNT_VALUE,
    SUBMIT_FIELD_ARGUMENT,
    SUBMIT_FIELD_STEP,
    SUBMIT_FIELD_RANGE_END,
    SUBMIT_FIELD_START_ID
} SubmitField;

typedef struct {
    uint32_t command_code;
    uint32_t count;
    uint32_t argument;
    uint32_t step;
    uint32_t range_end;
    uint32_t start_id;
    int field;
    int replace_on_digit;
    char message[96];
} SubmitForm;

typedef enum {
    VIDEO_FIELD_FILE,
    VIDEO_FIELD_SEGMENT_SECONDS,
    VIDEO_FIELD_START_ID
} VideoField;

typedef struct {
    char files[MAX_VIDEO_FILES][VIDEO_NAME_LEN];
    int file_count;
    int selected;
    int field;
    int replace_on_digit;
    uint32_t segment_seconds;
    uint32_t start_id;
    char message[256];
} VideoForm;

typedef struct {
    int active;
    uint32_t job_id;
    uint32_t expected_segments;
    char input_name[VIDEO_NAME_LEN];
    char status[512];
} VideoPipeline;

static DashboardMode dashboard_mode = DASHBOARD_MODE_MONITOR;
static SubmitForm submit_form;
static VideoForm video_form;
static VideoPipeline video_pipeline;
static uint32_t dashboard_next_task_id = 1000;

static int pair_attr(short pair) {
    return has_colors() ? COLOR_PAIR(pair) : 0;
}

static void init_colors(void) {
    if (!has_colors())
        return;

    start_color();
    use_default_colors();
    init_pair(COLOR_BORDER, COLOR_CYAN, -1);
    init_pair(COLOR_TITLE, COLOR_WHITE, COLOR_BLUE);
    init_pair(COLOR_IDLE, COLOR_GREEN, -1);
    init_pair(COLOR_BUSY, COLOR_YELLOW, -1);
    init_pair(COLOR_OFFLINE, COLOR_RED, -1);
    init_pair(COLOR_WARN, COLOR_MAGENTA, -1);
    init_pair(COLOR_ACCENT, COLOR_CYAN, -1);
    init_pair(COLOR_MUTED, COLOR_BLACK, -1);
}

static void format_time_value(time_t value, char *buf, size_t len) {
    struct tm tm_value;

    if (localtime_r(&value, &tm_value) == NULL) {
        snprintf(buf, len, "--:--:--");
        return;
    }

    strftime(buf, len, "%H:%M:%S", &tm_value);
}

static void format_duration(uint64_t ms, char *buf, size_t len) {
    if (ms < 1000ULL) {
        snprintf(buf, len, "%llums", (unsigned long long)ms);
    } else if (ms < 60000ULL) {
        snprintf(buf, len, "%llus", (unsigned long long)(ms / 1000ULL));
    } else if (ms < 3600000ULL) {
        unsigned long long total_seconds = ms / 1000ULL;
        snprintf(buf, len, "%llum%02llus",
                 total_seconds / 60ULL, total_seconds % 60ULL);
    } else {
        unsigned long long total_minutes = ms / 60000ULL;
        snprintf(buf, len, "%lluh%02llum",
                 total_minutes / 60ULL, total_minutes % 60ULL);
    }
}

static short status_color(WorkerStatus status) {
    switch (status) {
    case WORKER_IDLE:    return COLOR_IDLE;
    case WORKER_BUSY:    return COLOR_BUSY;
    case WORKER_OFFLINE: return COLOR_OFFLINE;
    default:             return COLOR_WARN;
    }
}

static void draw_title(WINDOW *win, const char *title) {
    wattron(win, pair_attr(COLOR_BORDER));
    box(win, 0, 0);
    wattroff(win, pair_attr(COLOR_BORDER));

    wattron(win, A_BOLD | pair_attr(COLOR_TITLE));
    mvwprintw(win, 0, 2, " %s ", title);
    wattroff(win, A_BOLD | pair_attr(COLOR_TITLE));
}

static uint32_t percent_done(uint32_t completed, uint32_t expected) {
    if (expected == 0)
        return 0;
    if (completed >= expected)
        return 100;
    return (uint32_t)((completed * 100ULL) / expected);
}

static void draw_meter(WINDOW *win, int y, int x, int width,
                       uint32_t completed, uint32_t expected) {
    int filled;

    if (width <= 0)
        return;

    filled = expected > 0
        ? (int)((completed * (uint64_t)width) / expected)
        : 0;
    if (filled > width)
        filled = width;

    for (int i = 0; i < width; i++) {
        if (i < filled) {
            wattron(win, A_BOLD | pair_attr(COLOR_ACCENT));
            mvwaddch(win, y, x + i, '#');
            wattroff(win, A_BOLD | pair_attr(COLOR_ACCENT));
        } else {
            wattron(win, pair_attr(COLOR_MUTED));
            mvwaddch(win, y, x + i, '.');
            wattroff(win, pair_attr(COLOR_MUTED));
        }
    }
}

static void count_workers(const MasterSnapshot *snapshot,
                          int *idle, int *busy, int *offline) {
    *idle = 0;
    *busy = 0;
    *offline = 0;

    for (int i = 0; i < snapshot->worker_count; i++) {
        switch (snapshot->workers[i].status) {
        case WORKER_IDLE:    (*idle)++; break;
        case WORKER_BUSY:    (*busy)++; break;
        case WORKER_OFFLINE: (*offline)++; break;
        default:             break;
        }
    }
}

static const char *job_state_name(const JobSnapshot *job) {
    if (job->active)
        return "running";
    if (job->expected > 0)
        return "complete";
    return "idle";
}

static int has_video_extension(const char *name) {
    const char *dot = strrchr(name, '.');

    if (!dot)
        return 0;

    return strcasecmp(dot, ".mp4") == 0 ||
        strcasecmp(dot, ".mov") == 0 ||
        strcasecmp(dot, ".mkv") == 0 ||
        strcasecmp(dot, ".avi") == 0 ||
        strcasecmp(dot, ".webm") == 0;
}

static void sort_video_files(void) {
    for (int i = 0; i < video_form.file_count; i++) {
        for (int j = i + 1; j < video_form.file_count; j++) {
            if (strcasecmp(video_form.files[i], video_form.files[j]) > 0) {
                char tmp[VIDEO_NAME_LEN];

                memcpy(tmp, video_form.files[i], sizeof(tmp));
                memcpy(video_form.files[i], video_form.files[j],
                       sizeof(video_form.files[i]));
                memcpy(video_form.files[j], tmp, sizeof(video_form.files[j]));
            }
        }
    }
}

static void refresh_video_files(void) {
    char input_dir[512];
    DIR *dp;
    struct dirent *entry;

    video_form.file_count = 0;
    video_form.selected = 0;
    snprintf(input_dir, sizeof(input_dir), "%s/input", master_video_dir());

    dp = opendir(input_dir);
    if (!dp) {
        snprintf(video_form.message, sizeof(video_form.message),
                 "No input directory: %.200s", input_dir);
        return;
    }

    while ((entry = readdir(dp)) != NULL &&
           video_form.file_count < MAX_VIDEO_FILES) {
        if (entry->d_name[0] == '.' || !has_video_extension(entry->d_name))
            continue;

        snprintf(video_form.files[video_form.file_count],
                 sizeof(video_form.files[video_form.file_count]),
                 "%s", entry->d_name);
        video_form.file_count++;
    }
    closedir(dp);

    sort_video_files();
    if (video_form.file_count == 0) {
        snprintf(video_form.message, sizeof(video_form.message),
                 "Put a video in %s/input.", master_video_dir());
    } else {
        snprintf(video_form.message, sizeof(video_form.message),
                 "Choose a video to split, process, and merge.");
    }
}

static void open_video_form(void) {
    memset(&video_form, 0, sizeof(video_form));
    video_form.segment_seconds = 10;
    video_form.start_id = dashboard_next_task_id;
    video_form.field = VIDEO_FIELD_FILE;
    video_form.replace_on_digit = 1;
    refresh_video_files();
    dashboard_mode = DASHBOARD_MODE_VIDEO;
}

static void video_form_move(int delta) {
    video_form.field += delta;
    if (video_form.field < 0)
        video_form.field = VIDEO_FIELD_COUNT - 1;
    if (video_form.field >= VIDEO_FIELD_COUNT)
        video_form.field = 0;
    video_form.replace_on_digit = 1;
}

static void adjust_video_value(int direction) {
    if (video_form.field == VIDEO_FIELD_FILE) {
        if (video_form.file_count == 0)
            return;
        video_form.selected += direction;
        if (video_form.selected < 0)
            video_form.selected = video_form.file_count - 1;
        if (video_form.selected >= video_form.file_count)
            video_form.selected = 0;
        return;
    }

    uint32_t *value = video_form.field == VIDEO_FIELD_SEGMENT_SECONDS
        ? &video_form.segment_seconds
        : &video_form.start_id;

    if (direction > 0) {
        if (*value < 0xFFFFFFFFu)
            (*value)++;
    } else {
        *value = *value > 1 ? *value - 1 : 1;
    }
    video_form.replace_on_digit = 1;
}

static void set_video_digit(int digit) {
    uint32_t *value;

    if (video_form.field == VIDEO_FIELD_FILE)
        return;

    value = video_form.field == VIDEO_FIELD_SEGMENT_SECONDS
        ? &video_form.segment_seconds
        : &video_form.start_id;

    if (video_form.replace_on_digit) {
        *value = (uint32_t)digit;
        video_form.replace_on_digit = 0;
    } else if (*value <= (0xFFFFFFFFu - (uint32_t)digit) / 10u) {
        *value = *value * 10u + (uint32_t)digit;
    }

    if (video_form.field == VIDEO_FIELD_SEGMENT_SECONDS && *value == 0)
        *value = 1;
}

static void backspace_video_value(void) {
    uint32_t *value;

    if (video_form.field == VIDEO_FIELD_FILE)
        return;

    value = video_form.field == VIDEO_FIELD_SEGMENT_SECONDS
        ? &video_form.segment_seconds
        : &video_form.start_id;
    *value /= 10u;
    if (video_form.field == VIDEO_FIELD_SEGMENT_SECONDS && *value == 0)
        *value = 1;
    video_form.replace_on_digit = 0;
}

static void submit_video_form(void) {
    MasterSnapshot snapshot;
    uint32_t segment_count = 0;
    int status;

    if (video_form.file_count == 0) {
        refresh_video_files();
        return;
    }

    master_get_snapshot(dashboard_master, &snapshot);
    if (snapshot.job.active) {
        snprintf(video_form.message, sizeof(video_form.message),
                 "A job is already running.");
        return;
    }

    snprintf(video_form.message, sizeof(video_form.message),
             "Splitting %.200s...", video_form.files[video_form.selected]);
    status = master_process_video(dashboard_master,
                                  video_form.files[video_form.selected],
                                  video_form.segment_seconds,
                                  video_form.start_id,
                                  &segment_count,
                                  video_form.message,
                                  sizeof(video_form.message));
    if (status != 0)
        return;

    master_get_snapshot(dashboard_master, &snapshot);
    memset(&video_pipeline, 0, sizeof(video_pipeline));
    video_pipeline.active = 1;
    video_pipeline.job_id = snapshot.job.job_id;
    video_pipeline.expected_segments = segment_count;
    snprintf(video_pipeline.input_name, sizeof(video_pipeline.input_name),
             "%s", video_form.files[video_form.selected]);
    snprintf(video_pipeline.status, sizeof(video_pipeline.status),
             "Processing %s: %u segment task(s).",
             video_pipeline.input_name, segment_count);

    dashboard_next_task_id = video_form.start_id + segment_count;
    dashboard_mode = DASHBOARD_MODE_MONITOR;
}

static void update_video_pipeline(const MasterSnapshot *snapshot) {
    char output_path[1024];
    char message[256];

    if (!video_pipeline.active)
        return;

    if (snapshot->job.job_id != video_pipeline.job_id || snapshot->job.active)
        return;

    if (snapshot->job.completed < video_pipeline.expected_segments)
        return;

    video_pipeline.active = 0;
    if (snapshot->job.failed > 0) {
        snprintf(video_pipeline.status, sizeof(video_pipeline.status),
                 "Video failed: %u segment(s) failed.", snapshot->job.failed);
        return;
    }

    if (master_merge_video(video_pipeline.input_name,
                           output_path,
                           sizeof(output_path),
                           message,
                           sizeof(message)) == 0) {
        snprintf(video_pipeline.status, sizeof(video_pipeline.status),
                 "Video complete: %.480s", output_path);
    } else {
        snprintf(video_pipeline.status, sizeof(video_pipeline.status),
                 "Merge failed: %s", message);
    }
}

static uint32_t *selected_numeric_value(void) {
    switch (submit_form.field) {
    case SUBMIT_FIELD_COUNT_VALUE: return &submit_form.count;
    case SUBMIT_FIELD_ARGUMENT:    return &submit_form.argument;
    case SUBMIT_FIELD_STEP:        return &submit_form.step;
    case SUBMIT_FIELD_RANGE_END:   return &submit_form.range_end;
    case SUBMIT_FIELD_START_ID:    return &submit_form.start_id;
    default:                       return NULL;
    }
}

static void apply_command_defaults(uint32_t command_code) {
    submit_form.command_code = command_code;
    submit_form.range_end = 0;

    switch ((CommandCode)command_code) {
    case CMD_PRIME:
        submit_form.count = 6;
        submit_form.argument = 5000000u;
        submit_form.step = 0;
        break;
    case CMD_MATRIX:
        submit_form.count = 10;
        submit_form.argument = 500u;
        submit_form.step = 0;
        break;
    case CMD_PRIME_RANGE:
        submit_form.count = 10;
        submit_form.argument = 1u;
        submit_form.step = 10000000u;
        break;
    case CMD_MONTE_CARLO:
        submit_form.count = 4;
        submit_form.argument = 10000000u;
        submit_form.step = 0;
        break;
    case CMD_MANDELBROT:
        submit_form.count = 6;
        submit_form.argument = 0;
        submit_form.step = 100u;
        break;
    case CMD_FFMPEG_SEGMENT:
        submit_form.count = 12;
        submit_form.argument = 0;
        submit_form.step = 1;
        break;
    case CMD_FFMPEG_SCRIPT:
        submit_form.count = 4;
        submit_form.argument = 0;
        submit_form.step = 1;
        break;
    default:
        break;
    }
}

static void open_submit_form(void) {
    memset(&submit_form, 0, sizeof(submit_form));
    apply_command_defaults(CMD_PRIME);
    submit_form.start_id = dashboard_next_task_id;
    submit_form.field = SUBMIT_FIELD_COMMAND;
    submit_form.replace_on_digit = 1;
    snprintf(submit_form.message, sizeof(submit_form.message),
             "Create a task batch.");
    dashboard_mode = DASHBOARD_MODE_SUBMIT;
}

static void submit_form_move(int delta) {
    submit_form.field += delta;
    if (submit_form.field < 0)
        submit_form.field = SUBMIT_FIELD_COUNT - 1;
    if (submit_form.field >= SUBMIT_FIELD_COUNT)
        submit_form.field = 0;
    submit_form.replace_on_digit = 1;
}

static uint32_t numeric_delta(void) {
    switch (submit_form.field) {
    case SUBMIT_FIELD_ARGUMENT:
        return submit_form.command_code == CMD_PRIME_RANGE ? 1000u : 1u;
    case SUBMIT_FIELD_STEP:
        return 1u;
    case SUBMIT_FIELD_RANGE_END:
        return 1000u;
    case SUBMIT_FIELD_START_ID:
        return 1u;
    default:
        return 1u;
    }
}

static void adjust_selected_value(int direction) {
    if (submit_form.field == SUBMIT_FIELD_COMMAND) {
        uint32_t command_code = submit_form.command_code;

        if (direction > 0)
            command_code = command_code >= CMD_FFMPEG_SCRIPT ? CMD_PRIME : command_code + 1;
        else
            command_code = command_code <= CMD_PRIME ? CMD_FFMPEG_SCRIPT : command_code - 1;
        apply_command_defaults(command_code);
        submit_form.replace_on_digit = 1;
        return;
    }

    uint32_t *value = selected_numeric_value();
    uint32_t delta = numeric_delta();

    if (!value)
        return;

    if (direction > 0) {
        if (*value <= 0xFFFFFFFFu - delta)
            *value += delta;
    } else {
        *value = *value > delta ? *value - delta : 0;
    }

    submit_form.replace_on_digit = 1;
}

static void set_selected_digit(int digit) {
    if (submit_form.field == SUBMIT_FIELD_COMMAND) {
        if (digit >= CMD_PRIME && digit <= CMD_FFMPEG_SCRIPT)
            apply_command_defaults((uint32_t)digit);
        return;
    }

    uint32_t *value = selected_numeric_value();
    if (!value)
        return;

    if (submit_form.replace_on_digit) {
        *value = (uint32_t)digit;
        submit_form.replace_on_digit = 0;
    } else if (*value <= (0xFFFFFFFFu - (uint32_t)digit) / 10u) {
        *value = *value * 10u + (uint32_t)digit;
    }
}

static void backspace_selected_value(void) {
    uint32_t *value = selected_numeric_value();

    if (!value)
        return;

    *value /= 10u;
    submit_form.replace_on_digit = 0;
}

static int validate_submit_form(void) {
    if (submit_form.command_code < CMD_PRIME ||
        submit_form.command_code > CMD_FFMPEG_SCRIPT) {
        snprintf(submit_form.message, sizeof(submit_form.message),
                 "Command must be 1 through 7.");
        return -1;
    }

    if (submit_form.count == 0 || submit_form.count > MAX_JOB_TASKS) {
        snprintf(submit_form.message, sizeof(submit_form.message),
                 "Task count must be 1 through %d.", MAX_JOB_TASKS);
        return -1;
    }

    if (submit_form.command_code == CMD_PRIME_RANGE &&
        submit_form.step == 0 &&
        submit_form.range_end < submit_form.argument) {
        snprintf(submit_form.message, sizeof(submit_form.message),
                 "Prime range needs step or range end >= argument.");
        return -1;
    }

    return 0;
}

static void submit_current_form(void) {
    MasterSnapshot snapshot;
    int submitted;

    if (validate_submit_form() != 0)
        return;

    master_get_snapshot(dashboard_master, &snapshot);
    if (snapshot.job.active) {
        snprintf(submit_form.message, sizeof(submit_form.message),
                 "A job is already running.");
        return;
    }

    submitted = master_submit_tasks(dashboard_master,
                                    submit_form.command_code,
                                    submit_form.count,
                                    submit_form.argument,
                                    submit_form.step,
                                    submit_form.range_end,
                                    submit_form.start_id);
    if (submitted <= 0) {
        snprintf(submit_form.message, sizeof(submit_form.message),
                 "Task submission failed.");
        return;
    }

    dashboard_next_task_id = submit_form.start_id + (uint32_t)submitted;
    dashboard_mode = DASHBOARD_MODE_MONITOR;
}

static void render_overview(const MasterSnapshot *snapshot, int rows, int cols) {
    int idle;
    int busy;
    int offline;
    int meter_width;
    char generated_at[16];
    WINDOW *win = newwin(5, cols, 3, 0);

    if (!win)
        return;

    count_workers(snapshot, &idle, &busy, &offline);
    format_time_value(snapshot->generated_at, generated_at, sizeof(generated_at));

    draw_title(win, "Overview");
    mvwprintw(win, 1, 2,
              "Queue: %-5d Workers: %-2d Idle: %-2d Busy: %-2d Offline: %-2d Snapshot: %s",
              snapshot->queue_depth, snapshot->worker_count,
              idle, busy, offline, generated_at);
    mvwprintw(win, 2, 2,
              "Job: %-8s #%u %-16s Expected: %-5u Completed: %-5u Failed: %-5u",
              job_state_name(&snapshot->job), snapshot->job.job_id,
              command_name(snapshot->job.command_code), snapshot->job.expected,
              snapshot->job.completed, snapshot->job.failed);

    meter_width = cols - 28;
    if (meter_width > 44)
        meter_width = 44;
    if (meter_width < 10)
        meter_width = 10;

    mvwprintw(win, 3, 2, "Progress:");
    draw_meter(win, 3, 12, meter_width,
               snapshot->job.completed, snapshot->job.expected);
    mvwprintw(win, 3, 14 + meter_width, "%3u%%",
              percent_done(snapshot->job.completed, snapshot->job.expected));

    wnoutrefresh(win);
    delwin(win);
    (void)rows;
}

static void render_workers(const MasterSnapshot *snapshot,
                           int y, int x, int height, int width) {
    WINDOW *win;
    int max_rows;
    int shown;

    if (height < 5 || width < 40)
        return;

    win = newwin(height, width, y, x);
    if (!win)
        return;

    draw_title(win, "Workers");
    wattron(win, A_BOLD);
    mvwprintw(win, 2, 2,
              "ID   STATE     TASK     COMMAND           ARG        DONE   AVG      RUN      HB");
    wattroff(win, A_BOLD);

    if (snapshot->worker_count == 0) {
        mvwprintw(win, 4, 2, "No workers registered.");
        wnoutrefresh(win);
        delwin(win);
        return;
    }

    max_rows = height - 4;
    shown = snapshot->worker_count < max_rows ? snapshot->worker_count : max_rows;

    for (int i = 0; i < shown; i++) {
        const WorkerSnapshot *worker = &snapshot->workers[i];
        char avg[16];
        char run[16];
        char heartbeat[16];
        int row = i + 3;

        format_duration(worker->avg_runtime_ms, avg, sizeof(avg));
        format_duration(worker->current_runtime_ms, run, sizeof(run));
        snprintf(heartbeat, sizeof(heartbeat), "%lus",
                 worker->last_heartbeat_age_sec);

        mvwprintw(win, row, 2, "%-4u", worker->worker_id);
        wattron(win, A_BOLD | pair_attr(status_color(worker->status)));
        mvwprintw(win, row, 7, "%-9s", worker_status_name(worker->status));
        wattroff(win, A_BOLD | pair_attr(status_color(worker->status)));
        mvwprintw(win, row, 17, "%-8u %-17.17s %-10u %-6u %-8s %-8s %-6s",
                  worker->current_task_id,
                  command_name(worker->current_command_code),
                  worker->current_argument,
                  worker->tasks_completed,
                  avg,
                  worker->status == WORKER_BUSY ? run : "-",
                  heartbeat);
    }

    if (snapshot->worker_count > shown)
        mvwprintw(win, height - 2, 2, "... %d more worker(s)",
                  snapshot->worker_count - shown);

    wnoutrefresh(win);
    delwin(win);
}

static void render_job(const MasterSnapshot *snapshot,
                       int y, int x, int height, int width) {
    WINDOW *win;
    char result_line[64];
    int meter_width;

    if (height < 7 || width < 32)
        return;

    win = newwin(height, width, y, x);
    if (!win)
        return;

    draw_title(win, "Job");
    mvwprintw(win, 2, 2, "State:     %-10s", job_state_name(&snapshot->job));
    mvwprintw(win, 3, 2, "Command:   %-18.18s", command_name(snapshot->job.command_code));
    mvwprintw(win, 4, 2, "Tasks:     %u / %u", snapshot->job.completed, snapshot->job.expected);
    mvwprintw(win, 5, 2, "Failed:    %u", snapshot->job.failed);

    meter_width = width - 16;
    if (meter_width > 28)
        meter_width = 28;
    if (meter_width >= 8) {
        mvwprintw(win, 6, 2, "Progress:");
        draw_meter(win, 6, 12, meter_width,
                   snapshot->job.completed, snapshot->job.expected);
    }

    snprintf(result_line, sizeof(result_line), "%llu",
             (unsigned long long)snapshot->job.sum_results);
    mvwprintw(win, height - 3, 2, "Result sum: %.32s", result_line);
    snprintf(result_line, sizeof(result_line), "%llu",
             (unsigned long long)snapshot->job.sum_arguments);
    mvwprintw(win, height - 2, 2, "Arg sum:    %.32s", result_line);

    wnoutrefresh(win);
    delwin(win);
}

static void add_activity_line(char lines[][96], int *count, int limit,
                              const char *text) {
    if (*count >= limit)
        return;

    snprintf(lines[*count], 96, "%s", text);
    (*count)++;
}

static void render_activity(const MasterSnapshot *snapshot,
                            int y, int x, int height, int width) {
    WINDOW *win;
    char lines[16][96];
    int count = 0;
    int limit = (int)(sizeof(lines) / sizeof(lines[0]));
    char line[96];

    if (height < 5 || width < 32)
        return;

    win = newwin(height, width, y, x);
    if (!win)
        return;

    draw_title(win, "Activity");

    if (snapshot->queue_depth > 0) {
        snprintf(line, sizeof(line), "%d task(s) waiting in queue.",
                 snapshot->queue_depth);
        add_activity_line(lines, &count, limit, line);
    }

    if (snapshot->job.active) {
        snprintf(line, sizeof(line), "Job %u running: %u/%u task(s) complete.",
                 snapshot->job.job_id, snapshot->job.completed,
                 snapshot->job.expected);
        add_activity_line(lines, &count, limit, line);
    } else if (snapshot->job.expected > 0) {
        snprintf(line, sizeof(line), "Job %u complete: %u done, %u failed.",
                 snapshot->job.job_id, snapshot->job.completed,
                 snapshot->job.failed);
        add_activity_line(lines, &count, limit, line);
    }

    if (video_pipeline.status[0] != '\0')
        add_activity_line(lines, &count, limit, video_pipeline.status);

    for (int i = 0; i < snapshot->worker_count && count < limit; i++) {
        const WorkerSnapshot *worker = &snapshot->workers[i];
        char run[16];

        if (worker->status == WORKER_BUSY) {
            format_duration(worker->current_runtime_ms, run, sizeof(run));
            snprintf(line, sizeof(line), "Worker %u running task %u (%s) for %s.",
                     worker->worker_id, worker->current_task_id,
                     command_name(worker->current_command_code), run);
            add_activity_line(lines, &count, limit, line);
        } else if (worker->status == WORKER_OFFLINE) {
            snprintf(line, sizeof(line), "Worker %u offline; last heartbeat %lus ago.",
                     worker->worker_id, worker->last_heartbeat_age_sec);
            add_activity_line(lines, &count, limit, line);
        }
    }

    if (count == 0)
        add_activity_line(lines, &count, limit, "System idle.");

    for (int i = 0; i < count && i < height - 3; i++)
        mvwprintw(win, i + 2, 2, "%.*s", width - 4, lines[i]);

    wnoutrefresh(win);
    delwin(win);
}

static void render_footer(int rows, int cols) {
    const char *text;

    if (dashboard_mode == DASHBOARD_MODE_SUBMIT) {
        text = "Enter: submit  Esc: cancel  Up/Down: field  Left/Right: adjust  digits: edit";
    } else if (dashboard_mode == DASHBOARD_MODE_VIDEO) {
        text = "Enter: process video  Esc: cancel  r: refresh  Up/Down: field  Left/Right: adjust";
    } else {
        text = "n: new task batch  v: process whole video  q: quit";
    }

    attron(A_REVERSE);
    mvhline(rows - 1, 0, ' ', cols);
    mvprintw(rows - 1, 1, "%.*s", cols - 2, text);
    attroff(A_REVERSE);
}

static void render_submit_field(WINDOW *win, int row, int selected,
                                const char *label, const char *value) {
    if (selected)
        wattron(win, A_REVERSE);

    mvwprintw(win, row, 3, "%-12s %s", label, value);

    if (selected)
        wattroff(win, A_REVERSE);
}

static void render_submit_form(int rows, int cols) {
    int width = cols - 6;
    int height = 15;
    int y;
    int x;
    char value[64];
    WINDOW *win;

    if (width > 76)
        width = 76;
    if (width < 60)
        width = cols - 2;

    y = (rows - height) / 2;
    x = (cols - width) / 2;
    if (y < 1)
        y = 1;
    if (x < 1)
        x = 1;

    win = newwin(height, width, y, x);
    if (!win)
        return;

    draw_title(win, "Submit Tasks");

    snprintf(value, sizeof(value), "%u  %s",
             submit_form.command_code, command_name(submit_form.command_code));
    render_submit_field(win, 2, submit_form.field == SUBMIT_FIELD_COMMAND,
                        "Command", value);

    snprintf(value, sizeof(value), "%u", submit_form.count);
    render_submit_field(win, 3, submit_form.field == SUBMIT_FIELD_COUNT_VALUE,
                        "Task count", value);

    snprintf(value, sizeof(value), "%u", submit_form.argument);
    render_submit_field(win, 4, submit_form.field == SUBMIT_FIELD_ARGUMENT,
                        "Argument", value);

    snprintf(value, sizeof(value), "%u", submit_form.step);
    render_submit_field(win, 5, submit_form.field == SUBMIT_FIELD_STEP,
                        "Step", value);

    snprintf(value, sizeof(value), "%u", submit_form.range_end);
    render_submit_field(win, 6, submit_form.field == SUBMIT_FIELD_RANGE_END,
                        "Range end", value);

    snprintf(value, sizeof(value), "%u", submit_form.start_id);
    render_submit_field(win, 7, submit_form.field == SUBMIT_FIELD_START_ID,
                        "Start ID", value);

    wattron(win, pair_attr(COLOR_MUTED));
    mvwhline(win, 9, 2, ACS_HLINE, width - 4);
    wattroff(win, pair_attr(COLOR_MUTED));

    mvwprintw(win, 10, 3, "Batch: %u task(s), IDs %u-%u",
              submit_form.count,
              submit_form.start_id,
              submit_form.count > 0
                  ? submit_form.start_id + submit_form.count - 1
                  : submit_form.start_id);

    if (submit_form.command_code == CMD_PRIME_RANGE) {
        uint32_t end_value = submit_form.step > 0
            ? submit_form.argument + submit_form.step - 1
            : submit_form.range_end;
        mvwprintw(win, 11, 3, "First range: %u..%u",
                  submit_form.argument, end_value);
    } else {
        mvwprintw(win, 11, 3, "First argument: %u", submit_form.argument);
    }

    wattron(win, pair_attr(COLOR_ACCENT));
    mvwprintw(win, height - 2, 3, "%.*s", width - 6, submit_form.message);
    wattroff(win, pair_attr(COLOR_ACCENT));

    wnoutrefresh(win);
    delwin(win);
}

static void render_video_form(int rows, int cols) {
    int width = cols - 6;
    int height = 16;
    int y;
    int x;
    WINDOW *win;
    char value[VIDEO_NAME_LEN];

    if (width > 78)
        width = 78;
    if (width < 60)
        width = cols - 2;

    y = (rows - height) / 2;
    x = (cols - width) / 2;
    if (y < 1)
        y = 1;
    if (x < 1)
        x = 1;

    win = newwin(height, width, y, x);
    if (!win)
        return;

    draw_title(win, "Process Video");

    if (video_form.file_count == 0) {
        mvwprintw(win, 2, 3, "No videos found in %s/input.", master_video_dir());
        mvwprintw(win, 4, 3, "Add .mp4, .mov, .mkv, .avi, or .webm files there.");
    } else {
        snprintf(value, sizeof(value), "%s",
                 video_form.files[video_form.selected]);
        render_submit_field(win, 2, video_form.field == VIDEO_FIELD_FILE,
                            "Video", value);

        snprintf(value, sizeof(value), "%u", video_form.segment_seconds);
        render_submit_field(win, 3,
                            video_form.field == VIDEO_FIELD_SEGMENT_SECONDS,
                            "Segment sec", value);

        snprintf(value, sizeof(value), "%u", video_form.start_id);
        render_submit_field(win, 4, video_form.field == VIDEO_FIELD_START_ID,
                            "Start ID", value);

        wattron(win, pair_attr(COLOR_MUTED));
        mvwhline(win, 6, 2, ACS_HLINE, width - 4);
        wattroff(win, pair_attr(COLOR_MUTED));

        mvwprintw(win, 7, 3, "This will split the video, queue cmd=6 for all segments,");
        mvwprintw(win, 8, 3, "then merge processed segments after the job completes.");
        mvwprintw(win, 10, 3, "Videos: %d found. Selected %d of %d.",
                  video_form.file_count,
                  video_form.selected + 1,
                  video_form.file_count);
    }

    wattron(win, pair_attr(COLOR_ACCENT));
    mvwprintw(win, height - 2, 3, "%.*s", width - 6, video_form.message);
    wattroff(win, pair_attr(COLOR_ACCENT));

    wnoutrefresh(win);
    delwin(win);
}

static void render_dashboard(void) {
    MasterSnapshot snapshot;
    int rows;
    int cols;
    int content_top = 8;
    int content_height;
    char generated_at[16];

    if (!dashboard_master)
        return;

    master_get_snapshot(dashboard_master, &snapshot);
    update_video_pipeline(&snapshot);
    master_get_snapshot(dashboard_master, &snapshot);
    getmaxyx(stdscr, rows, cols);
    erase();

    if (rows < MIN_ROWS || cols < MIN_COLS) {
        mvprintw(0, 0, "Terminal too small. Need at least %dx%d.",
                 MIN_COLS, MIN_ROWS);
        doupdate();
        return;
    }

    format_time_value(snapshot.generated_at, generated_at, sizeof(generated_at));

    attron(A_BOLD);
    mvprintw(0, 2, "Distributed Resource Scheduler");
    attroff(A_BOLD);
    attron(pair_attr(COLOR_MUTED));
    mvprintw(1, 2, "Live master status, generated at %s", generated_at);
    attroff(pair_attr(COLOR_MUTED));
    mvhline(2, 0, ACS_HLINE, cols);

    render_overview(&snapshot, rows, cols);

    content_height = rows - content_top - 1;
    if (cols >= 118 && content_height >= 12) {
        int left_width = (cols * 66) / 100;
        int right_width = cols - left_width;
        int job_height = content_height > 18 ? 10 : 8;

        render_workers(&snapshot, content_top, 0, content_height, left_width);
        render_job(&snapshot, content_top, left_width, job_height, right_width);
        render_activity(&snapshot, content_top + job_height, left_width,
                        content_height - job_height, right_width);
    } else {
        int job_height = content_height >= 15 ? 8 : 0;
        int worker_height = content_height - job_height;

        render_workers(&snapshot, content_top, 0, worker_height, cols);
        if (job_height > 0)
            render_job(&snapshot, content_top + worker_height, 0,
                       job_height, cols);
    }

    if (dashboard_mode == DASHBOARD_MODE_SUBMIT)
        render_submit_form(rows, cols);
    if (dashboard_mode == DASHBOARD_MODE_VIDEO)
        render_video_form(rows, cols);

    render_footer(rows, cols);
    doupdate();
}

static void handle_key(int ch) {
    if (ch == ERR)
        return;

    if (dashboard_mode == DASHBOARD_MODE_VIDEO) {
        if (ch == 27 || ch == 'q' || ch == 'Q') {
            dashboard_mode = DASHBOARD_MODE_MONITOR;
            return;
        }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            submit_video_form();
            return;
        }
        if (ch == 'r' || ch == 'R') {
            refresh_video_files();
            return;
        }
        if (ch == '\t' || ch == KEY_DOWN) {
            video_form_move(1);
            return;
        }
        if (ch == KEY_BTAB || ch == KEY_UP) {
            video_form_move(-1);
            return;
        }
        if (ch == KEY_LEFT || ch == '-') {
            adjust_video_value(-1);
            return;
        }
        if (ch == KEY_RIGHT || ch == '+' || ch == '=') {
            adjust_video_value(1);
            return;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            backspace_video_value();
            return;
        }
        if (ch >= '0' && ch <= '9')
            set_video_digit(ch - '0');
        return;
    }

    if (dashboard_mode == DASHBOARD_MODE_SUBMIT) {
        if (ch == 27 || ch == 'q' || ch == 'Q') {
            dashboard_mode = DASHBOARD_MODE_MONITOR;
            return;
        }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            submit_current_form();
            return;
        }
        if (ch == '\t' || ch == KEY_DOWN) {
            submit_form_move(1);
            return;
        }
        if (ch == KEY_BTAB || ch == KEY_UP) {
            submit_form_move(-1);
            return;
        }
        if (ch == KEY_LEFT || ch == '-') {
            adjust_selected_value(-1);
            return;
        }
        if (ch == KEY_RIGHT || ch == '+' || ch == '=') {
            adjust_selected_value(1);
            return;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            backspace_selected_value();
            return;
        }
        if (ch >= '0' && ch <= '9')
            set_selected_digit(ch - '0');
        return;
    }

    if (ch == 'n' || ch == 'N')
        open_submit_form();
    else if (ch == 'v' || ch == 'V')
        open_video_form();
    else if (ch == 'q' || ch == 'Q')
        dashboard_running = 0;
}

int dashboard_init(MasterState *master) {
    if (!master)
        return -1;

    setlocale(LC_ALL, "");
    dashboard_tty_in = fopen("/dev/tty", "r");
    dashboard_tty_out = fopen("/dev/tty", "w");
    if (!dashboard_tty_in || !dashboard_tty_out) {
        dashboard_stop();
        return -1;
    }

    dashboard_screen = newterm(NULL, dashboard_tty_out, dashboard_tty_in);
    if (!dashboard_screen) {
        dashboard_stop();
        return -1;
    }

    set_term(dashboard_screen);
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    init_colors();

    dashboard_master = master;
    dashboard_running = 1;
    dashboard_started = 1;
    return 0;
}

void dashboard_run(void) {
    int ch;

    while (dashboard_running) {
        ch = getch();
        handle_key(ch);
        render_dashboard();
        napms(REFRESH_MS);
    }
}

void dashboard_stop(void) {
    dashboard_running = 0;

    if (dashboard_started && dashboard_screen) {
        set_term(dashboard_screen);
        endwin();
        delscreen(dashboard_screen);
    }

    dashboard_started = 0;
    dashboard_screen = NULL;
    dashboard_master = NULL;

    if (dashboard_tty_in) {
        fclose(dashboard_tty_in);
        dashboard_tty_in = NULL;
    }
    if (dashboard_tty_out) {
        fclose(dashboard_tty_out);
        dashboard_tty_out = NULL;
    }
}

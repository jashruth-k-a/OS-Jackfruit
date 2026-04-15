#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <pthread.h>
#include "monitor_ioctl.h"

/* ══════════════════════════════════════════════════════
   CONSTANTS
══════════════════════════════════════════════════════ */
#define SOCK_PATH        "/tmp/mini_runtime.sock"
#define MAX_CONTAINERS   32
#define STACK_SIZE       (1 << 20)
#define LOG_BUF_CAPACITY 256
#define ID_LEN           64
#define LOG_LINE_MAX     1024

#define DEFAULT_SOFT_LIMIT_BYTES (40UL << 20)   /* 40 MiB */
#define DEFAULT_HARD_LIMIT_BYTES (64UL << 20)   /* 64 MiB */

/* ══════════════════════════════════════════════════════
   WIRE PROTOCOL
══════════════════════════════════════════════════════ */
typedef enum {
    CMD_START = 1,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP,
} cmd_kind_t;

typedef struct {
    cmd_kind_t    kind;
    char          id[ID_LEN];
    char          rootfs[4096];
    char          command[256];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int           nice_value;
} __attribute__((packed)) ctrl_request_t;

typedef struct {
    int  status;
    char message[1024];
} __attribute__((packed)) ctrl_response_t;

/* ══════════════════════════════════════════════════════
   BOUNDED BUFFER
══════════════════════════════════════════════════════ */
typedef struct {
    char line[LOG_LINE_MAX];
    char container_id[ID_LEN];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUF_CAPACITY];
    size_t          head, tail, count;
    int             shutdown;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

static void bb_init(bounded_buffer_t *bb)
{
    memset(bb, 0, sizeof(*bb));
    pthread_mutex_init(&bb->mutex, NULL);
    pthread_cond_init(&bb->not_empty, NULL);
    pthread_cond_init(&bb->not_full, NULL);
}

static int bb_push(bounded_buffer_t *bb, const log_item_t *item)
{
    pthread_mutex_lock(&bb->mutex);
    while (bb->count == LOG_BUF_CAPACITY && !bb->shutdown)
        pthread_cond_wait(&bb->not_full, &bb->mutex);
    if (bb->shutdown && bb->count == LOG_BUF_CAPACITY) {
        pthread_mutex_unlock(&bb->mutex);
        return -1;
    }
    bb->items[bb->tail] = *item;
    bb->tail = (bb->tail + 1) % LOG_BUF_CAPACITY;
    bb->count++;
    pthread_cond_signal(&bb->not_empty);
    pthread_mutex_unlock(&bb->mutex);
    return 0;
}

static int bb_pop(bounded_buffer_t *bb, log_item_t *item)
{
    pthread_mutex_lock(&bb->mutex);
    while (bb->count == 0) {
        if (bb->shutdown) {
            pthread_mutex_unlock(&bb->mutex);
            return -1;
        }
        pthread_cond_wait(&bb->not_empty, &bb->mutex);
    }
    *item = bb->items[bb->head];
    bb->head = (bb->head + 1) % LOG_BUF_CAPACITY;
    bb->count--;
    pthread_cond_signal(&bb->not_full);
    pthread_mutex_unlock(&bb->mutex);
    return 0;
}

static void bb_shutdown(bounded_buffer_t *bb)
{
    pthread_mutex_lock(&bb->mutex);
    bb->shutdown = 1;
    pthread_cond_broadcast(&bb->not_empty);
    pthread_cond_broadcast(&bb->not_full);
    pthread_mutex_unlock(&bb->mutex);
}

/* ══════════════════════════════════════════════════════
   CONTAINER RECORD
══════════════════════════════════════════════════════ */
typedef enum {
    STATE_RUNNING = 1,
    STATE_EXITED,
    STATE_KILLED,
} cont_state_t;

typedef struct {
    char         id[ID_LEN];
    char         rootfs[4096];
    char         command[256];
    pid_t        host_pid;
    cont_state_t state;
    int          exit_code;
    int          exit_signal;
    int          stop_requested;
    int          log_fd;
    int          pipe_read_fd;
    pthread_t    reader_tid;
    time_t       start_time;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int          run_waiters[8];
    int          run_waiter_count;
} container_t;

/* ══════════════════════════════════════════════════════
   GLOBAL STATE
══════════════════════════════════════════════════════ */
static struct {
    container_t      slots[MAX_CONTAINERS];
    int              count;
    int              server_fd;
    int              monitor_fd;
    int              shutdown;
    bounded_buffer_t bb;
    pthread_t        logger_tid;
} G;

static volatile sig_atomic_t g_got_sigchld = 0;

/* ══════════════════════════════════════════════════════
   LOGGER THREAD
══════════════════════════════════════════════════════ */
static void *logger_thread_fn(void *arg)
{
    (void)arg;
    log_item_t item;
    while (bb_pop(&G.bb, &item) == 0) {
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (G.slots[i].host_pid > 0 &&
                strcmp(G.slots[i].id, item.container_id) == 0) {
                if (G.slots[i].log_fd >= 0) {
                    ssize_t r = write(G.slots[i].log_fd,
                                      item.line,
                                      strlen(item.line));
                    (void)r;
                }
                break;
            }
        }
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════
   READER THREAD
══════════════════════════════════════════════════════ */
typedef struct {
    int  pipe_fd;
    char id[ID_LEN];
} reader_arg_t;

static void *reader_thread_fn(void *arg)
{
    reader_arg_t *ra = arg;
    char          buf[LOG_LINE_MAX];
    ssize_t       n;

    while ((n = read(ra->pipe_fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, ra->id, ID_LEN - 1);
        strncpy(item.line,         buf,    LOG_LINE_MAX - 1);
        bb_push(&G.bb, &item);
    }

    close(ra->pipe_fd);
    free(ra);
    return NULL;
}

/* ══════════════════════════════════════════════════════
   CHILD FUNCTION
══════════════════════════════════════════════════════ */
typedef struct {
    int  pipe_write_fd;
    char rootfs[4096];
    char command[256];
    int  nice_value;
} child_cfg_t;

static int child_fn(void *arg)
{
    child_cfg_t *cfg = arg;

    /* redirect stdout + stderr into pipe */
    if (dup2(cfg->pipe_write_fd, STDOUT_FILENO) < 0) _exit(1);
    if (dup2(cfg->pipe_write_fd, STDERR_FILENO) < 0) _exit(1);
    close(cfg->pipe_write_fd);

    /* chroot FIRST */
    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        _exit(1);
    }

    /* chdir("/") immediately after chroot — mandatory.
       Without this, relative paths can escape the chroot jail. */
    if (chdir("/") < 0) {
        perror("chdir");
        _exit(1);
    }

    /* mount /proc INSIDE new root, AFTER chroot */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("mount /proc");

    /* CFS scheduling priority */
    if (cfg->nice_value != 0) {
        if (setpriority(PRIO_PROCESS, 0, cfg->nice_value) < 0)
            perror("setpriority");
    }

    execl("/bin/sh", "sh", "-c", cfg->command, NULL);
    perror("execl");
    _exit(127);
}

/* ══════════════════════════════════════════════════════
   HELPERS
══════════════════════════════════════════════════════ */
static container_t *find_container(const char *id)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (G.slots[i].host_pid != 0 &&
            strcmp(G.slots[i].id, id) == 0)
            return &G.slots[i];
    return NULL;
}

static container_t *alloc_slot(void)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (G.slots[i].host_pid == 0)
            return &G.slots[i];
    return NULL;
}

/* ══════════════════════════════════════════════════════
   REAP CHILDREN
══════════════════════════════════════════════════════ */
static void reap_children(void)
{
    int   status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            container_t *c = &G.slots[i];
            if (c->host_pid != pid) continue;

            if (WIFEXITED(status)) {
                c->exit_code = WEXITSTATUS(status);
                c->state     = STATE_EXITED;
            } else if (WIFSIGNALED(status)) {
                c->exit_signal = WTERMSIG(status);
                c->state = (c->stop_requested || c->exit_signal != SIGKILL)
                           ? STATE_EXITED
                           : STATE_KILLED;
            }

            /* unregister from kernel monitor */
            if (G.monitor_fd >= 0) {
                struct monitor_unregister_req req;
                memset(&req, 0, sizeof(req));
                req.pid = pid;
                strncpy(req.container_id, c->id, MONITOR_ID_LEN - 1);
                ioctl(G.monitor_fd, MONITOR_UNREGISTER, &req);
            }

            /* notify CMD_RUN waiters */
            ctrl_response_t resp;
            memset(&resp, 0, sizeof(resp));
            resp.status = c->exit_code;
            snprintf(resp.message, sizeof(resp.message),
                     "container '%s' exited (state=%s exit_signal=%d)",
                     c->id,
                     (c->state == STATE_KILLED) ? "hard_limit_killed" :
                     (c->state == STATE_EXITED) ? "stopped" : "unknown",
                     c->exit_signal);

            for (int w = 0; w < c->run_waiter_count; w++) {
                ssize_t r = write(c->run_waiters[w], &resp, sizeof(resp));
                (void)r;
                close(c->run_waiters[w]);
            }
            c->run_waiter_count = 0;

            if (c->log_fd >= 0) {
                close(c->log_fd);
                c->log_fd = -1;
            }

            fprintf(stderr, "[engine] container '%s' pid=%d reaped "
                    "(state=%s)\n",
                    c->id, pid,
                    (c->state == STATE_KILLED) ? "hard_limit_killed" :
                    (c->state == STATE_EXITED) ? "stopped" : "unknown");
            break;
        }
    }
}

/* ══════════════════════════════════════════════════════
   COMMAND: START / RUN
══════════════════════════════════════════════════════ */
static void handle_cmd_start(const ctrl_request_t *req,
                              ctrl_response_t      *resp,
                              int                   client_fd,
                              int                   is_run)
{
    if (find_container(req->id)) {
        resp->status = -EEXIST;
        snprintf(resp->message, sizeof(resp->message),
                 "container '%s' already exists", req->id);
        return;
    }

    container_t *c = alloc_slot();
    if (!c) {
        resp->status = -ENOMEM;
        snprintf(resp->message, sizeof(resp->message), "no free slots");
        return;
    }

    /* apply default memory limits if not specified */
    unsigned long soft = req->soft_limit_bytes ?
                         req->soft_limit_bytes : DEFAULT_SOFT_LIMIT_BYTES;
    unsigned long hard = req->hard_limit_bytes ?
                         req->hard_limit_bytes : DEFAULT_HARD_LIMIT_BYTES;

    /* open log file */
    mkdir("logs", 0755);
    char logpath[256];
    snprintf(logpath, sizeof(logpath), "logs/%s.log", req->id);
    int log_fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        resp->status = -errno;
        snprintf(resp->message, sizeof(resp->message),
                 "open log: %s", strerror(errno));
        return;
    }

    /* create pipe */
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) {
        resp->status = -errno;
        snprintf(resp->message, sizeof(resp->message),
                 "pipe2: %s", strerror(errno));
        close(log_fd);
        return;
    }

    /* allocate clone stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        resp->status = -ENOMEM;
        snprintf(resp->message, sizeof(resp->message), "malloc stack failed");
        close(pipefd[0]);
        close(pipefd[1]);
        close(log_fd);
        return;
    }

    child_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pipe_write_fd = pipefd[1];
    strncpy(cfg.rootfs,  req->rootfs,  sizeof(cfg.rootfs)  - 1);
    strncpy(cfg.command, req->command, sizeof(cfg.command) - 1);
    cfg.nice_value = req->nice_value;

    pid_t pid = clone(child_fn,
                      stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      &cfg);
    free(stack);

    if (pid < 0) {
        resp->status = -errno;
        snprintf(resp->message, sizeof(resp->message),
                 "clone: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        close(log_fd);
        return;
    }

    /* CRITICAL: close write end in parent so EOF works */
    close(pipefd[1]);

    /* register with kernel monitor */
    if (G.monitor_fd >= 0) {
        struct monitor_register_req mreg;
        memset(&mreg, 0, sizeof(mreg));
        mreg.pid              = pid;
        mreg.soft_limit_bytes = soft;
        mreg.hard_limit_bytes = hard;
        strncpy(mreg.container_id, req->id, MONITOR_ID_LEN - 1);
        ioctl(G.monitor_fd, MONITOR_REGISTER, &mreg);
    }

    /* fill slot */
    memset(c, 0, sizeof(*c));
    strncpy(c->id,      req->id,      ID_LEN - 1);
    strncpy(c->rootfs,  req->rootfs,  sizeof(c->rootfs)  - 1);
    strncpy(c->command, req->command, sizeof(c->command) - 1);
    c->host_pid         = pid;
    c->state            = STATE_RUNNING;
    c->log_fd           = log_fd;
    c->pipe_read_fd     = pipefd[0];
    c->stop_requested   = 0;
    c->start_time       = time(NULL);
    c->soft_limit_bytes = soft;
    c->hard_limit_bytes = hard;

    /* start reader thread — detached so shutdown never blocks on it */
    reader_arg_t *ra = malloc(sizeof(*ra));
    if (ra) {
        ra->pipe_fd = pipefd[0];
        strncpy(ra->id, req->id, ID_LEN - 1);
        pthread_t tid;
        pthread_create(&tid, NULL, reader_thread_fn, ra);
        pthread_detach(tid);
        c->reader_tid = tid;
    } else {
        close(pipefd[0]);
    }

    if (is_run) {
        if (c->run_waiter_count < 8)
            c->run_waiters[c->run_waiter_count++] = client_fd;
        resp->status = 1; /* sentinel: defer response */
        return;
    }

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "started container '%s' pid=%d soft=%luMiB hard=%luMiB",
             req->id, pid, soft >> 20, hard >> 20);
}

/* ══════════════════════════════════════════════════════
   COMMAND: PS
══════════════════════════════════════════════════════ */
static void handle_cmd_ps(ctrl_response_t *resp)
{
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    int found = 0;

    for (int i = 0; i < MAX_CONTAINERS; i++) {
        container_t *c = &G.slots[i];
        if (c->host_pid == 0) continue;
        found = 1;

        const char *st =
            (c->state == STATE_RUNNING) ? "running"          :
            (c->state == STATE_KILLED)  ? "hard_limit_killed" :
                                          "stopped";

        /* format start time */
        char timebuf[32] = "unknown";
        if (c->start_time) {
            struct tm *tm_info = localtime(&c->start_time);
            strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);
        }

        char line[256];
        snprintf(line, sizeof(line),
                 "  %-16s  pid=%-6d  %-18s  started=%s  "
                 "soft=%luMiB  hard=%luMiB\n",
                 c->id, c->host_pid, st, timebuf,
                 c->soft_limit_bytes >> 20,
                 c->hard_limit_bytes >> 20);
        strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
    }

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "%s", found ? buf : "(no containers)\n");
}

/* ══════════════════════════════════════════════════════
   COMMAND: LOGS
══════════════════════════════════════════════════════ */
static void handle_cmd_logs(const ctrl_request_t *req,
                             ctrl_response_t      *resp)
{
    char path[256];
    snprintf(path, sizeof(path), "logs/%s.log", req->id);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        resp->status = -errno;
        snprintf(resp->message, sizeof(resp->message),
                 "cannot open log '%s': %s", path, strerror(errno));
        return;
    }

    ssize_t n = read(fd, resp->message, sizeof(resp->message) - 1);
    if (n < 0) n = 0;
    resp->message[n] = '\0';
    resp->status = 0;
    close(fd);
}

/* ══════════════════════════════════════════════════════
   COMMAND: STOP
══════════════════════════════════════════════════════ */
static void handle_cmd_stop(const ctrl_request_t *req,
                             ctrl_response_t      *resp)
{
    container_t *c = find_container(req->id);
    if (!c) {
        resp->status = -ENOENT;
        snprintf(resp->message, sizeof(resp->message),
                 "container '%s' not found", req->id);
        return;
    }
    if (c->state != STATE_RUNNING) {
        resp->status = -EINVAL;
        snprintf(resp->message, sizeof(resp->message),
                 "container '%s' is not running", req->id);
        return;
    }

    /* set flag BEFORE signal — race condition prevention */
    c->stop_requested = 1;
    kill(c->host_pid, SIGTERM);

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "sent SIGTERM to container '%s' (pid=%d)",
             req->id, c->host_pid);
}

/* ══════════════════════════════════════════════════════
   CLIENT DISPATCH
══════════════════════════════════════════════════════ */
static void handle_client(int client_fd)
{
    ctrl_request_t  req;
    ctrl_response_t resp;
    memset(&resp, 0, sizeof(resp));

    ssize_t n = read(client_fd, &req, sizeof(req));
    if (n != (ssize_t)sizeof(req)) {
        close(client_fd);
        return;
    }

    int deferred = 0;

    switch (req.kind) {
    case CMD_START:
        handle_cmd_start(&req, &resp, client_fd, 0);
        break;
    case CMD_RUN:
        handle_cmd_start(&req, &resp, client_fd, 1);
        if (resp.status == 1) deferred = 1;
        break;
    case CMD_PS:
        handle_cmd_ps(&resp);
        break;
    case CMD_LOGS:
        handle_cmd_logs(&req, &resp);
        break;
    case CMD_STOP:
        handle_cmd_stop(&req, &resp);
        break;
    default:
        resp.status = -ENOSYS;
        snprintf(resp.message, sizeof(resp.message),
                 "unknown command %d", req.kind);
    }

    if (!deferred) {
        ssize_t r = write(client_fd, &resp, sizeof(resp));
        (void)r;
        close(client_fd);
    }
}

/* ══════════════════════════════════════════════════════
   SIGNAL HANDLERS
══════════════════════════════════════════════════════ */
static void sigchld_handler(int sig) { (void)sig; g_got_sigchld = 1; }
static void sigterm_handler(int sig) { (void)sig; G.shutdown    = 1; }

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    signal(SIGPIPE, SIG_IGN);
}

/* ══════════════════════════════════════════════════════
   SUPERVISOR MAIN
══════════════════════════════════════════════════════ */
static int run_supervisor(const char *default_rootfs)
{
    (void)default_rootfs;

    setup_signals();
    bb_init(&G.bb);
    memset(G.slots, 0, sizeof(G.slots));
    G.shutdown = 0;

    G.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (G.monitor_fd < 0)
        fprintf(stderr,
                "[engine] /dev/container_monitor not available "
                "(memory limits disabled)\n");

    G.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (G.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    unlink(SOCK_PATH);
    if (bind(G.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(G.server_fd, 16) < 0) {
        perror("listen"); return 1;
    }

    pthread_create(&G.logger_tid, NULL, logger_thread_fn, NULL);

    fprintf(stderr, "[engine] supervisor ready  socket=%s\n", SOCK_PATH);

    /* ── EVENT LOOP ── */
    while (!G.shutdown) {
        fd_set rfds;
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        FD_ZERO(&rfds);
        FD_SET(G.server_fd, &rfds);

        int ready = select(G.server_fd + 1, &rfds, NULL, NULL, &tv);

        if (g_got_sigchld) {
            g_got_sigchld = 0;
            reap_children();
        }

        if (ready < 0) {
            if (errno == EINTR)  continue;
            if (errno == ECHILD) continue;  /* FIX: no children yet */
            perror("select");
            break;
        }

        if (ready > 0 && FD_ISSET(G.server_fd, &rfds)) {
            int cfd = accept(G.server_fd, NULL, NULL);
            if (cfd >= 0)
                handle_client(cfd);
        }
    }

    /* ── GRACEFUL SHUTDOWN ── */
    fprintf(stderr, "[engine] shutting down...\n");

    /* Step 1: SIGTERM all running containers */
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        container_t *c = &G.slots[i];
        if (c->host_pid && c->state == STATE_RUNNING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
    }

    /* Step 2: wait 3 seconds */
    sleep(3);

    /* Step 3: force kill survivors */
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        container_t *c = &G.slots[i];
        if (c->host_pid && c->state == STATE_RUNNING) {
            fprintf(stderr, "[engine] force-killing '%s'\n", c->id);
            kill(c->host_pid, SIGKILL);
        }
    }

    /* Step 4: reap zombies */
    sleep(1);
    reap_children();

    /* Step 5: close log fds */
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (G.slots[i].log_fd >= 0) {
            close(G.slots[i].log_fd);
            G.slots[i].log_fd = -1;
        }
    }

    /* Step 6: drain + join logger */
    bb_shutdown(&G.bb);
    pthread_join(G.logger_tid, NULL);

    /* Step 7: cleanup */
    if (G.monitor_fd >= 0) close(G.monitor_fd);
    close(G.server_fd);
    unlink(SOCK_PATH);

    fprintf(stderr, "[engine] done.\n");
    return 0;
}

/* ══════════════════════════════════════════════════════
   CLI CLIENT
══════════════════════════════════════════════════════ */
static int cli_send(const ctrl_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "cannot connect to supervisor at %s\n"
                "is 'sudo ./engine supervisor' running?\n",
                SOCK_PATH);
        close(fd);
        return 1;
    }

    ssize_t r = write(fd, req, sizeof(*req));
    if (r != (ssize_t)sizeof(*req)) {
        perror("write");
        close(fd);
        return 1;
    }

    ctrl_response_t resp;
    memset(&resp, 0, sizeof(resp));
    ssize_t n = read(fd, &resp, sizeof(resp));

    if (n > 0)
        printf("%s\n", resp.message);
    else
        fprintf(stderr, "no response from supervisor\n");

    close(fd);
    return (n > 0 && resp.status == 0) ? 0 : 1;
}

/* ══════════════════════════════════════════════════════
   MAIN
══════════════════════════════════════════════════════ */
static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <rootfs>\n"
        "  %s start  <id> <rootfs> <cmd> "
            "[--nice N] [--soft-mib N] [--hard-mib N]\n"
        "  %s run    <id> <rootfs> <cmd> "
            "[--nice N] [--soft-mib N] [--hard-mib N]\n"
        "  %s ps\n"
        "  %s logs  <id>\n"
        "  %s stop  <id>\n",
        prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
    if (argc < 2) { print_usage(argv[0]); return 1; }

    const char *subcmd = argv[1];

    if (strcmp(subcmd, "supervisor") == 0) {
        const char *rootfs = (argc >= 3) ? argv[2] : ".";
        return run_supervisor(rootfs);
    }

    ctrl_request_t req;
    memset(&req, 0, sizeof(req));

    if (strcmp(subcmd, "ps") == 0) {
        req.kind = CMD_PS;
        return cli_send(&req);
    }

    if (strcmp(subcmd, "logs") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        req.kind = CMD_LOGS;
        strncpy(req.id, argv[2], ID_LEN - 1);
        return cli_send(&req);
    }

    if (strcmp(subcmd, "stop") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        req.kind = CMD_STOP;
        strncpy(req.id, argv[2], ID_LEN - 1);
        return cli_send(&req);
    }

    if (strcmp(subcmd, "start") == 0 || strcmp(subcmd, "run") == 0) {
        if (argc < 5) { print_usage(argv[0]); return 1; }
        req.kind = (strcmp(subcmd, "run") == 0) ? CMD_RUN : CMD_START;
        strncpy(req.id,      argv[2], ID_LEN - 1);
        strncpy(req.rootfs,  argv[3], sizeof(req.rootfs)  - 1);
        strncpy(req.command, argv[4], sizeof(req.command) - 1);

        for (int i = 5; i < argc; i++) {
            if (strcmp(argv[i], "--nice") == 0 && i + 1 < argc)
                req.nice_value = atoi(argv[++i]);
            else if (strcmp(argv[i], "--soft-mib") == 0 && i + 1 < argc)
                req.soft_limit_bytes = (unsigned long)atol(argv[++i]) << 20;
            else if (strcmp(argv[i], "--hard-mib") == 0 && i + 1 < argc)
                req.hard_limit_bytes = (unsigned long)atol(argv[++i]) << 20;
            else {
                fprintf(stderr, "unknown option: %s\n", argv[i]);
                return 1;
            }
        }
        return cli_send(&req);
    }

    fprintf(stderr, "unknown subcommand: %s\n", subcmd);
    print_usage(argv[0]);
    return 1;
}


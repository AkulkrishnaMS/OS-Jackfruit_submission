#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <sys/mount.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <sys/resource.h>

#include "monitor_ioctl.h"

#define SOCKET_PATH     "/tmp/jackfruit.sock"
#define MONITOR_DEV     "/dev/container_monitor"
#define STACK_SIZE      (1024 * 1024)
#define MAX_CONTAINERS  10
#define BUFFER_SIZE     20
#define LOG_MSG_MAX     512

/* ══════════════════════════════════════════════════════════════
 *  Data structures
 * ══════════════════════════════════════════════════════════════ */

typedef enum {
    STATE_STARTING,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED
} ContainerState;

struct Container {
    char            name[32];
    pid_t           host_pid;
    ContainerState  state;
    bool            stop_requested;
    char            exit_reason[64];
    time_t          started_at;
    unsigned long   soft_limit_kb;   /* KB */
    unsigned long   hard_limit_kb;   /* KB */
};

struct Container container_registry[MAX_CONTAINERS];
int container_count = 0;

/* ── Bounded log buffer ───────────────────────────────────────*/
typedef struct {
    char msg[LOG_MSG_MAX];
    char container_name[32];
} LogEntry;

struct {
    LogEntry        buffer[BUFFER_SIZE];
    int             head;
    int             tail;
    pthread_mutex_t lock;
    sem_t           empty;
    sem_t           full;
} log_queue;

/* ── Child process arguments ──────────────────────────────── */
struct ChildArgs {
    char  rootfs_path[256];
    char *command[16];   /* argv for execv */
    int   pipe_fd;       /* write end — child stdout/stderr */
};

struct ProducerArgs {
    int  pipe_fd;
    char name[32];
};

/* ══════════════════════════════════════════════════════════════
 *  TASK 3 — Logging consumer thread
 *  Drains the bounded buffer, writes each entry to a log file.
 * ══════════════════════════════════════════════════════════════ */
void *consumer_thread_func(void *arg)
{
    (void)arg;
    while (1) {
        sem_wait(&log_queue.full);

        pthread_mutex_lock(&log_queue.lock);
        LogEntry entry = log_queue.buffer[log_queue.head];
        log_queue.head = (log_queue.head + 1) % BUFFER_SIZE;
        pthread_mutex_unlock(&log_queue.lock);

        sem_post(&log_queue.empty);

        /* Write to /tmp/jackfruit_<name>.log */
        char path[256];
        snprintf(path, sizeof(path),
                 "/tmp/jackfruit_%s.log", entry.container_name);
        int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, entry.msg, strlen(entry.msg));
            close(fd);
        }
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
 *  TASK 3 — Logging producer thread (one per container)
 *  Reads from the container's pipe, pushes lines into the buffer.
 * ══════════════════════════════════════════════════════════════ */
void *producer_thread_func(void *arg)
{
    struct ProducerArgs *pa = (struct ProducerArgs *)arg;
    FILE  *stream = fdopen(pa->pipe_fd, "r");
    char   line[LOG_MSG_MAX];

    if (!stream) { free(pa); return NULL; }

    while (fgets(line, sizeof(line), stream)) {
        sem_wait(&log_queue.empty);     /* wait for a free slot */

        pthread_mutex_lock(&log_queue.lock);
        strncpy(log_queue.buffer[log_queue.tail].msg,
                line, LOG_MSG_MAX - 1);
        strncpy(log_queue.buffer[log_queue.tail].container_name,
                pa->name, 31);
        log_queue.tail = (log_queue.tail + 1) % BUFFER_SIZE;
        pthread_mutex_unlock(&log_queue.lock);

        sem_post(&log_queue.full);      /* signal consumer */
    }

    fclose(stream);
    free(pa);
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
 *  TASK 1 — Container child function
 *  Runs INSIDE the new namespaces.
 * ══════════════════════════════════════════════════════════════ */
int container_process(void *arg)
{
    struct ChildArgs *args = (struct ChildArgs *)arg;

    /* Redirect stdout/stderr into the logging pipe */
    dup2(args->pipe_fd, STDOUT_FILENO);
    dup2(args->pipe_fd, STDERR_FILENO);
    close(args->pipe_fd);

    /* chroot into the container's root filesystem */
    if (chroot(args->rootfs_path) != 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir /");
        return 1;
    }

    /* Mount /proc so PID namespace tools (ps, top) work */
    mount("proc", "/proc", "proc", 0, NULL);

    /* Execute the command */
    execvp(args->command[0], args->command);

    /* If execvp returns, something went wrong */
    perror("execvp");
    return 127;
}

/* ══════════════════════════════════════════════════════════════
 *  TASK 1 — Launch a container
 *
 *  soft_limit_kb / hard_limit_kb: memory limits in KILOBYTES.
 *  nice_val: scheduler priority (-20 high .. +19 low).
 *  cmd_tokens: NULL-terminated argv for the process inside container.
 * ══════════════════════════════════════════════════════════════ */
void launch_container(const char  *name,
                      const char  *rootfs,
                      char *const  cmd_tokens[],
                      int          nice_val,
                      unsigned long soft_limit_kb,
                      unsigned long hard_limit_kb)
{
    if (container_count >= MAX_CONTAINERS) {
        fprintf(stderr, "[daemon] max containers reached\n");
        return;
    }

    /* Create pipe: container stdout/stderr → supervisor */
    int pipe_fds[2];
    if (pipe(pipe_fds) < 0) { perror("pipe"); return; }

    /* Build child args */
    char *stack = malloc(STACK_SIZE);
    struct ChildArgs *args = malloc(sizeof(struct ChildArgs));
    if (!stack || !args) { perror("malloc"); return; }

    strncpy(args->rootfs_path, rootfs, 255);
    args->pipe_fd = pipe_fds[1];   /* child writes here */

    /* Copy command tokens into args */
    int i = 0;
    while (cmd_tokens && cmd_tokens[i] && i < 14) {
        args->command[i] = cmd_tokens[i];
        i++;
    }
    args->command[i] = NULL;

    /* Fill metadata slot */
    struct Container *c = &container_registry[container_count];
    memset(c, 0, sizeof(*c));
    strncpy(c->name, name, 31);
    c->state         = STATE_RUNNING;
    c->stop_requested = false;
    c->started_at    = time(NULL);
    c->soft_limit_kb = soft_limit_kb;
    c->hard_limit_kb = hard_limit_kb;
    strcpy(c->exit_reason, "RUNNING");

    /* clone() — creates isolated child with new namespaces */
    pid_t pid = clone(container_process,
                      stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      args);

    close(pipe_fds[1]);   /* parent closes write end */

    if (pid < 0) {
        perror("clone");
        close(pipe_fds[0]);
        free(stack);
        free(args);
        return;
    }

    c->host_pid = pid;
    free(stack);   /* kernel copied the stack; we can free our copy */

    /* ── TASK 5 — Set nice value (scheduling priority) ─────── */
    if (nice_val != 0) {
        if (setpriority(PRIO_PROCESS, pid, nice_val) == -1)
            perror("setpriority");
        else
            printf("[daemon] container '%s' nice=%d\n", name, nice_val);
    }

    /* ── TASK 4 — Register with kernel monitor ───────────────
     *
     * We pass limits in KB.  The kernel module compares RSS (also in KB).
     * Typical values:
     *   soft_limit_kb = 20480  (20 MB)  → soft warning
     *   hard_limit_kb = 40960  (40 MB)  → hard kill
     */
    int mon_fd = open(MONITOR_DEV, O_RDWR);
    if (mon_fd >= 0) {
        struct container_limits lim = {
            .pid        = pid,
            .soft_limit = soft_limit_kb,
            .hard_limit = hard_limit_kb,
        };
        if (ioctl(mon_fd, IOCTL_REGISTER_CONTAINER, &lim) < 0)
            perror("ioctl REGISTER");
        else
            printf("[daemon] registered PID %d with monitor "
                   "(soft=%luKB hard=%luKB)\n",
                   pid, soft_limit_kb, hard_limit_kb);
        close(mon_fd);
    } else {
        fprintf(stderr,
                "[daemon] kernel monitor unavailable (%s)\n",
                strerror(errno));
    }

    /* ── TASK 3 — Start log producer thread ──────────────────*/
    struct ProducerArgs *pa = malloc(sizeof(struct ProducerArgs));
    pa->pipe_fd = pipe_fds[0];
    strncpy(pa->name, name, 31);
    pthread_t pt;
    pthread_create(&pt, NULL, producer_thread_func, pa);
    pthread_detach(pt);

    container_count++;
    printf("[daemon] started container '%s' pid=%d\n", name, pid);
}

/* ══════════════════════════════════════════════════════════════
 *  TASK 2 — Stop a container
 * ══════════════════════════════════════════════════════════════ */
void stop_container(const char *name, char *response)
{
    for (int i = 0; i < container_count; i++) {
        if (strcmp(container_registry[i].name, name) == 0 &&
            container_registry[i].state == STATE_RUNNING) {
            container_registry[i].stop_requested = true;
            kill(container_registry[i].host_pid, SIGTERM);
            sprintf(response, "✅ Stopped %s", name);
            return;
        }
    }
    strcpy(response, "❌ Not found or already stopped");
}

/* ══════════════════════════════════════════════════════════════
 *  TASK 2 — SIGCHLD handler
 *  Reaps dead children, updates state.
 *  This prevents zombie processes.
 * ══════════════════════════════════════════════════════════════ */
void sigchld_handler(int signum)
{
    (void)signum;
    int    status;
    pid_t  pid;

    /* Loop — multiple children may have exited at once */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < container_count; i++) {
            if (container_registry[i].host_pid == pid) {
                container_registry[i].state = STATE_STOPPED;

                if (container_registry[i].stop_requested) {
                    strcpy(container_registry[i].exit_reason,
                           "Stopped (Manual)");
                } else if (WIFSIGNALED(status) &&
                           WTERMSIG(status) == SIGKILL) {
                    strcpy(container_registry[i].exit_reason,
                           "Killed (Hard Limit)");
                } else if (WIFSIGNALED(status)) {
                    snprintf(container_registry[i].exit_reason, 64,
                             "Killed (Signal %d)", WTERMSIG(status));
                } else {
                    snprintf(container_registry[i].exit_reason, 64,
                             "Normal Exit (code %d)",
                             WEXITSTATUS(status));
                }
                break;
            }
        }
    }
}

/* ══════════════════════════════════════════════════════════════
 *  TASK 1 + 2 — Daemon / supervisor main loop
 * ══════════════════════════════════════════════════════════════ */
int run_daemon(void)
{
    printf("🍍 Jackfruit Daemon starting...\n");

    /* Install SIGCHLD handler to reap zombies */
    signal(SIGCHLD, sigchld_handler);

    /* Initialise bounded log buffer */
    pthread_mutex_init(&log_queue.lock, NULL);
    sem_init(&log_queue.empty, 0, BUFFER_SIZE);
    sem_init(&log_queue.full,  0, 0);

    /* Start the log consumer thread */
    pthread_t ct;
    pthread_create(&ct, NULL, consumer_thread_func, NULL);
    pthread_detach(ct);

    /* Create UNIX domain socket for CLI commands */
    unlink(SOCKET_PATH);
    int server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    bind(server_sock, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_sock, 5);
    chmod(SOCKET_PATH, 0777);

    printf("🎧 Listening on %s\n", SOCKET_PATH);

    /* ── Main event loop ──────────────────────────────────── */
    while (1) {
        int    client_sock = accept(server_sock, NULL, NULL);
        char   buffer[1024] = {0};
        char   response[4096] = {0};
        char  *saveptr;

        read(client_sock, buffer, sizeof(buffer) - 1);
        char *cmd = strtok_r(buffer, " \n", &saveptr);

        /* ── start <name> <rootfs> <nice> <cmd> [args...] ── */
        if (cmd && strcmp(cmd, "start") == 0) {
            char *name      = strtok_r(NULL, " \n", &saveptr);
            char *rootfs    = strtok_r(NULL, " \n", &saveptr);
            char *nice_str  = strtok_r(NULL, " \n", &saveptr);
            /*
             * Everything after nice is the command + args.
             * We collect them into a token array.
             */
            int   nice_val      = nice_str ? atoi(nice_str) : 0;
            char *cmd_tokens[16];
            int   ntok = 0;
            char *tok;
            while ((tok = strtok_r(NULL, " \n", &saveptr)) && ntok < 15)
                cmd_tokens[ntok++] = tok;
            cmd_tokens[ntok] = NULL;

            if (!name || !rootfs || ntok == 0) {
                strcpy(response, "❌ Usage: start <name> <rootfs> <nice> <cmd> [args]");
            } else {
                /*
                 * Default limits: soft=20 MB, hard=40 MB (in KB).
                 * The CLI can override with --soft-kb / --hard-kb flags later.
                 */
                unsigned long soft_kb = 20480UL;   /* 20 MB */
                unsigned long hard_kb = 40960UL;   /* 40 MB */

                launch_container(name, rootfs,
                                 (char *const *)cmd_tokens,
                                 nice_val, soft_kb, hard_kb);
                snprintf(response, sizeof(response),
                         "✅ Started %s (nice=%d soft=%luKB hard=%luKB)",
                         name, nice_val, soft_kb, hard_kb);
            }

        /* ── ps ─────────────────────────────────────────── */
        } else if (cmd && strcmp(cmd, "ps") == 0) {
            snprintf(response, sizeof(response),
                     "%-20s %-8s %-10s %-12s %-12s %s\n",
                     "NAME", "PID", "STATE",
                     "SOFT(KB)", "HARD(KB)", "REASON");
            for (int i = 0; i < container_count; i++) {
                char line[256];
                snprintf(line, sizeof(line),
                         "%-20s %-8d %-10s %-12lu %-12lu %s\n",
                         container_registry[i].name,
                         container_registry[i].host_pid,
                         container_registry[i].state == STATE_RUNNING
                             ? "RUNNING" : "STOPPED",
                         container_registry[i].soft_limit_kb,
                         container_registry[i].hard_limit_kb,
                         container_registry[i].exit_reason);
                strncat(response, line,
                        sizeof(response) - strlen(response) - 1);
            }

        /* ── stop <name> ─────────────────────────────────── */
        } else if (cmd && strcmp(cmd, "stop") == 0) {
            char *name = strtok_r(NULL, " \n", &saveptr);
            if (name)
                stop_container(name, response);
            else
                strcpy(response, "❌ Usage: stop <name>");

        /* ── logs <name> ─────────────────────────────────── */
        } else if (cmd && strcmp(cmd, "logs") == 0) {
            char *name = strtok_r(NULL, " \n", &saveptr);
            if (!name) {
                strcpy(response, "❌ Usage: logs <name>");
            } else {
                char path[256];
                snprintf(path, sizeof(path),
                         "/tmp/jackfruit_%s.log", name);
                FILE *f = fopen(path, "r");
                if (!f) {
                    snprintf(response, sizeof(response),
                             "❌ No log for '%s'\n", name);
                } else {
                    size_t n = fread(response, 1,
                                     sizeof(response) - 1, f);
                    response[n] = '\0';
                    fclose(f);
                }
            }

        } else {
            strcpy(response, "❌ Unknown command. Use: start, ps, stop, logs");
        }

        send(client_sock, response, strlen(response), 0);
        close(client_sock);
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  main — either daemon or CLI client
 * ══════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "daemon") == 0)
        return run_daemon();

    /* CLI mode — forward all args to daemon over the socket */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "❌ Cannot connect to daemon (%s).\n"
                "   Is it running?  sudo ./engine daemon\n",
                strerror(errno));
        return 1;
    }

    /* Build command string from argv */
    char buffer[1024] = {0};
    for (int i = 1; i < argc; i++) {
        strncat(buffer, argv[i], sizeof(buffer) - strlen(buffer) - 2);
        strncat(buffer, " ",     sizeof(buffer) - strlen(buffer) - 1);
    }

    send(sock, buffer, strlen(buffer), 0);

    /* Print response */
    char resp[4096] = {0};
    read(sock, resp, sizeof(resp) - 1);
    printf("%s\n", resp);

    close(sock);
    return 0;
}

// wyvern_daemon.c
#define _GNU_SOURCE
#include "wyvern_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <dlfcn.h>
#include <sys/syscall.h>

#ifndef MAX_TARGETS
#define MAX_TARGETS 1024
#endif

// ----- Tracer metadata per PID -----

typedef enum {
    TR_NONE  = 0,
    TR_WYVERN = 1,
    TR_LTTNG  = 2,
    TR_PERF   = 3,
    // add more
} tracer_type_t;

typedef enum {
    TS_UNKNOWN = 0,
    TS_IDLE    = 1,
    TS_RUNNING = 2,
    TS_PAUSED  = 3,
    TS_STOPPED = 4,
} tracer_state_t;

typedef struct {
    int in_use;
    int pid;                  // target TGID
    int attached;             // 0/1

    tracer_type_t  type;
    tracer_state_t state;

    uint64_t running_since_ns; // valid when state==RUNNING
    uint64_t last_event_ns;    // last control event time
} TargetSlot;

static TargetSlot g_targets[MAX_TARGETS];

// function prototypes for customization points

static uint64_t now_ns_monotonic(void);
static void die(const char *msg);
static void ensure_sudo(void);
static int run_cmd(const char *cmd);
static int start_perf_record_sched(int duration_sec);
static int wyvern_attach_so_via_ptrace_dlopen(int target_pid, const char *so_path);
static int wyvern_control_tracer(int target_pid, int action);
static int create_lttng_session(void);
static int start_lttng(void);
static int lttng_snapshot(void);
static int stop_lttng(void);
static int lttng_ust_record(void);
static int lttng_ust_stop(void);

// ----- Customization points -----

// Called when we receive an event for a PID that is not attached yet.
// Put your "attach tracer to pid" logic here.
// You can set slot->attached=1 if attach succeeds, and update type/state.
static void on_first_attach(TargetSlot *slot, const WyvernShm *evt)
{
    // Decide tracer type:
    // Priority:
    //  1) slot->type if already set (e.g., config)
    //  2) evt->tracer_type hint
    tracer_type_t t = slot->type;
    if (t == TR_NONE && evt->tracer_type != 0)
        t = (tracer_type_t)evt->tracer_type;

    // Default if still unknown
    if (t == TR_NONE)
        t = TR_WYVERN;

    slot->type = t;

    switch (t) {
    case TR_PERF: {
        // Attach perf for 30 seconds
        // For many perf setups you need privileges; if perf fails, it prints why.
        if (start_perf_record_sched(30) == 0) {
            // slot->attached = 1;
            // slot->state = TS_RUNNING;
            // slot->running_since_ns = now_ns_monotonic();
        } else {
            slot->attached = 0;
            slot->state = TS_UNKNOWN;
        }
        break;
    }

    case TR_LTTNG: {
        slot->attached = 1;
        slot->state = TS_RUNNING;
        break;
    }

    case TR_WYVERN: {
        // Call your future ptrace+dlopen injector
        // TODO: pick this from config / evt->user_data / env var
        const char *so_path = "./sample.so"; // placeholder
        if (wyvern_attach_so_via_ptrace_dlopen(slot->pid, so_path) == 0) {
            slot->attached = 1;
            slot->state = TS_RUNNING;
            slot->running_since_ns = now_ns_monotonic();
        } else {
            slot->attached = 0;
            slot->state = TS_UNKNOWN;
        }
        break;
    }

    default:
        printf("[daemon] TODO attach for tracer type=%d pid=%d\n", (int)t, slot->pid);
        slot->attached = 0;
        slot->state = TS_UNKNOWN;
        break;
    }
}

// Called when tracer is already attached and a new event arrives.
// Put your "control tracer" logic here (start/stop, set mode, dump, etc.)
static void on_control_event(TargetSlot *slot, const WyvernShm *evt) {
    tracer_type_t t = slot->type;
    if (t == TR_NONE && evt->tracer_type != 0)
        t = (tracer_type_t)evt->tracer_type;

    // Default if still unknown
    if (t == TR_NONE)
        t = TR_WYVERN;

    slot->type = t;

    switch (t) {
    case TR_PERF: {
        // Attach perf for 30 seconds
        // For many perf setups you need privileges; if perf fails, it prints why.
        // if (start_perf_record_sched(30) == 0) {
        //     slot->attached = 1;
        //     slot->state = TS_RUNNING;
        //     slot->running_since_ns = now_ns_monotonic();
        // } else {
        //     slot->attached = 0;
        //     slot->state = TS_UNKNOWN;
        // }
        break;
    }

    case TR_LTTNG: {
        lttng_snapshot();
        break;
    }

    case TR_WYVERN: {
        // Control wyvern action 1 for enable / action 2 for disable
        if (evt->action == 1) {
            wyvern_control_tracer(slot->pid, 0);
            slot->state = TS_RUNNING;
            slot->last_event_ns = now_ns_monotonic();
        } else if (evt->action == 2) {
            wyvern_control_tracer(slot->pid, 1);
            slot->state = TS_PAUSED;
            slot->last_event_ns = now_ns_monotonic();
        }
        break;
    }

    default:
        printf("[daemon] TODO attach for tracer type=%d pid=%d\n", (int)t, slot->pid);
        slot->attached = 0;
        slot->state = TS_UNKNOWN;
        break;
    }

}

// ----- End customization points -----

static atomic_int g_got_sig = 0;
static volatile sig_atomic_t stop_requested = 0;

static uint64_t now_ns_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sigusr1_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si; (void)uc;
    atomic_store_explicit(&g_got_sig, 1, memory_order_relaxed);
}

static void sigint_handler(int sig) {
    (void)sig;
    stop_requested = 1;
}

static void die(const char *msg) {
    fprintf(stderr, "wyvern_daemon: %s (errno=%d: %s)\n", msg, errno, strerror(errno));
    exit(1);
}

static TargetSlot *find_slot(int pid) {
    for (int i = 0; i < MAX_TARGETS; i++) {
        if (g_targets[i].in_use && g_targets[i].pid == pid) return &g_targets[i];
    }
    return NULL;
}

static int shm_open_autofix(const char *name, mode_t mode)
{
    // Attempt 1: open/create normally
    int fd = shm_open(name, O_RDWR | O_CREAT, mode);
    if (fd >= 0) {
        // Make permissions explicit (helps when created under restrictive umask)
        (void)fchmod(fd, mode);
        return fd;
    }

    // If permission denied, try to remove stale object and recreate
    if (errno == EACCES) {
        fprintf(stderr, "[daemon] shm_open(%s) EACCES; trying shm_unlink + recreate\n", name);

        // Best effort: remove old object
        if (shm_unlink(name) != 0) {
            fprintf(stderr, "[daemon] shm_unlink(%s) failed: %s\n", name, strerror(errno));
            // continue anyway: we will retry and report the real error
        }

        // Attempt 2: create fresh exclusively
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, mode);
        if (fd >= 0) {
            (void)fchmod(fd, mode);
            return fd;
        }

        // If it already exists (race), fall back to normal open
        if (errno == EEXIST) {
            fd = shm_open(name, O_RDWR, mode);
            if (fd >= 0) {
                (void)fchmod(fd, mode);
                return fd;
            }
        }
    }

    return -1; // caller uses errno
}

static TargetSlot *get_or_create_slot(int pid) {
    TargetSlot *s = find_slot(pid);
    if (s) return s;

    for (int i = 0; i < MAX_TARGETS; i++) {
        if (!g_targets[i].in_use) {
            memset(&g_targets[i], 0, sizeof(g_targets[i]));
            g_targets[i].in_use = 1;
            g_targets[i].pid = pid;
            g_targets[i].attached = 0;
            g_targets[i].type = TR_NONE;
            g_targets[i].state = TS_UNKNOWN;
            g_targets[i].running_since_ns = 0;
            g_targets[i].last_event_ns = 0;
            return &g_targets[i];
        }
    }
    return NULL; // table full
}

static const char *type_str(tracer_type_t t) {
    switch (t) {
        case TR_WYVERN: return "WYVERN";
        case TR_LTTNG:  return "LTTNG";
        case TR_PERF:   return "PERF";
        default:        return "NONE";
    }
}

static const char *state_str(tracer_state_t s) {
    switch (s) {
        case TS_IDLE:    return "IDLE";
        case TS_RUNNING: return "RUNNING";
        case TS_PAUSED:  return "PAUSED";
        case TS_STOPPED: return "STOPPED";
        default:         return "UNKNOWN";
    }
}

// Optional helper: update slot fields based on incoming event hints
static void apply_event_hints(TargetSlot *slot, const WyvernShm *evt) {
    if (evt->tracer_type != 0) slot->type = (tracer_type_t)evt->tracer_type;

    // If sender explicitly sets tracer_state, honor it.
    if (evt->tracer_state != 0) {
        tracer_state_t new_state = (tracer_state_t)evt->tracer_state;

        if (slot->state != new_state) {
            slot->state = new_state;
            if (new_state == TS_RUNNING) {
                slot->running_since_ns = now_ns_monotonic();
            }
        }
    }
}

int main(void) {

    ensure_sudo();

    const char *shm_name = WYVERN_SHM_NAME;

    printf("[daemon] pid=%d shm=%s\n", getpid(), shm_name);
    fflush(stdout);

    // Install signal handlers
    // SIGUSR1
    struct sigaction sa_usr1;
    memset(&sa_usr1, 0, sizeof(sa_usr1));
    sa_usr1.sa_sigaction = sigusr1_handler;
    sa_usr1.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa_usr1.sa_mask);
    if (sigaction(SIGUSR1, &sa_usr1, NULL) != 0)
        die("sigaction(SIGUSR1) failed");

    // SIGINT (Ctrl+C)
    struct sigaction sa_int;
    memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = sigint_handler;   // simpler handler
    sa_int.sa_flags = SA_RESTART;
    sigemptyset(&sa_int.sa_mask);
    if (sigaction(SIGINT, &sa_int, NULL) != 0)
        die("sigaction(SIGINT) failed");

    // Map shm
    // Choose perms. For “root daemon + root sender”, 0600 is fine.
    // If you want non-root senders to read/write the shm, use 0666 (less safe).
    mode_t mode = 0600;
    int fd = shm_open_autofix(shm_name, mode);
    if (fd < 0) die("shm_open failed");
    if (ftruncate(fd, (off_t)sizeof(WyvernShm)) != 0) die("ftruncate failed");

    WyvernShm *shm = (WyvernShm *)mmap(NULL, sizeof(WyvernShm),
                                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) die("mmap failed");
    close(fd);

    // Initialize header if needed
    if (shm->magic != WYVERN_SHM_MAGIC || shm->version != WYVERN_SHM_VERSION || shm->size != sizeof(WyvernShm)) {
        memset(shm, 0, sizeof(*shm));
        shm->magic = WYVERN_SHM_MAGIC;
        shm->version = WYVERN_SHM_VERSION;
        shm->size = (uint32_t)sizeof(WyvernShm);
        shm->seq = 0;
    }

    uint64_t last_seq = 0;

    create_lttng_session();
    start_lttng();
    lttng_ust_record();

    for (;;) {
        pause();

        if (stop_requested) {
            printf("[daemon] SIGINT received, shutting down...\n");
            stop_lttng();
            lttng_ust_stop();
            break;
        }

        if (!atomic_exchange_explicit(&g_got_sig, 0, memory_order_relaxed))
            continue;

        WyvernShm evt = *shm; // snapshot

        if (evt.seq == last_seq) continue;
        last_seq = evt.seq;

        if (evt.magic != WYVERN_SHM_MAGIC || evt.version != WYVERN_SHM_VERSION || evt.size != sizeof(WyvernShm)) {
            printf("[daemon] shm header mismatch; ignoring\n");
            continue;
        }

        int pid = evt.target_pid;
        if (pid <= 0) {
            printf("[daemon] invalid pid; ignoring\n");
            continue;
        }

        TargetSlot *slot = get_or_create_slot(pid);
        if (!slot) {
            printf("[daemon] target table full; ignoring pid=%d\n", pid);
            continue;
        }

        slot->last_event_ns = now_ns_monotonic();
        apply_event_hints(slot, &evt);

        printf("[daemon] event seq=%llu pid=%d tid=%d action=%d attached=%d type=%s state=%s\n",
               (unsigned long long)evt.seq,
               pid, evt.target_tid, evt.action,
               slot->attached, type_str(slot->type), state_str(slot->state));

        if (evt.tracer_type == TR_NONE) {
            if (evt.action == 0) {
                evt.tracer_type = TR_LTTNG;
            }
            else if (evt.action == 1 || evt.action == 2) {
                evt.tracer_type = TR_WYVERN;
            }
        }
        
        if (!slot->attached) {
            // First time attach path (customize here)
            on_first_attach(slot, &evt);

            // After attach, you may immediately control based on the same event
            // (kept here because it's useful in practice)
            if (slot->attached) {
                on_control_event(slot, &evt);
            }
        } else {
            // Already attached → control path (customize here)
            on_control_event(slot, &evt);
        }

        // Example: “time since running” (if RUNNING)
        if (slot->state == TS_RUNNING && slot->running_since_ns != 0) {
            uint64_t dt_ns = now_ns_monotonic() - slot->running_since_ns;
            printf("[daemon] pid=%d running_for=%.3fs\n", pid, (double)dt_ns / 1e9);
        }

        fflush(stdout);
    }

    // unreachable in this POC
    munmap(shm, sizeof(*shm));
    return 0;
}

static void ensure_sudo(void)
{
    if (geteuid() != 0) {
        fprintf(stderr, "[daemon] This program must be run with sudo.\n");
        fprintf(stderr, "Try: sudo %s\n", program_invocation_name);
        exit(EXIT_FAILURE);
    }
}

static int run_cmd(const char *cmd)
{
    pid_t c = fork();
    int status;

    if (c < 0) {
        perror("fork");
        return -1;
    }

    if (c == 0) {
        execlp("sh", "sh", "-c", cmd, (char *)NULL);
        perror("exec(sh)");
        _exit(127);
    }

    if (waitpid(c, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[daemon] command failed: %s\n", cmd);
        return -1;
    }

    return 0;
}

// ----- perf functions -----

static int start_perf_record_sched(int duration_sec)
{
    char out_s[128];
    char duration_s[32];

    // get system time
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    // create filename with timestamp
    strftime(out_s, sizeof(out_s),
             "perf-wyvern-%Y%m%d-%H%M%S.data", &tm);

    snprintf(duration_s, sizeof(duration_s), "%d", duration_sec);

    pid_t c = fork();
    if (c < 0) {
        perror("fork(perf)");
        return -1;
    }

    if (c == 0) {
        execlp("sudo",
               "sudo",
               "perf",
               "record",
               "-e", "sched:*",
               "-e", "irq:*",
               "-a",
               "-g",
               "-m", "8M",
               "-o", out_s,
               "--",
               "sleep", duration_s,
               (char*)NULL);

        perror("exec(perf)");
        _exit(127);
    }

    printf("[daemon] PERF started: perf record -> %s (duration %ds)\n",
           out_s, duration_sec);

    return 0;
}

// ----- end perf functions -----

// ----- wyvern tracer functions -----
// // Helper to find symbols in the target process (like dlopen)
// static void* get_remote_address(int pid, void* local_addr) {
//     // This assumes the library (libc/libdl) is mapped at the same offset relative 
//     // to the base in both processes, which is true for standard ASLR.
//     return local_addr; 
// }

static int wyvern_attach_so_via_ptrace_dlopen(int target_pid, const char *so_path)
{
    // TODO: implement ptrace attach + remote dlopen injection here.
    printf("[daemon] attaching .so via ptrace dlopen to target %d: %s\n", target_pid, so_path);
    return 0;
}

static int wyvern_control_tracer(int target_pid, int action)
{
    if (action == 0) {
        // send signal SIGUSR1+0 to enable fentry probes in the target process
        if (kill(target_pid, SIGUSR1+0) == 0) {
            printf("[daemon] sent signal to target %d: enable probes\n", target_pid);
        } else {
            printf("[daemon] failed to send signal to target %d: %s\n", target_pid, strerror(errno));
        } 
    } else if (action == 1) {
        // send signal SIGUSR1+1 to disable fentry probes in the target process
        if (kill(target_pid, SIGUSR1+1) == 0) {
            printf("[daemon] sent signal to target %d: disable probes\n", target_pid);
        } else {
            printf("[daemon] failed to send signal to target %d: %s\n", target_pid, strerror(errno));
        }
    } else {
        printf("[daemon] (TODO) control tracer for target %d: action=%d\n", target_pid, action);
    }
    return 0;
}

// ----- end wyvern tracer functions -----

// ----- lttng functions -----

static int create_lttng_session(void)
{
    // destroy (ignore errors)
    if (run_cmd("sudo lttng destroy wyvern-session 2>/dev/null || true") < 0) {
        fprintf(stderr, "[daemon] warning: failed to destroy existing session\n");
    }

    // create snapshot session
    if (run_cmd("sudo lttng create wyvern-session --snapshot "
                "--output=/tmp/wyvern-kernel-trace --trace-format=ctf-1.8") < 0) {
        fprintf(stderr, "[daemon] failed to create LTTng session\n");
        return -1;
    }

    // destroy (ignore errors)
    if (run_cmd("sudo lttng destroy wyvern-session-ust 2>/dev/null || true") < 0) {
        fprintf(stderr, "[daemon] warning: failed to destroy existing session\n");
    }

    // create snapshot session
    if (run_cmd("sudo lttng create wyvern-session-ust "
                "--output=/tmp/wyvern-ust-trace --trace-format=ctf-1.8") < 0) {
        fprintf(stderr, "[daemon] failed to create LTTng session\n");
        return -1;
    }

    printf("[daemon] LTTng sessions created: wyvern-session wyvern-session-ust\n");
    return 0;
}

static int start_lttng(void)
{
    if (run_cmd("sudo lttng enable-channel --session=wyvern-session --kernel kchan --num-subbuf=4 --subbuf-size=4M") < 0)
        return -1;

    if (run_cmd("sudo lttng enable-event --session=wyvern-session --kernel --channel=kchan --tracepoint 'sched_*'") < 0)
        return -1;

    if (run_cmd("sudo lttng enable-event --session=wyvern-session --kernel --channel=kchan --tracepoint 'irq_*'") < 0)
        return -1;

    if (run_cmd("sudo lttng add-context --session=wyvern-session --kernel --channel=kchan --type=callstack-kernel") < 0)
        return -1;

    if (run_cmd("sudo lttng start wyvern-session") < 0)
        return -1;

    if (run_cmd("sudo lttng enable-event --session=wyvern-session-ust -u --channel=uchan wyvern:probe2") < 0)
        return -1;
    
    printf("[daemon] LTTng tracing started successfully\n");
    return 0;
}

static int lttng_snapshot(void)
{
    pid_t c = fork();

    if (c < 0) {
        perror("fork(lttng_snapshot)");
        return -1;
    }

    if (c == 0) {
        // child process: run sequence

        printf("[daemon] recording immediate snapshot\n");
        if (run_cmd("sudo lttng snapshot record --session=wyvern-session --name=wyvern-snapshot-before") < 0)
            _exit(1);
        
        for (int i = 60; i > 0; --i) {
            printf("\r[daemon] waiting %2d seconds... ", i);
            fflush(stdout);
            sleep(1);
        }
        printf("\n");

        printf("[daemon] recording post-monitor snapshot\n");
        if (run_cmd("sudo lttng snapshot record --session=wyvern-session --name=wyvern-snapshot-60s-monitor") < 0)
            _exit(1);

        _exit(0);
    }

    // parent returns immediately (non-blocking)
    printf("[daemon] LTTng snapshot sequence started (before + 60s)\n");
    return 0;
}

static int stop_lttng(void)
{
    if (run_cmd("sudo lttng stop wyvern-session 2>/dev/null || true") < 0) {
        fprintf(stderr, "[daemon] warning: failed to stop LTTng\n");
        return -1;
    }

    printf("[daemon] LTTng stopped\n");
    return 0;
}

static int lttng_ust_record(void)
{

    if (run_cmd("sudo lttng start wyvern-session-ust") < 0)
        return -1;
    
    printf("[daemon] Wyvern UST tracing started successfully\n");
    return 0;
}

static int lttng_ust_stop(void)
{

    if (run_cmd("sudo lttng stop wyvern-session-ust 2>/dev/null || true") < 0) {
        fprintf(stderr, "[daemon] warning: failed to stop LTTng UST\n");
        return -1;
    }
    
    printf("[daemon] Wyvern UST tracing stopped successfully\n");
    return 0;
}

// ----- end lttng functions -----

// gcc -O2 -Wall -Wextra -o wyvern_daemon wyvern_daemon.c
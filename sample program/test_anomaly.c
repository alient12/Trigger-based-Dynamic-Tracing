// test_anomaly.c
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <string.h>
#include <sys/syscall.h>

static atomic_int hog_on = 0;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;

static int ncpu_online(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 1;
}

static void pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        // POC: just print once; if you run under preload, this still goes to stderr
        fprintf(stderr, "sched_setaffinity(cpu=%d) failed: %s\n", cpu, strerror(errno));
    }
}

static void busy_work_us(int us) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    long long target_ns = (long long)us * 1000LL;

    for (;;) {
        for (volatile int i = 0; i < 1000; i++) {
            int x = i * i; (void)x; // burn some CPU cycles
        }
        if ((rand() & 0x3F) == 0) sched_yield();

        clock_gettime(CLOCK_MONOTONIC, &now);
        long long dt_ns =
            (now.tv_sec - start.tv_sec) * 1000000000LL +
            (now.tv_nsec - start.tv_nsec);
        if (dt_ns >= target_ns) break;
    }
}

static void *victim_thread(void *arg) {
    int cpu = *(int*)arg;
    pin_to_cpu(cpu);

    struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000}; // 5ms
    while (1) {
        nanosleep(&sleep_ts, NULL); // generates wakeups
        busy_work_us(200);          // small “normal” work
    }
    return NULL;
}

typedef struct { int cpu; } HogArg;

static void *hog_thread(void *arg) {
    int cpu = ((HogArg*)arg)->cpu;
    pin_to_cpu(cpu);

    for (;;) {
        // Park cleanly when off
        pthread_mutex_lock(&g_mu);
        while (!atomic_load_explicit(&hog_on, memory_order_relaxed)) {
            pthread_cond_wait(&g_cv, &g_mu);
        }
        pthread_mutex_unlock(&g_mu);

        // call getpid() 10000 times to generate syscalls and some scheduler activity
        volatile long sink;
        for (int i = 0; i < 10000; i++) {
            sink = syscall(SYS_getpid);
        }
        
        // Burn CPU while on
        while (atomic_load_explicit(&hog_on, memory_order_relaxed)) {
            for (volatile unsigned long long i = 0; i < 300000000ULL; i++) {}
        }
    }
    return NULL;
}

static void set_hog_on(int on) {
    atomic_store_explicit(&hog_on, on, memory_order_relaxed);
    if (on) {
        pthread_mutex_lock(&g_mu);
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
}

int main(int argc, char **argv) {
    int warmup_sec   = (argc > 1) ? atoi(argv[1]) : 20;
    int abnormal_sec = (argc > 2) ? atoi(argv[2]) : 10;
    int hog_threads  = (argc > 3) ? atoi(argv[3]) : 0; // 0 => auto

    srand((unsigned)time(NULL));

    int ncpu = ncpu_online();
    if (hog_threads <= 0) hog_threads = ncpu * 2; // oversubscribe => strong contention

    int victim_cpu = 0;
    const char *s = getenv("TEST_CPU");
    if (s && *s) {
        int v = atoi(s);
        if (v >= 0 && v < ncpu) victim_cpu = v;
    }

    printf("test_anomaly: warmup=%ds abnormal=%ds ncpu=%d hog_threads=%d victim_cpu=%d\n",
           warmup_sec, abnormal_sec, ncpu, hog_threads, victim_cpu);
    fflush(stdout);

    pthread_t victim;
    if (pthread_create(&victim, NULL, victim_thread, &victim_cpu) != 0) {
        perror("pthread_create victim");
        return 1;
    }

    pthread_t *hogs = calloc((size_t)hog_threads, sizeof(*hogs));
    HogArg *hargs = calloc((size_t)hog_threads, sizeof(*hargs));
    if (!hogs || !hargs) {
        perror("calloc");
        return 1;
    }

    // Spread hog threads across CPUs (round-robin)
    for (int i = 0; i < hog_threads; i++) {
        hargs[i].cpu = i % ncpu;
        if (pthread_create(&hogs[i], NULL, hog_thread, &hargs[i]) != 0) {
            perror("pthread_create hog");
            return 1;
        }
    }

    for (int t = 0; t < warmup_sec; t++) {
        if (t % 5 == 0) { printf("[baseline] t=%ds\n", t); fflush(stdout); }
        sleep(1);
    }

    printf("[abnormal] enabling CPU hogs\n");
    fflush(stdout);
    set_hog_on(1);

    for (int t = 0; t < abnormal_sec; t++) {
        if (t % 5 == 0) { printf("[abnormal] t=%ds\n", t); fflush(stdout); }
        sleep(1);
    }

    printf("[recovery] disabling CPU hogs\n");
    fflush(stdout);
    set_hog_on(0);

    while (1) {
        printf("[recovery] still alive\n");
        fflush(stdout);
        sleep(5);
    }
}

// gcc -O2 -g -fPIE -pie -pthread -o test_anomaly test_anomaly.c
#define _GNU_SOURCE
#include "wyvern_ipc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DAEMON_NAME "wyvern_daemon"

static uint64_t now_ns_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void die(const char *msg)
{
    fprintf(stderr, "signal_sender: %s (errno=%d: %s)\n",
            msg, errno, strerror(errno));
    exit(1);
}

static pid_t find_daemon_pid(const char *name)
{
    DIR *d = opendir("/proc");
    if (!d) die("opendir /proc failed");

    struct dirent *de;

    while ((de = readdir(d))) {
        if (de->d_type != DT_DIR)
            continue;

        pid_t pid = atoi(de->d_name);
        if (pid <= 0)
            continue;

        char path[256];
        snprintf(path, sizeof(path), "/proc/%d/comm", pid);

        FILE *f = fopen(path, "r");
        if (!f)
            continue;

        char comm[256];
        if (fgets(comm, sizeof(comm), f)) {
            comm[strcspn(comm, "\n")] = 0;

            if (strcmp(comm, name) == 0) {
                fclose(f);
                closedir(d);
                return pid;
            }
        }

        fclose(f);
    }

    closedir(d);
    return -1;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr,
            "Usage: %s <target_pid> <target_tid> <action>\n",
            argv[0]);
        return 2;
    }

    int target_pid = atoi(argv[1]);
    int target_tid = atoi(argv[2]);
    int action     = atoi(argv[3]);

    pid_t daemon_pid = find_daemon_pid(DAEMON_NAME);

    if (daemon_pid <= 0)
        die("daemon not found");

    int fd = shm_open(WYVERN_SHM_NAME, O_RDWR | O_CREAT, 0600);
    if (fd < 0)
        die("shm_open failed");

    if (ftruncate(fd, sizeof(WyvernShm)) != 0)
        die("ftruncate failed");

    WyvernShm *shm = mmap(NULL, sizeof(WyvernShm),
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, 0);

    if (shm == MAP_FAILED)
        die("mmap failed");

    close(fd);

    if (shm->magic != WYVERN_SHM_MAGIC) {
        memset(shm, 0, sizeof(*shm));
        shm->magic = WYVERN_SHM_MAGIC;
        shm->version = WYVERN_SHM_VERSION;
        shm->size = sizeof(WyvernShm);
    }

    shm->ts_ns = now_ns_monotonic();
    shm->target_pid = target_pid;
    shm->target_tid = target_tid;
    shm->action = action;
    shm->seq++;

    if (kill(daemon_pid, SIGUSR1) != 0)
        die("kill(SIGUSR1) failed");

    printf("sent event seq=%lu to daemon pid=%d\n",
           shm->seq, daemon_pid);

    munmap(shm, sizeof(*shm));

    return 0;
}

// gcc -O2 -Wall -Wextra -o signal_sender signal_sender.c
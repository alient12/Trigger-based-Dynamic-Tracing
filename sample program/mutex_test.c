#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>

pthread_mutex_t lock;

// Thread 1: repeatedly holds the mutex for a long time
void* long_task(void* arg) {
    pid_t tid = syscall(SYS_gettid);

    printf("Thread 1 started. TID: %d\n", tid);

    size_t count = 0;

    while (1) {
        count++;
        if (count < 100) {
            pthread_mutex_lock(&lock);
            usleep(1000);
            pthread_mutex_unlock(&lock);
            continue;
        } else if (count == 100) {
            printf("Thread 1 (TID %d): Starting anomaly loop...\n", tid);
        }
        printf("Thread 1 (TID %d): Trying to acquire mutex...\n", tid);
        pthread_mutex_lock(&lock);

        if (count%3 == 0) {
            printf("Thread 1 (TID %d): Mutex acquired. Doing long task...\n", tid);
            sleep(5);
        } else {
            printf("Thread 1 (TID %d): Mutex acquired. Doing short task...\n", tid);
            usleep(500000); // short work (0.5 sec)
        }

        printf("Thread 1 (TID %d): Releasing mutex.\n", tid);
        pthread_mutex_unlock(&lock);

        sleep(1);
    }
    return NULL;
}

// Thread 2: repeatedly tries to acquire the mutex
void* waiting_task(void* arg) {
    pid_t tid = syscall(SYS_gettid);
    printf("Thread 2 started. TID: %d\n", tid);

    size_t count = 0;

    while (1) {
        count++;
        if (count < 100) {
            pthread_mutex_lock(&lock);
            usleep(1000);
            pthread_mutex_unlock(&lock);
            continue;
        }
        printf("Thread 2 (TID %d): Trying to acquire mutex...\n", tid);
        pthread_mutex_lock(&lock);

        printf("Thread 2 (TID %d): Mutex acquired! Doing quick work.\n", tid);
        usleep(200000); // short work (0.2 sec)

        pthread_mutex_unlock(&lock);
        printf("Thread 2 (TID %d): Released mutex.\n", tid);

        sleep(1);
    }
    return NULL;
}

int main() {
    printf("PID: %d\n", getpid());

    pthread_t t1, t2;

    pthread_mutex_init(&lock, NULL);

    pthread_create(&t1, NULL, long_task, NULL);
    pthread_create(&t2, NULL, waiting_task, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    pthread_mutex_destroy(&lock);

    return 0;
}


// gcc -O2 -Wall -Wextra -o mutex_test mutex_test.c -lpthread
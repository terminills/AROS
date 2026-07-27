/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#define THREAD_COUNT 2

static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int ready;
static int release_waiters;
static int signaled;
static int timed_out;

static void *waiter_thread(void *unused)
{
    struct timespec deadline;
    int result = 0;

    (void)unused;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;

    pthread_mutex_lock(&mutex);
    ++ready;
    while (!release_waiters && result == 0)
        result = pthread_cond_timedwait(&condition, &mutex, &deadline);

    if (result == 0)
        ++signaled;
    else if (result == ETIMEDOUT)
        ++timed_out;
    pthread_mutex_unlock(&mutex);
    return NULL;
}

int main(void)
{
    pthread_t threads[THREAD_COUNT];
    unsigned int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    puts("pthread_cond_signal: start");

    for (i = 0; i < THREAD_COUNT; ++i)
    {
        if (pthread_create(&threads[i], NULL, waiter_thread, NULL) != 0)
        {
            printf("pthread_cond_signal: failed to create thread %u\n", i);
            return 1;
        }
    }

    for (;;)
    {
        const struct timespec pause = { 0, 1000000 };

        pthread_mutex_lock(&mutex);
        if (ready == THREAD_COUNT)
            break;
        pthread_mutex_unlock(&mutex);
        nanosleep(&pause, NULL);
    }

    puts("pthread_cond_signal: both waiters ready");

    /*
     * Keep the mutex locked across both signals. A signaled waiter therefore
     * cannot reacquire it and remove itself before the second signal.
     */
    release_waiters = 1;
    pthread_cond_signal(&condition);
    pthread_cond_signal(&condition);
    puts("pthread_cond_signal: sent two wakes");
    pthread_mutex_unlock(&mutex);

    for (i = 0; i < THREAD_COUNT; ++i)
        pthread_join(threads[i], NULL);

    if (signaled != THREAD_COUNT || timed_out != 0)
    {
        printf("pthread_cond_signal: signaled=%d timed_out=%d\n",
            signaled, timed_out);
        return 2;
    }

    puts("pthread_cond_signal: distinct waiter wake passed");
    return 0;
}

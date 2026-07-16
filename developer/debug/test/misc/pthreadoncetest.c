/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <pthread.h>
#include <sched.h>
#include <stdio.h>

#define THREAD_COUNT 4

static pthread_once_t static_once = PTHREAD_ONCE_INIT;
static pthread_once_t zero_once = {0};
static pthread_once_t concurrent_once = {0};
static int static_calls;
static int zero_calls;
static int concurrent_calls;

static void static_init(void)
{
    ++static_calls;
}

static void zero_init(void)
{
    ++zero_calls;
}

static void concurrent_init(void)
{
    unsigned int i;

    ++concurrent_calls;
    for (i = 0; i < 64; ++i)
        sched_yield();
}

static void *once_thread(void *unused)
{
    (void)unused;
    pthread_once(&concurrent_once, concurrent_init);
    return NULL;
}

int main(void)
{
    pthread_t threads[THREAD_COUNT];
    unsigned int i;

    pthread_once(&static_once, static_init);
    pthread_once(&static_once, static_init);
    if (static_calls != 1)
    {
        printf("pthread_once: static initializer ran %d times\n", static_calls);
        return 1;
    }

    pthread_once(&zero_once, zero_init);
    pthread_once(&zero_once, zero_init);
    if (zero_calls != 1)
    {
        printf("pthread_once: zero initializer ran %d times\n", zero_calls);
        return 2;
    }

    for (i = 0; i < THREAD_COUNT; ++i)
    {
        if (pthread_create(&threads[i], NULL, once_thread, NULL) != 0)
        {
            printf("pthread_once: failed to create thread %u\n", i);
            return 3;
        }
    }

    for (i = 0; i < THREAD_COUNT; ++i)
        pthread_join(threads[i], NULL);

    if (concurrent_calls != 1)
    {
        printf("pthread_once: concurrent initializer ran %d times\n",
            concurrent_calls);
        return 4;
    }

    printf("pthread_once: all tests passed\n");
    return 0;
}

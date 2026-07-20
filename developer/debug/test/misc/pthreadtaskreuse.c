/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <proto/dos.h>
#include <proto/arossupport.h>
#include <proto/exec.h>

#include <pthread.h>
#include <stdio.h>

#define TEST_MARKER ((IPTR)0x50545258)
#define MAX_REUSE_ATTEMPTS 128

static pthread_key_t test_key;
static struct Task *volatile victim_task;
static volatile int victim_setup_ok;
static volatile int victim_ready;
static volatile int detached_ready;
static volatile int task_reused;
static volatile IPTR observed_value;

static void *victim_thread(void *unused)
{
    (void)unused;

    victim_setup_ok = pthread_setspecific(test_key, (void *)TEST_MARKER) == 0;
    victim_task = FindTask(NULL);
    victim_ready = 1;

    /* Exercise the unattended-alert lifecycle without blocking on a requester. */
    RemTask(NULL);
    return NULL;
}

static void *probe_thread(void *unused)
{
    (void)unused;

    if (FindTask(NULL) == victim_task)
    {
        task_reused = 1;
        observed_value = (IPTR)pthread_getspecific(test_key);
    }

    return NULL;
}

static void *detached_thread(void *unused)
{
    (void)unused;

    detached_ready = 1;
    RemTask(NULL);
    return NULL;
}

int main(void)
{
    pthread_t victim;
    pthread_t probe;
    pthread_t detached;
    void *victim_result;
    unsigned int attempt;
    unsigned int wait_attempt;

    kprintf("[pthreadtaskreuse] BEGIN abnormal task-address reuse regression\n");

    if (pthread_key_create(&test_key, NULL) != 0)
    {
        printf("FAIL: could not create pthread TLS key\n");
        return 1;
    }

    if (pthread_create(&victim, NULL, victim_thread, NULL) != 0)
    {
        printf("FAIL: could not create abnormal-exit thread\n");
        pthread_key_delete(test_key);
        return 2;
    }

    for (wait_attempt = 0;
         wait_attempt < 50 && !victim_ready;
         ++wait_attempt)
    {
        Delay(1);
    }

    if (!victim_ready)
    {
        printf("FAIL: abnormal-exit thread did not start\n");
        pthread_key_delete(test_key);
        return 3;
    }

    if (!victim_setup_ok)
    {
        printf("FAIL: abnormal-exit thread could not set TLS\n");
        pthread_key_delete(test_key);
        return 4;
    }

    /* Let RemTask finish before asking Exec for replacement task storage. */
    Delay(5);

    for (attempt = 0; attempt < MAX_REUSE_ATTEMPTS && !task_reused; ++attempt)
    {
        if (pthread_create(&probe, NULL, probe_thread, NULL) != 0)
        {
            printf("FAIL: could not create probe thread %u\n", attempt);
            pthread_key_delete(test_key);
            return 5;
        }

        pthread_join(probe, NULL);
    }

    if (!task_reused)
    {
        printf("SKIP: Exec did not reuse the abnormal thread task address\n");
    }
    else if (observed_value != 0)
    {
        printf("FAIL: replacement task inherited stale TLS %p\n",
            (void *)observed_value);
        kprintf("[pthreadtaskreuse] FAIL stale TLS %p\n",
            (void *)observed_value);
        return 6;
    }

    if (pthread_join(victim, &victim_result) != 0)
    {
        printf("FAIL: abnormal-exit thread could not be joined\n");
        pthread_key_delete(test_key);
        return 7;
    }

    if (victim_result != NULL)
    {
        printf("FAIL: abnormal-exit thread returned %p\n", victim_result);
        pthread_key_delete(test_key);
        return 8;
    }

    if (pthread_create(&detached, NULL, detached_thread, NULL) != 0)
    {
        printf("FAIL: could not create detached abnormal-exit thread\n");
        pthread_key_delete(test_key);
        return 9;
    }

    for (wait_attempt = 0;
         wait_attempt < 50 && !detached_ready;
         ++wait_attempt)
    {
        Delay(1);
    }

    if (!detached_ready)
    {
        printf("FAIL: detached abnormal-exit thread did not start\n");
        pthread_key_delete(test_key);
        return 10;
    }

    Delay(5);
    if (pthread_detach(detached) != 0)
    {
        printf("FAIL: detached abnormal-exit thread was not reclaimed\n");
        pthread_key_delete(test_key);
        return 11;
    }

    pthread_key_delete(test_key);

    if (task_reused)
    {
        printf("PASS: recycled task address has fresh pthread TLS\n");
        kprintf("[pthreadtaskreuse] PASS recycled task address has fresh pthread TLS\n");
    }
    printf("PASS: abnormal-exit thread was joined and reclaimed\n");
    kprintf("[pthreadtaskreuse] PASS abnormal thread joined and reclaimed\n");
    printf("PASS: abnormal-exit detached slot was reclaimed\n");
    kprintf("[pthreadtaskreuse] PASS abnormal detached slot reclaimed\n");
    return 0;
}

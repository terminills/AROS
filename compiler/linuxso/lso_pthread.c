/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    linuxso glibc pthread/semaphore ABI on top of the AROS pthread linklib.

    glibc's synchronisation objects are much smaller than AROS's (mutex 40
    vs 248 bytes, cond 48 vs 264, rwlock 56 vs 232, sem_t 32 vs 552), so
    every guest object carries a pointer to a heap-allocated AROS object in
    its first word.  Static initialisers are all-zero on glibc; the pointer
    is created on first use under one global semaphore.
*/

#include <proto/exec.h>
#include <exec/memory.h>

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <time.h>

#include "linuxso_intern.h"
#include "lso_lx.h"

/* Linux values */
#define LX_CLOCK_REALTIME  0
#define LX_CLOCK_MONOTONIC 1
#define LX_PTHREAD_CREATE_DETACHED 1
#define LX_CPU_SETSIZE 1024


static struct SignalSemaphore lazy_lock;
static int lazy_lock_ready;

static void lazy_init(void)
{
    Forbid();
    if (!lazy_lock_ready)
    {
        InitSemaphore(&lazy_lock);
        lazy_lock_ready = 1;
    }
    Permit();
}

/* ---- guest object layouts ------------------------------------------------ */

struct lx_mutex {            /* 40 bytes on glibc */
    pthread_mutex_t *impl;
    uint32_t magic;
    uint32_t pad;
    int kind;                /* glibc __kind offset 16: static recursive initialiser sets 1 */
    int pad2[3];
};

struct lx_cond {             /* 48 bytes */
    pthread_cond_t *impl;
    uint32_t magic;
    int clock;               /* Linux clock id, set by pthread_cond_init */
    uint64_t pad[4];
};

struct lx_rwlock {           /* 56 bytes */
    pthread_rwlock_t *impl;
    uint32_t magic;
    uint32_t pad;
    uint64_t pad2[5];
};

struct lx_sem {              /* 32 bytes */
    sem_t *impl;
    uint32_t magic;
    uint32_t pad;
    uint64_t pad2[2];
};

struct lx_attr {             /* 56 bytes */
    uint32_t magic;
    int detachstate;
    size_t stacksize;
    uint64_t pad[5];
};

struct lx_mutexattr { int kind; };
struct lx_condattr { int value; };   /* (clock << 1) | 1 */

#define LX_MAGIC_MUTEX 0x4d4f534c
#define LX_MAGIC_COND  0x434f534c
#define LX_MAGIC_RW    0x574f534c
#define LX_MAGIC_SEM   0x534f534c
#define LX_MAGIC_ATTR  0x414f534c

/* ---- lazy creation -------------------------------------------------------- */

static pthread_mutex_t *mutex_impl(struct lx_mutex *m)
{
    pthread_mutex_t *impl = *(pthread_mutex_t *volatile *)&m->impl;

    if (impl)
        return impl;
    lazy_init();
    ObtainSemaphore(&lazy_lock);
    impl = m->impl;
    if (!impl)
    {
        impl = AllocMem(sizeof(*impl), MEMF_ANY | MEMF_CLEAR);
        if (impl)
        {
            pthread_mutexattr_t a;
            pthread_mutexattr_init(&a);
            pthread_mutexattr_settype(&a, m->kind == 1 ? PTHREAD_MUTEX_RECURSIVE :
                                          m->kind == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
            pthread_mutex_init(impl, &a);
            pthread_mutexattr_destroy(&a);
            m->magic = LX_MAGIC_MUTEX;
            __sync_synchronize();
            m->impl = impl;
        }
    }
    ReleaseSemaphore(&lazy_lock);
    return impl;
}

static pthread_cond_t *cond_impl(struct lx_cond *c)
{
    pthread_cond_t *impl = *(pthread_cond_t *volatile *)&c->impl;

    if (impl)
        return impl;
    lazy_init();
    ObtainSemaphore(&lazy_lock);
    impl = c->impl;
    if (!impl)
    {
        impl = AllocMem(sizeof(*impl), MEMF_ANY | MEMF_CLEAR);
        if (impl)
        {
            pthread_cond_init(impl, NULL);
            c->magic = LX_MAGIC_COND;
            __sync_synchronize();
            c->impl = impl;
        }
    }
    ReleaseSemaphore(&lazy_lock);
    return impl;
}

static pthread_rwlock_t *rw_impl(struct lx_rwlock *r)
{
    pthread_rwlock_t *impl = *(pthread_rwlock_t *volatile *)&r->impl;

    if (impl)
        return impl;
    lazy_init();
    ObtainSemaphore(&lazy_lock);
    impl = r->impl;
    if (!impl)
    {
        impl = AllocMem(sizeof(*impl), MEMF_ANY | MEMF_CLEAR);
        if (impl)
        {
            pthread_rwlock_init(impl, NULL);
            r->magic = LX_MAGIC_RW;
            __sync_synchronize();
            r->impl = impl;
        }
    }
    ReleaseSemaphore(&lazy_lock);
    return impl;
}

/* ---- mutex ---------------------------------------------------------------- */

int lx_pthread_mutexattr_init(struct lx_mutexattr *a)    { a->kind = 0; return 0; }
int lx_pthread_mutexattr_destroy(struct lx_mutexattr *a) { (void)a; return 0; }
int lx_pthread_mutexattr_settype(struct lx_mutexattr *a, int kind) { a->kind = kind; return 0; }

int lx_pthread_mutex_init(struct lx_mutex *m, const struct lx_mutexattr *a)
{
    memset(m, 0, sizeof(*m));
    m->kind = a ? a->kind : 0;
    return mutex_impl(m) ? 0 : LX_ENOMEM;
}

int lx_pthread_mutex_destroy(struct lx_mutex *m)
{
    pthread_mutex_t *impl = m->impl;

    if (impl)
    {
        m->impl = NULL;
        pthread_mutex_destroy(impl);
        FreeMem(impl, sizeof(*impl));
    }
    m->magic = 0;
    return 0;
}

int lx_pthread_mutex_lock(struct lx_mutex *m)
{
    pthread_mutex_t *impl = mutex_impl(m);
    return impl ? lso_pthread_ret(pthread_mutex_lock(impl)) : LX_ENOMEM;
}

int lx_pthread_mutex_trylock(struct lx_mutex *m)
{
    pthread_mutex_t *impl = mutex_impl(m);
    return impl ? lso_pthread_ret(pthread_mutex_trylock(impl)) : LX_ENOMEM;
}

int lx_pthread_mutex_unlock(struct lx_mutex *m)
{
    pthread_mutex_t *impl = m->impl;
    return impl ? lso_pthread_ret(pthread_mutex_unlock(impl)) : LX_EPERM;
}

/* ---- condition variables -------------------------------------------------- */

int lx_pthread_condattr_init(struct lx_condattr *a)    { a->value = 1; return 0; }
int lx_pthread_condattr_destroy(struct lx_condattr *a) { (void)a; return 0; }
int lx_pthread_condattr_setclock(struct lx_condattr *a, int clock)
{
    if (clock != LX_CLOCK_REALTIME && clock != LX_CLOCK_MONOTONIC)
        return LX_EINVAL;
    a->value = (clock << 1) | 1;
    return 0;
}

int lx_pthread_cond_init(struct lx_cond *c, const struct lx_condattr *a)
{
    memset(c, 0, sizeof(*c));
    c->clock = a ? (a->value >> 1) : LX_CLOCK_REALTIME;
    return cond_impl(c) ? 0 : LX_ENOMEM;
}

int lx_pthread_cond_destroy(struct lx_cond *c)
{
    pthread_cond_t *impl = c->impl;

    if (impl)
    {
        c->impl = NULL;
        pthread_cond_destroy(impl);
        FreeMem(impl, sizeof(*impl));
    }
    c->magic = 0;
    return 0;
}

int lx_pthread_cond_signal(struct lx_cond *c)
{
    pthread_cond_t *impl = cond_impl(c);
    return impl ? lso_pthread_ret(pthread_cond_signal(impl)) : LX_ENOMEM;
}

int lx_pthread_cond_broadcast(struct lx_cond *c)
{
    pthread_cond_t *impl = cond_impl(c);
    return impl ? lso_pthread_ret(pthread_cond_broadcast(impl)) : LX_ENOMEM;
}

int lx_pthread_cond_wait(struct lx_cond *c, struct lx_mutex *m)
{
    pthread_cond_t *impl = cond_impl(c);
    pthread_mutex_t *mi = mutex_impl(m);

    if (!impl || !mi)
        return LX_ENOMEM;
    return lso_pthread_ret(pthread_cond_wait(impl, mi));
}

int lx_pthread_cond_timedwait(struct lx_cond *c, struct lx_mutex *m, const struct lx_timespec *abstime)
{
    pthread_cond_t *impl = cond_impl(c);
    pthread_mutex_t *mi = mutex_impl(m);
    struct timespec now, rel;

    if (!impl || !mi)
        return LX_ENOMEM;
    if (!abstime)
        return lso_pthread_ret(pthread_cond_wait(impl, mi));
    if (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000L)
        return LX_EINVAL;

    /* the guest's abstime is on the cond's clock; AROS wants a relative delay */
    lso_clock_gettime(c->clock, &now);
    rel.tv_sec = (time_t)(abstime->tv_sec - now.tv_sec);
    rel.tv_nsec = abstime->tv_nsec - now.tv_nsec;
    if (rel.tv_nsec < 0)
    {
        rel.tv_nsec += 1000000000L;
        rel.tv_sec--;
    }
    if (rel.tv_sec < 0)
        return LX_ETIMEDOUT;
    return lso_pthread_ret(pthread_cond_timedwait_relative_np(impl, mi, &rel));
}

/* ---- rwlock --------------------------------------------------------------- */

int lx_pthread_rwlock_init(struct lx_rwlock *r, const void *attr)
{
    (void)attr;
    memset(r, 0, sizeof(*r));
    return rw_impl(r) ? 0 : LX_ENOMEM;
}

int lx_pthread_rwlock_destroy(struct lx_rwlock *r)
{
    pthread_rwlock_t *impl = r->impl;

    if (impl)
    {
        r->impl = NULL;
        pthread_rwlock_destroy(impl);
        FreeMem(impl, sizeof(*impl));
    }
    r->magic = 0;
    return 0;
}

int lx_pthread_rwlock_rdlock(struct lx_rwlock *r)
{
    pthread_rwlock_t *impl = rw_impl(r);
    return impl ? lso_pthread_ret(pthread_rwlock_rdlock(impl)) : LX_ENOMEM;
}

int lx_pthread_rwlock_wrlock(struct lx_rwlock *r)
{
    pthread_rwlock_t *impl = rw_impl(r);
    return impl ? lso_pthread_ret(pthread_rwlock_wrlock(impl)) : LX_ENOMEM;
}

int lx_pthread_rwlock_tryrdlock(struct lx_rwlock *r)
{
    pthread_rwlock_t *impl = rw_impl(r);
    return impl ? lso_pthread_ret(pthread_rwlock_tryrdlock(impl)) : LX_ENOMEM;
}

int lx_pthread_rwlock_trywrlock(struct lx_rwlock *r)
{
    pthread_rwlock_t *impl = rw_impl(r);
    return impl ? lso_pthread_ret(pthread_rwlock_trywrlock(impl)) : LX_ENOMEM;
}

int lx_pthread_rwlock_unlock(struct lx_rwlock *r)
{
    pthread_rwlock_t *impl = r->impl;
    return impl ? lso_pthread_ret(pthread_rwlock_unlock(impl)) : LX_EPERM;
}

/* ---- once ----------------------------------------------------------------- */

static pthread_mutex_t once_lock;
static int once_lock_ready;

int lx_pthread_once(int *once, void (*init_routine)(void))
{
    if (*(volatile int *)once == 2)
        return 0;
    Forbid();
    if (!once_lock_ready)
    {
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&once_lock, &a);
        pthread_mutexattr_destroy(&a);
        once_lock_ready = 1;
    }
    Permit();
    pthread_mutex_lock(&once_lock);
    if (*once != 2)
    {
        *once = 1;
        init_routine();
        __sync_synchronize();
        *once = 2;
    }
    pthread_mutex_unlock(&once_lock);
    return 0;
}

/* ---- keys ----------------------------------------------------------------- */

int lx_pthread_key_create(unsigned int *key, void (*dtor)(void *))
{
    pthread_key_t k;
    int r = pthread_key_create(&k, dtor);
    if (r == 0)
        *key = k;
    return lso_pthread_ret(r);
}

int lx_pthread_key_delete(unsigned int key)
{
    return lso_pthread_ret(pthread_key_delete((pthread_key_t)key));
}

void *lx_pthread_getspecific(unsigned int key)
{
    return pthread_getspecific((pthread_key_t)key);
}

int lx_pthread_setspecific(unsigned int key, const void *value)
{
    return lso_pthread_ret(pthread_setspecific((pthread_key_t)key, value));
}

/* ---- attributes ----------------------------------------------------------- */

int lx_pthread_attr_init(struct lx_attr *a)
{
    memset(a, 0, sizeof(*a));
    a->magic = LX_MAGIC_ATTR;
    return 0;
}

int lx_pthread_attr_destroy(struct lx_attr *a)    { a->magic = 0; return 0; }
int lx_pthread_attr_setdetachstate(struct lx_attr *a, int s) { a->detachstate = s; return 0; }
int lx_pthread_attr_getdetachstate(const struct lx_attr *a, int *s) { *s = a->detachstate; return 0; }
int lx_pthread_attr_setstacksize(struct lx_attr *a, size_t s)
{
    if (s < 16384)
        return LX_EINVAL;
    a->stacksize = s;
    return 0;
}
int lx_pthread_attr_getstacksize(const struct lx_attr *a, size_t *s) { *s = a->stacksize; return 0; }

/* ---- threads -------------------------------------------------------------- */

struct lx_thread_start {
    void *(*fn)(void *);
    void *arg;
};

static void *thread_trampoline(void *p)
{
    struct lx_thread_start s = *(struct lx_thread_start *)p;
    void *ret;

    FreeMem(p, sizeof(s));
    ret = s.fn(s.arg);
    lso_task_free(FindTask(NULL));
    return ret;
}

int lx_pthread_create(unsigned long *thread, const struct lx_attr *a,
                      void *(*fn)(void *), void *arg)
{
    pthread_attr_t attr;
    pthread_t t = 0;
    struct lx_thread_start *s;
    int r;

    s = AllocMem(sizeof(*s), MEMF_ANY);
    if (!s)
        return LX_EAGAIN;
    s->fn = fn;
    s->arg = arg;

    pthread_attr_init(&attr);
    if (a && a->magic == LX_MAGIC_ATTR)
    {
        if (a->stacksize)
            pthread_attr_setstacksize(&attr, a->stacksize);
        if (a->detachstate == LX_PTHREAD_CREATE_DETACHED)
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    }
    r = pthread_create(&t, &attr, thread_trampoline, s);
    pthread_attr_destroy(&attr);
    if (r)
    {
        FreeMem(s, sizeof(*s));
        return lso_pthread_ret(r);
    }
    *thread = t;
    LSO_DEBUG("pthread_create -> %lu (stack %lu)", (unsigned long)t,
              (unsigned long)((a && a->magic == LX_MAGIC_ATTR) ? a->stacksize : 0));
    return 0;
}

int lx_pthread_join(unsigned long thread, void **ret)
{
    return lso_pthread_ret(pthread_join((pthread_t)thread, ret));
}

int lx_pthread_detach(unsigned long thread)
{
    return lso_pthread_ret(pthread_detach((pthread_t)thread));
}

unsigned long lx_pthread_self(void)
{
    return (unsigned long)pthread_self();
}

int lx_pthread_equal(unsigned long a, unsigned long b)
{
    return a == b;
}

void lx_pthread_exit(void *ret)
{
    lso_task_free(FindTask(NULL));
    pthread_exit(ret);
}

int lx_pthread_setname_np(unsigned long thread, const char *name)
{
    return lso_pthread_ret(pthread_setname_np((pthread_t)thread, name));
}

int lx_pthread_getaffinity_np(unsigned long thread, size_t setsize, unsigned long *set)
{
    unsigned int n = lso_ncpus(), i;

    (void)thread;
    if (setsize < sizeof(unsigned long))
        return LX_EINVAL;
    memset(set, 0, setsize);
    for (i = 0; i < n && i < setsize * 8; i++)
        set[i / 64] |= 1UL << (i % 64);
    return 0;
}

int lx_sched_getaffinity(int pid, size_t setsize, unsigned long *set)
{
    (void)pid;
    if (lx_pthread_getaffinity_np(0, setsize, set))
    {
        lso_set_errno(LX_EINVAL);
        return -1;
    }
    return 0;
}

int lx_sched_cpucount(size_t setsize, const unsigned long *set)
{
    size_t i;
    int n = 0;

    for (i = 0; i < setsize / sizeof(unsigned long); i++)
        n += __builtin_popcountl(set[i]);
    return n;
}

/* ---- semaphores ----------------------------------------------------------- */

int lx_sem_init(struct lx_sem *s, int pshared, unsigned int value)
{
    sem_t *impl;

    (void)pshared;
    memset(s, 0, sizeof(*s));
    impl = AllocMem(sizeof(*impl), MEMF_ANY | MEMF_CLEAR);
    if (!impl)
    {
        lso_set_errno(LX_ENOMEM);
        return -1;
    }
    if (sem_init(impl, 0, value))
    {
        FreeMem(impl, sizeof(*impl));
        lso_set_errno_from_aros();
        return -1;
    }
    s->impl = impl;
    s->magic = LX_MAGIC_SEM;
    return 0;
}

int lx_sem_destroy(struct lx_sem *s)
{
    sem_t *impl = s->impl;

    if (impl)
    {
        s->impl = NULL;
        sem_destroy(impl);
        FreeMem(impl, sizeof(*impl));
    }
    s->magic = 0;
    return 0;
}

static int sem_call(struct lx_sem *s, int (*fn)(sem_t *))
{
    if (!s->impl)
    {
        lso_set_errno(LX_EINVAL);
        return -1;
    }
    if (fn(s->impl))
    {
        lso_set_errno_from_aros();
        return -1;
    }
    return 0;
}

int lx_sem_wait(struct lx_sem *s)    { return sem_call(s, sem_wait); }
int lx_sem_trywait(struct lx_sem *s) { return sem_call(s, sem_trywait); }
int lx_sem_post(struct lx_sem *s)    { return sem_call(s, sem_post); }

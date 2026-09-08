/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Prototypes of the glibc-ABI entry points the shim table exports.
    Every lx_* function takes and returns Linux/glibc layouts and sets
    the guest's (glibc-numbered) errno.
*/

#ifndef LSO_LX_H
#define LSO_LX_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

struct lx_mutex;
struct lx_mutexattr;
struct lx_cond;
struct lx_condattr;
struct lx_rwlock;
struct lx_attr;
struct lx_sem;
struct lx_timespec;
struct lx_timeval;

/* lso_pthread.c */
int lx_pthread_mutexattr_init(struct lx_mutexattr *a);
int lx_pthread_mutexattr_destroy(struct lx_mutexattr *a);
int lx_pthread_mutexattr_settype(struct lx_mutexattr *a, int kind);
int lx_pthread_mutex_init(struct lx_mutex *m, const struct lx_mutexattr *a);
int lx_pthread_mutex_destroy(struct lx_mutex *m);
int lx_pthread_mutex_lock(struct lx_mutex *m);
int lx_pthread_mutex_trylock(struct lx_mutex *m);
int lx_pthread_mutex_unlock(struct lx_mutex *m);
int lx_pthread_condattr_init(struct lx_condattr *a);
int lx_pthread_condattr_destroy(struct lx_condattr *a);
int lx_pthread_condattr_setclock(struct lx_condattr *a, int clock);
int lx_pthread_cond_init(struct lx_cond *c, const struct lx_condattr *a);
int lx_pthread_cond_destroy(struct lx_cond *c);
int lx_pthread_cond_signal(struct lx_cond *c);
int lx_pthread_cond_broadcast(struct lx_cond *c);
int lx_pthread_cond_wait(struct lx_cond *c, struct lx_mutex *m);
int lx_pthread_cond_timedwait(struct lx_cond *c, struct lx_mutex *m, const struct lx_timespec *abstime);
int lx_pthread_rwlock_init(struct lx_rwlock *r, const void *attr);
int lx_pthread_rwlock_destroy(struct lx_rwlock *r);
int lx_pthread_rwlock_rdlock(struct lx_rwlock *r);
int lx_pthread_rwlock_wrlock(struct lx_rwlock *r);
int lx_pthread_rwlock_tryrdlock(struct lx_rwlock *r);
int lx_pthread_rwlock_trywrlock(struct lx_rwlock *r);
int lx_pthread_rwlock_unlock(struct lx_rwlock *r);
int lx_pthread_once(int *once, void (*init_routine)(void));
int lx_pthread_key_create(unsigned int *key, void (*dtor)(void *));
int lx_pthread_key_delete(unsigned int key);
void *lx_pthread_getspecific(unsigned int key);
int lx_pthread_setspecific(unsigned int key, const void *value);
int lx_pthread_attr_init(struct lx_attr *a);
int lx_pthread_attr_destroy(struct lx_attr *a);
int lx_pthread_attr_setdetachstate(struct lx_attr *a, int s);
int lx_pthread_attr_getdetachstate(const struct lx_attr *a, int *s);
int lx_pthread_attr_setstacksize(struct lx_attr *a, size_t s);
int lx_pthread_attr_getstacksize(const struct lx_attr *a, size_t *s);
int lx_pthread_create(unsigned long *thread, const struct lx_attr *a, void *(*fn)(void *), void *arg);
int lx_pthread_join(unsigned long thread, void **ret);
int lx_pthread_detach(unsigned long thread);
unsigned long lx_pthread_self(void);
int lx_pthread_equal(unsigned long a, unsigned long b);
void lx_pthread_exit(void *ret);
int lx_pthread_setname_np(unsigned long thread, const char *name);
int lx_pthread_getaffinity_np(unsigned long thread, size_t setsize, unsigned long *set);
int lx_sched_getaffinity(int pid, size_t setsize, unsigned long *set);
int lx_sched_cpucount(size_t setsize, const unsigned long *set);
int lx_sem_init(struct lx_sem *s, int pshared, unsigned int value);
int lx_sem_destroy(struct lx_sem *s);
int lx_sem_wait(struct lx_sem *s);
int lx_sem_trywait(struct lx_sem *s);
int lx_sem_post(struct lx_sem *s);

/* lso_file.c */
FILE *lso_fopen(const char *path, const char *mode);
int lx_xstat64(int ver, const char *path, void *buf);
int lx_lxstat64(int ver, const char *path, void *buf);
int lx_fxstat64(int ver, int fd, void *buf);
int lx_stat64(const char *path, void *buf);
int lx_fstat64(int fd, void *buf);
int lx_open64(const char *path, int flags, ...);
int lx_creat64(const char *path, int mode);
int lx_close(int fd);
long lx_read(int fd, void *buf, size_t n);
long lx_write(int fd, const void *buf, size_t n);
long lx_pread64(int fd, void *buf, size_t n, int64_t off);
int64_t lx_lseek64(int fd, int64_t off, int whence);

/* lso_mem.c */
void *lx_mmap64(void *addr, size_t len, int prot, int flags, int fd, int64_t off);
int lx_munmap(void *addr, size_t len);
int lx_mprotect(void *addr, size_t len, int prot);
int lx_madvise(void *addr, size_t len, int advice);

/* lso_shim.c */
int lx_clock_gettime(int lx_clock, struct lx_timespec *ts);
int lx_gettimeofday(struct lx_timeval *tv, void *tz);
int lx_nanosleep(const struct lx_timespec *req, struct lx_timespec *rem);
int lx_getrlimit64(int resource, void *r);
int lx_uname(void *u);
int lx_prctl(int option, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5);

/* lso_task.c */
int *lso_errno_location(void);
struct lso_tls_index;
void *lso_tls_get_addr(struct lso_tls_index *ti);

#endif /* LSO_LX_H */

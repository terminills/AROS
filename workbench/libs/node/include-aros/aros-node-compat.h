/*
 * Target-only compatibility declarations for the Node 22 static-core build.
 * Keep these out of host generators and graduate generally useful contracts
 * into libc only after their independent runtime validation.
 */
#ifndef AROS_NODE_COMPAT_H
#define AROS_NODE_COMPAT_H

#include <stddef.h>
#include <signal.h>
#include <string.h>

#ifndef EHOSTDOWN
#define EHOSTDOWN 64
#endif

#ifdef __cplusplus
extern "C" {
#endif

int posix_memalign(void **memptr, size_t alignment, size_t size);

static inline void *aros_node_memrchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;

    while (n--) {
        if (p[n] == (unsigned char)c)
            return (void *)(p + n);
    }
    return NULL;
}
#define memrchr aros_node_memrchr

static inline int aros_node_pthread_sigmask(int how,
                                            const sigset_t *set,
                                            sigset_t *oldset)
{
    (void)how;
    (void)set;
    (void)oldset;
    return 0;
}
#define pthread_sigmask aros_node_pthread_sigmask

#ifdef __cplusplus
}
#endif

#endif

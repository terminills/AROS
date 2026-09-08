/*
 * Force-included compatibility surface for the Node 22 AROS target.
 *
 * Node applies target CPPFLAGS to preprocessed assembly as well as C/C++.
 * Keep every C declaration behind __ASSEMBLER__ so sources such as
 * node_text_start.S see only preprocessor-safe content.
 */
#ifndef AROS_NODE_COMPAT_H
#define AROS_NODE_COMPAT_H

/*
 * Node's zstd GYP target defines this globally. It must be removed before
 * pool.c tests it, not merely inside zstd's later threading.h include.
 */
#ifdef __AROS__
#undef ZSTD_MULTITHREAD
#endif

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <string.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AROS stdc lacks the GNU memrchr extension used by deps/nbytes. */
static inline void *aros_node_memrchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;

    while (n--) {
        if (p[n] == (unsigned char)c)
            return (void *)(p + n);
    }
    return (void *)0;
}
#define memrchr aros_node_memrchr

/*
 * AROS pthread has no per-thread signal masks. Abseil uses this while
 * establishing thread identity, so capability absence is a successful no-op.
 */
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
#endif /* !__ASSEMBLER__ */

#endif /* AROS_NODE_COMPAT_H */

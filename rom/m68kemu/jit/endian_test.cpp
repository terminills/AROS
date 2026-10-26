/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "endian_access.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>

static void require(bool ok) { if (!ok) { std::puts("FAIL endian access"); std::exit(1); } }
template<class T> void check(bool movbe)
{
    unsigned char a[72], b[72];
    uint32_t seed = 123;
    for (unsigned offset = 0; offset < 64; ++offset)
        for (unsigned n = 0; n < 4096; ++n) {
            seed = seed * 1664525U + 1013904223U;
            T v = T(seed);
            memset(a, 0xa5, sizeof a); memcpy(b, a, sizeof a);
            m68kjit::storeBE<T>(a + offset, v);
            require(m68kjit::loadBE<T>(a + offset) == v);
#if defined(__x86_64__) && defined(__GNUC__)
            if (movbe) {
                m68kjit::storeMovbe<T>(b + offset, v);
                require(memcmp(a, b, sizeof a) == 0);
                require(m68kjit::loadMovbe<T>(a + offset) == v);
            }
#endif
        }
}
using Load = uint32_t (*)(const void *);
using Store = void (*)(void *, uint32_t);
__attribute__((noinline)) uint32_t fallbackLoad(const void *p) { return m68kjit::loadBE<uint32_t>(p); }
__attribute__((noinline)) void fallbackStore(void *p, uint32_t v) { m68kjit::storeBE<uint32_t>(p,v); }
#if defined(__x86_64__) && defined(__GNUC__)
__attribute__((noinline, target("movbe"))) uint32_t fastLoad(const void *p) { return m68kjit::loadMovbe<uint32_t>(p); }
__attribute__((noinline, target("movbe"))) void fastStore(void *p, uint32_t v) { m68kjit::storeMovbe<uint32_t>(p,v); }
#endif
__attribute__((noinline)) void bench(const char *name, Load load, Store store)
{
    unsigned char data[4100] = {};
    uint32_t sum = 0;
    auto start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < 5000000; ++i) {
        unsigned pos = (i * 17) & 4095;
        store(data + pos, i);
        sum += load(data + pos);
    }
    double ns = std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/5000000;
    std::printf("synthetic indirect store/load %s ns_pair=%.3f checksum=%08x\n", name, ns, sum);
}
int main(int argc, char **argv)
{
    bool enabled = m68kjit::hasMovbe() && !(argc == 2 && strcmp(argv[1], "--fallback") == 0);
    check<uint16_t>(enabled); check<uint32_t>(enabled);
    std::printf("PASS endian_cases=524288 movbe_detected=%d enabled=%d; aligned/unaligned and store canaries\n", m68kjit::hasMovbe(), enabled);
    bench("fallback", fallbackLoad, fallbackStore);
#if defined(__x86_64__) && defined(__GNUC__)
    if (enabled) bench("MOVBE", fastLoad, fastStore);
#endif
}

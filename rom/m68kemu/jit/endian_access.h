/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#pragma once
#include <stdint.h>
#include <string.h>
#if defined(__x86_64__) && defined(__GNUC__)
#include <cpuid.h>
#endif

namespace m68kjit {
/* Ordinary, already validated guest RAM only. These helpers do not implement
 * address errors, device callbacks, planar mirroring or guest bounds checks.
 * Dispatch once per engine, never CPUID per access. */
inline bool hasMovbe()
{
#if defined(__x86_64__) && defined(__GNUC__)
    unsigned a, b, c, d;
    return __get_cpuid(1, &a, &b, &c, &d) && (c & (1U << 22));
#else
    return false;
#endif
}
template<class T> inline T loadBE(const void *p)
{
    static_assert(sizeof(T) == 2 || sizeof(T) == 4);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    T v;
    memcpy(&v, p, sizeof v);
    if constexpr (sizeof(T) == 2) return __builtin_bswap16(v);
    else return __builtin_bswap32(v);
#else
    const auto *b = static_cast<const uint8_t *>(p);
    T v = 0;
    for (unsigned i = 0; i < sizeof(T); ++i) v = T((v << 8) | b[i]);
    return v;
#endif
}
template<class T> inline void storeBE(void *p, T v)
{
    static_assert(sizeof(T) == 2 || sizeof(T) == 4);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if constexpr (sizeof(T) == 2) v = __builtin_bswap16(v);
    else v = __builtin_bswap32(v);
    memcpy(p, &v, sizeof v);
#else
    auto *b = static_cast<uint8_t *>(p);
    for (unsigned i = 0; i < sizeof(T); ++i) {
        b[sizeof(T) - 1 - i] = uint8_t(v); v >>= 8;
    }
#endif
}
#if defined(__x86_64__) && defined(__GNUC__)
/* Packed aliasable objects make unaligned memory operands valid C++ too. */
template<class T> struct __attribute__((packed, may_alias)) Unaligned { T v; };
template<class T> __attribute__((target("movbe"))) inline T loadMovbe(const void *p)
{
    static_assert(sizeof(T) == 2 || sizeof(T) == 4);
    T v;
    asm("movbe %1,%0" : "=r"(v) : "m"(static_cast<const Unaligned<T> *>(p)->v));
    return v;
}
template<class T> __attribute__((target("movbe"))) inline void storeMovbe(void *p, T v)
{
    static_assert(sizeof(T) == 2 || sizeof(T) == 4);
    asm("movbe %1,%0" : "=m"(static_cast<Unaligned<T> *>(p)->v) : "r"(v));
}
#endif
}

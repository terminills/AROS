/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "moveq_cache.h"
#include <cstring>
#include <type_traits>
#if defined(__AROS__) && defined(__x86_64__) && defined(M68KJIT_NATIVE_EXECMEM)
#include <exec/memory.h>
#include <proto/exec.h>
#define JIT_EXECMEM 1
#else
#define JIT_EXECMEM 0
#endif
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace m68kjit
{
MoveqCache::~MoveqCache()
{
#if defined(__linux__) && defined(__x86_64__)
    for (auto &entry : entries)
        if (entry.page) munmap(entry.page, entry.bytes);
#elif JIT_EXECMEM
    for (auto &entry : entries)
        if (entry.page) FreeMem(entry.page, entry.bytes);
#endif
}

MoveqCache::Code MoveqCache::lookup(uint16_t opcode)
{
    if (!enabled || !supports(opcode)) return nullptr;
#if (defined(__linux__) && defined(__x86_64__)) || JIT_EXECMEM
    /* SysV x86-64 only. No guest addresses are embedded: the key is the
     * actual instruction word, not PC. Separate caches have no shared state.
     * The caller must use the CPU's prefetched IRD, not reread guest memory.
     */
    static_assert(std::is_standard_layout_v<moira::Registers>);
    static_assert(sizeof(bool) == 1);
    unsigned dst = (opcode >> 9) & 7;
    unsigned index = dst * 256 + (opcode & 255);
    auto &entry = entries[index];
    if (entry.tried) return entry.code;
    entry.tried = true;
#if JIT_EXECMEM
    /* Native AROS executable heap, explicitly opted in by the build. This
     * backend cannot promise W^X. Never protect shared heap pages. */
    constexpr size_t size = 64;
    void *page = AllocMem(size, MEMF_ANY | MEMF_EXECUTABLE);
    if (!page) return nullptr;
#else
    long size = sysconf(_SC_PAGESIZE);
    if (size <= 0) return nullptr;
    void *page = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return nullptr;
#endif
    auto *p = static_cast<unsigned char *>(page);
    auto byte = [&](uint8_t value) { *p++ = value; };
    auto word = [&](uint32_t value) { std::memcpy(p, &value, 4); p += 4; };
    auto storeByte = [&](size_t offset, uint8_t value)
    {
        byte(0xC6); byte(0x87); word(static_cast<uint32_t>(offset)); byte(value);
    };
    /* ENDBR64, then mov dword ptr [rdi + displacement], immediate. */
    byte(0xF3); byte(0x0F); byte(0x1E); byte(0xFA);
    byte(0xC7); byte(0x87); word(offsetof(moira::Registers, d) + dst * 4);
    int32_t value = (opcode & 128) ? int32_t(opcode & 255) - 256 : opcode & 255;
    word(static_cast<uint32_t>(value));
    constexpr size_t sr = offsetof(moira::Registers, sr);
    storeByte(sr + offsetof(moira::StatusRegister, n), value < 0);
    storeByte(sr + offsetof(moira::StatusRegister, z), value == 0);
    storeByte(sr + offsetof(moira::StatusRegister, v), 0);
    storeByte(sr + offsetof(moira::StatusRegister, c), 0);
    byte(0xC3);
#if JIT_EXECMEM
    CacheClearE(page, size_t(p - static_cast<unsigned char *>(page)), CACRF_ClearI | CACRF_ClearD);
#else
    /* Linux: failed RX protection is fallback, with no RWX interval. */
    if (mprotect(page, size, PROT_READ | PROT_EXEC) != 0)
    {
        munmap(page, size);
        return nullptr;
    }
#endif
    entry.page = page;
    entry.bytes = size;
    entry.code = reinterpret_cast<Code>(page);
    ++count;
    return entry.code;
#else
    return nullptr;
#endif
}
}

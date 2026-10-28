/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Host fixture for the production OpenLibrary and OldOpenLibrary thunks.
*/
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

using ULONG = uint32_t;
using UWORD = uint16_t;
using IPTR = uintptr_t;
using STRPTR = char *;
static_assert(sizeof(ULONG) == 4, "The guest ABI needs 32-bit registers");

struct TestCPU
{
    ULONG d[8] = {}, a[8] = {};
};

struct M68KEmuContext
{
    const char *name = nullptr;
    ULONG native_result = 0, class_result = 0, loader_result = 0;
    ULONG native_calls = 0, class_calls = 0, loader_calls = 0;
    ULONG class_version = 0xFFFFFFFFu, loader_version = 0xFFFFFFFFu;
    ULONG version_address = 0, cached_version = 0;
    ULONG tail_pc = 0, tail_d0 = 0, tail_a0 = 0, tail_a6 = 0;
    bool trace_calls = false;
    struct { char name[64]; ULONG base, reopen; } m68klibs[16] = {};
    UWORD num_m68klibs = 0;
};

static ULONG M68KEmu_GetD(void *cpu, int n)
{
    return static_cast<TestCPU *>(cpu)->d[n];
}

static ULONG M68KEmu_GetA(void *cpu, int n)
{
    return static_cast<TestCPU *>(cpu)->a[n];
}

static void *m68k_to_host_or_shadow(M68KEmuContext *ctx, ULONG addr)
{
    return addr == 0x100 ? const_cast<char *>(ctx->name) : nullptr;
}

#define THUNK_D(n) M68KEmu_GetD(cpu, n)
#define THUNK_A(n) M68KEmu_GetA(cpu, n)
#define THUNK_PTR(n) m68k_to_host_or_shadow(ctx, M68KEmu_GetA(cpu, n))
#define bug(...) ((void)0)

static ULONG M68KEmu_OpenLibrary(M68KEmuContext *ctx, const char *)
{
    ++ctx->native_calls;
    return ctx->native_result;
}

static ULONG m68k_open_class_library(M68KEmuContext *ctx, const char *, ULONG version)
{
    ++ctx->class_calls;
    ctx->class_version = version;
    return ctx->class_result;
}

static ULONG M68KEmu_LoadM68KLibrary(M68KEmuContext *ctx, const char *, ULONG version)
{
    ++ctx->loader_calls;
    ctx->loader_version = version;
    return ctx->loader_result;
}

static UWORD m68k_read16(M68KEmuContext *ctx, ULONG addr)
{
    ctx->version_address = addr;
    return static_cast<UWORD>(ctx->cached_version);
}

static STRPTR FilePart(STRPTR path)
{
    const char *slash = std::strrchr(path, '/');
    const char *colon = std::strrchr(path, ':');
    const char *end = slash;
    if (colon && (!end || colon > end)) end = colon;
    return const_cast<char *>(end ? end + 1 : path);
}

#include "constants.inc"
#include "functions.inc"

static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
    std::exit(1); } } while (0)

static TestCPU cpu_with(ULONG d0 = 0)
{
    TestCPU cpu;
    cpu.d[0] = d0;
    cpu.a[1] = 0x100;
    cpu.a[6] = 0x600;
    return cpu;
}

static M68KEmuContext context_with(const char *name = "widget.library")
{
    M68KEmuContext ctx;
    ctx.name = name;
    ctx.tail_pc = 0xAAAAAAAAu;
    ctx.tail_d0 = 0xBBBBBBBBu;
    ctx.tail_a0 = 0xCCCCCCCCu;
    ctx.tail_a6 = 0xDDDDDDDDu;
    return ctx;
}

static void cache(M68KEmuContext &ctx, UWORD version)
{
    ctx.num_m68klibs = 1;
    std::snprintf(ctx.m68klibs[0].name, sizeof(ctx.m68klibs[0].name),
                  "%s", "widget.library");
    ctx.m68klibs[0].base = 0x4000;
    ctx.m68klibs[0].reopen = 0x4444;
    ctx.cached_version = version;
}

static void null_and_native()
{
    auto ctx = context_with(nullptr);
    auto cpu = cpu_with(0x12340002u);
    auto saved = cpu;
    CHECK(thunk_exec_OpenLibrary(&ctx, &cpu) == 0);
    CHECK(ctx.native_calls == 0 && ctx.class_calls == 0 && ctx.loader_calls == 0);
    CHECK(std::memcmp(&cpu, &saved, sizeof cpu) == 0);
    CHECK(ctx.tail_a6 == 0xDDDDDDDDu);

    ctx = context_with();
    ctx.native_result = 0x3000;
    CHECK(thunk_exec_OpenLibrary(&ctx, &cpu) == 0x3000);
    CHECK(ctx.native_calls == 1 && ctx.class_calls == 0 && ctx.loader_calls == 0);
    CHECK(ctx.tail_a6 == 0xDDDDDDDDu);
    CHECK(std::memcmp(&cpu, &saved, sizeof cpu) == 0);
}

static void class_versions()
{
    auto ctx = context_with("layout.gadget");
    ctx.class_result = 0x3500;
    auto cpu = cpu_with(0x12340002u);
    auto saved = cpu;
    CHECK(thunk_exec_OpenLibrary(&ctx, &cpu) == 0x3500);
    CHECK(ctx.class_calls == 1 && ctx.class_version == 2);
    CHECK(ctx.loader_calls == 0 && ctx.tail_a6 == 0xDDDDDDDDu);
    CHECK(std::memcmp(&cpu, &saved, sizeof cpu) == 0);

    ctx = context_with("layout.gadget");
    ctx.class_result = 0x3500;
    cpu = cpu_with(0xFFFFFFFFu);
    saved = cpu;
    CHECK(thunk_exec_OldOpenLibrary(&ctx, &cpu) == 0x3500);
    CHECK(ctx.class_calls == 1 && ctx.class_version == 0);
    CHECK(ctx.loader_calls == 0);
    CHECK(std::memcmp(&cpu, &saved, sizeof cpu) == 0);
}

static void cached_versions()
{
    auto ctx = context_with("LIBS68K:WIDGET.LIBRARY");
    cache(ctx, 2);
    auto cpu = cpu_with(0x12340002u);
    auto saved = cpu;
    CHECK(thunk_exec_OpenLibrary(&ctx, &cpu) == 0x4000);
    CHECK(ctx.version_address == 0x4000 + M68K_LIB_VERSION);
    CHECK(ctx.tail_pc == 0x4444 && ctx.tail_d0 == 2);
    CHECK(ctx.tail_a0 == 0 && ctx.tail_a6 == 0x600);
    CHECK(ctx.native_calls == 1 && ctx.class_calls == 1 && ctx.loader_calls == 0);
    CHECK(ctx.class_version == 2);
    CHECK(std::memcmp(&cpu, &saved, sizeof cpu) == 0);

    ctx = context_with();
    cache(ctx, 1);
    cpu = cpu_with(2);
    CHECK(thunk_exec_OpenLibrary(&ctx, &cpu) == 0);
    CHECK(ctx.tail_pc == 0xAAAAAAAAu && ctx.tail_d0 == 0xBBBBBBBBu);
    CHECK(ctx.loader_calls == 0);

    ctx = context_with();
    cache(ctx, 1);
    cpu = cpu_with(0xFFFFFFFFu);
    saved = cpu;
    CHECK(thunk_exec_OldOpenLibrary(&ctx, &cpu) == 0x4000);
    CHECK(ctx.tail_pc == 0x4444 && ctx.tail_d0 == 0);
    CHECK(ctx.tail_a6 == 0x600 && ctx.loader_calls == 0);
    CHECK(std::memcmp(&cpu, &saved, sizeof cpu) == 0);
}

static void disk_versions()
{
    auto ctx = context_with();
    ctx.loader_result = 0x5000;
    auto cpu = cpu_with(0x12340002u);
    auto saved = cpu;
    CHECK(thunk_exec_OpenLibrary(&ctx, &cpu) == 0x5000);
    CHECK(ctx.native_calls == 1 && ctx.class_calls == 1 && ctx.loader_calls == 1);
    CHECK(ctx.class_version == 2 && ctx.loader_version == 2);
    CHECK(ctx.tail_a6 == 0x600);
    CHECK(std::memcmp(&cpu, &saved, sizeof cpu) == 0);

    ctx = context_with();
    ctx.loader_result = 0x5000;
    cpu = cpu_with(0xFFFFFFFFu);
    saved = cpu;
    CHECK(thunk_exec_OpenLibrary(&ctx, &cpu) == 0x5000);
    CHECK(ctx.loader_version == 0xFFFFu);
    CHECK(std::memcmp(&cpu, &saved, sizeof cpu) == 0);

    ctx = context_with();
    ctx.loader_result = 0x5000;
    cpu = cpu_with(0xFFFFFFFFu);
    saved = cpu;
    CHECK(thunk_exec_OldOpenLibrary(&ctx, &cpu) == 0x5000);
    CHECK(ctx.class_version == 0 && ctx.loader_version == 0);
    CHECK(ctx.tail_a6 == 0x600);
    CHECK(std::memcmp(&cpu, &saved, sizeof cpu) == 0);

    ctx = context_with();
    cpu = cpu_with(0xABC00000u);
    CHECK(thunk_exec_OpenLibrary(&ctx, &cpu) == 0);
    CHECK(ctx.class_version == 0 && ctx.loader_version == 0);
    CHECK(ctx.tail_a6 == 0x600);
}

int main()
{
    null_and_native();
    class_versions();
    cached_versions();
    disk_versions();
    std::printf("m68kemu OpenLibrary versions: %u checks, 0 failed\n", checks);
}

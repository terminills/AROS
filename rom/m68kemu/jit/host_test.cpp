/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Host-only differential test. A lowered payload is not a complete CPU step.
 */
#include "moveq_cache.h"
#include "Moira/Moira.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

class Oracle : public moira::Moira
{
public:
    mutable std::vector<uint8_t> mem = std::vector<uint8_t>(65536);
    Oracle() { setModel(moira::Model::M68040); }
    uint8_t read8(uint32_t a) const override { return mem[a & 65535]; }
    uint16_t read16(uint32_t a) const override { return (read8(a) << 8) | read8(a + 1); }
    void write8(uint32_t a, uint8_t v) const override { mem[a & 65535] = v; }
    void write16(uint32_t a, uint16_t v) const override { write8(a, v >> 8); write8(a + 1, v); }
    void sync(int cycles) override { clock += cycles; }
    void prepare(uint16_t opcode, unsigned ccr, std::mt19937 &rng)
    {
        reg = {};
        for (auto &r : reg.r) r = rng();
        reg.pc = reg.pc0 = 0x1000;
        reg.sr.s = true;
        reg.sr.ipl = 7;
        reg.sr.x = (ccr & 16) != 0; reg.sr.n = (ccr & 8) != 0;
        reg.sr.z = (ccr & 4) != 0; reg.sr.v = (ccr & 2) != 0;
        reg.sr.c = (ccr & 1) != 0;
        flags = 0; ipl = 0; clock = 0;
        queue.ird = opcode; queue.irc = 0x4E71;
    }
    bool step(m68kjit::MoveqCache &cache)
    {
        /* Host harness only: read16 is nonthrowing ordinary RAM. Never use
         * this adapter for AROS device memory or arbitrary client callbacks.
         * Nonzero flags keep IRQ, tracing, stop, debug and fault paths in Moira.
         */
        if (flags || cpuModel != moira::Model::M68040 || reg.pc != reg.pc0 ||
            reg.pc > mem.size() - 6)
        {
            execute(); return false;
        }
        auto code = cache.lookup(queue.ird);
        if (!code) { execute(); return false; }
        reg.pc += 2;
        cp = 0;
        code(&reg);
        reg.pc0 = reg.pc;
        queue.ird = queue.irc;
        if constexpr (MOIRA_EMULATE_FC) fcl = moira::FC::USER_PROG;
        reg.ipl = ipl;
        queue.irc = read16(reg.pc + 2);
        readBuffer = queue.irc;
        sync(2);
        return true;
    }
    struct Hidden { moira::PrefetchQueue queue; int flags; int cp; uint8_t fcl; uint16_t readBuffer; int64_t clock; };
    Hidden hidden() const { return {queue, flags, cp, fcl, readBuffer, clock}; }
    void loopProgram()
    {
        write16(0x1000, 0x70A5); write16(0x1002, 0x60FC);
        reg.pc = reg.pc0 = 0x1000; queue.ird = 0x70A5; queue.irc = 0x60FC;
    }
    moira::Registers snapshot() const { return reg; }
    void rewind(uint16_t opcode) { reg.pc = reg.pc0 = 0x1000; queue.ird = opcode; queue.irc = 0x4E71; }
};
static void require(bool ok, const char *why)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
int main()
{
    using Clock = std::chrono::steady_clock;
    Oracle oracle, candidate;
    m68kjit::MoveqCache cache, disabled(false), second;
    std::mt19937 rng(0x68040);
    unsigned cases = 0, rejected = 0;
    std::mt19937 candidateRng(0x68040);
    double compileNs = 0;
    for (unsigned op = 0; op < 65536; ++op)
    {
        if (!cache.supports(op))
        {
            require(cache.lookup(op) == nullptr, "unsupported opcode must fall back");
            ++rejected; continue;
        }
        auto start = Clock::now(); auto code = cache.lookup(op);
        compileNs += std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        require(code != nullptr, "host code allocation");
        require(disabled.lookup(op) == nullptr, "disabled engine fallback");
        require(cache.lookup(op) == code, "stable immutable cache hit");
        for (unsigned ccr = 0; ccr < 32; ++ccr)
        {
            oracle.prepare(op, ccr, rng);
            candidate.prepare(op, ccr, candidateRng);
            auto initial = oracle.snapshot(); auto actual = initial;
            code(&actual);
            oracle.execute(); auto expected = oracle.snapshot();
            require(candidate.step(cache), "supported host step dispatched");
            auto fullActual = candidate.snapshot();
            require(std::memcmp(&expected, &fullActual, sizeof expected) == 0, "full host step register mismatch");
            auto h = oracle.hidden(), j = candidate.hidden();
            require(h.queue.ird == j.queue.ird && h.queue.irc == j.queue.irc && h.flags == j.flags &&
                    h.cp == j.cp && h.fcl == j.fcl && h.readBuffer == j.readBuffer && h.clock == j.clock,
                    "full host step hidden-state mismatch");
            /* Payload owns register result/CCR only; PC, prefetch, clocks,
             * faults and all integration hooks remain interpreter work.
             */
            expected.pc = initial.pc; expected.pc0 = initial.pc0;
            require(std::memcmp(&expected, &actual, sizeof actual) == 0, "MOVEQ state differs from Moira");
            ++cases;
        }
    }
    unsigned fallbackCases = 0;
    for (unsigned scenario = 0; scenario < 8; ++scenario)
    {
        std::mt19937 a(1234), b(1234);
        uint16_t op = scenario == 0 ? 0x4E71 : scenario == 1 ? 0xA000 :
                      scenario == 2 ? 0x4E73 : scenario == 3 ? 0xF000 : 0x7001;
        oracle.prepare(op, 31, a); candidate.prepare(op, 31, b);
        if (scenario == 4) { oracle.setSR(0xA000); candidate.setSR(0xA000); }
        if (scenario == 5)
        {
            oracle.setSR(0x2000); candidate.setSR(0x2000);
            oracle.setIPL(3); candidate.setIPL(3);
        }
        if (scenario == 6)
        {
            oracle.setModel(moira::Model::M68000); candidate.setModel(moira::Model::M68000);
        }
        oracle.execute();
        require(!candidate.step(scenario == 7 ? disabled : cache), "guarded path must fall back");
        auto expected = oracle.snapshot(), actual = candidate.snapshot();
        require(std::memcmp(&expected, &actual, sizeof actual) == 0, "fallback CPU state");
        require(oracle.mem == candidate.mem && oracle.getClock() == candidate.getClock(), "fallback memory or cycles");
        oracle.setModel(moira::Model::M68040); candidate.setModel(moira::Model::M68040);
        ++fallbackCases;
    }
    std::printf("PASS fallback_scenarios=%u (NOP A-line RTE F-line trace IRQ model disabled)\n", fallbackCases);
    /* Guest writes do not retroactively replace the prefetched word. Compare
     * both the retained IRD and a later refill after modifying the same PC.
     */
    std::mt19937 smcA(42), smcB(42);
    oracle.prepare(0x7001, 31, smcA); candidate.prepare(0x7001, 31, smcB);
    oracle.write16(0x1000, 0x70FF); candidate.write16(0x1000, 0x70FF);
    oracle.execute(); require(candidate.step(cache), "prefetched MOVEQ dispatch");
    require(candidate.getD(0) == 1 && candidate.getD(0) == oracle.getD(0), "prefetch survives self modification");
    oracle.rewind(oracle.read16(0x1000)); candidate.rewind(candidate.read16(0x1000));
    oracle.execute(); require(candidate.step(cache), "modified MOVEQ dispatch");
    require(candidate.getD(0) == 0xFFFFFFFF && candidate.getD(0) == oracle.getD(0), "modified instruction after refill");
    std::puts("PASS same-PC mutation with retained prefetch and refill");
    auto independent = second.lookup(0x7080);
    require(independent && independent != cache.lookup(0x7080), "per-instance cache isolation");
    auto state = oracle.snapshot(); independent(&state);
    require(state.d[0] == 0xFFFFFF80, "second instance result");
    /* A changed instruction at the same PC selects a new opcode entry. */
    cache.lookup(0x7001)(&state); require(state.d[0] == 1, "changed opcode selection");
    cache.lookup(0x70FF)(&state); require(state.d[0] == 0xFFFFFFFF, "changed opcode selection 2");
    constexpr unsigned iterations = 5000000;
    auto code = cache.lookup(0x70A5);
    for (unsigned i = 0; i < 10000; ++i) code(&state);
    auto start = Clock::now();
    for (unsigned i = 0; i < iterations; ++i) code(&state);
    double payload = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / iterations;
    oracle.prepare(0x70A5, 31, rng);
    start = Clock::now();
    for (unsigned i = 0; i < iterations; ++i) { oracle.rewind(0x70A5); oracle.execute(); }
    double full = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / iterations;
    oracle.prepare(0x70A5, 31, rng); candidate.prepare(0x70A5, 31, candidateRng);
    oracle.loopProgram(); candidate.loopProgram();
    for (unsigned i = 0; i < 10000; ++i) { oracle.execute(); candidate.step(cache); }
    start = Clock::now();
    for (unsigned i = 0; i < iterations; ++i) oracle.execute();
    double matchedInterpreter = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / iterations;
    start = Clock::now();
    unsigned compiledSteps = 0;
    for (unsigned i = 0; i < iterations; ++i) compiledSteps += candidate.step(cache);
    double matchedJit = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / iterations;
    require(oracle.getD(0) == candidate.getD(0) && oracle.getPC0() == candidate.getPC0() &&
            oracle.getClock() == candidate.getClock(), "mixed MOVEQ/BRA loop mismatch");
    require(compiledSteps == iterations / 2, "mixed loop fallback count");
    std::printf("host_synthetic_moveq_bra interpreter_ns=%.3f jit_ns=%.3f ratio=%.3f compiled_steps=%u fallback_steps=%u\n",
                matchedInterpreter, matchedJit, matchedInterpreter / matchedJit, compiledSteps, iterations - compiledSteps);
    std::printf("PASS differential_cases=%u unsupported_fallbacks=%u compiled=%zu\n", cases, rejected, cache.compiled());
    std::printf("compile_all_ms=%.3f warmup_calls=10000 payload_ns=%.3f interpreter_step_with_rewind_ns=%.3f\n", compileNs / 1e6, payload, full);
    std::puts("NOT a speedup comparison: payload excludes prefetch, timing, faults and integration hooks.");
}

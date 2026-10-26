/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "moveq_cache.h"
#include "endian_access.h"
#include <cstdio>
#include <cstring>

int main()
{
    bool movbe = m68kjit::hasMovbe();
    unsigned char ram[72], reference[72];
    for (unsigned offset = 0; offset < 64; ++offset)
        for (unsigned n = 0; n < 4096; ++n) {
            uint32_t value = n * 1664525U + offset;
            std::memset(ram, 0xa5, sizeof ram);
            std::memcpy(reference, ram, sizeof ram);
            m68kjit::storeBE<uint32_t>(reference + offset, value);
            if (movbe) m68kjit::storeMovbe<uint32_t>(ram + offset, value);
            else m68kjit::storeBE<uint32_t>(ram + offset, value);
            uint32_t read = movbe ? m68kjit::loadMovbe<uint32_t>(ram + offset)
                                  : m68kjit::loadBE<uint32_t>(ram + offset);
            if (read != value || std::memcmp(ram, reference, sizeof ram)) {
                std::puts("NATIVE-PAYLOAD FAIL endian access"); return 20;
            }
        }
    m68kjit::MoveqCache cache, disabled(false);
    unsigned cases = 0;
    for (unsigned op = 0; op < 65536; ++op) {
        auto code = cache.lookup(op);
        if (!cache.supports(op)) {
            if (code) return 20;
            continue;
        }
        if (!code || disabled.lookup(op)) {
            std::puts("NATIVE-PAYLOAD FAIL allocation or disabled fallback");
            return 20;
        }
        for (unsigned ccr = 0; ccr < 32; ++ccr) {
            moira::Registers actual{}, expected{};
            for (auto &reg : actual.r) reg = 0x12345678;
            actual.pc = 0x1000; actual.pc0 = 0x998;
            actual.sr.x = (ccr & 16) != 0; actual.sr.n = (ccr & 8) != 0;
            actual.sr.z = (ccr & 4) != 0; actual.sr.v = (ccr & 2) != 0;
            actual.sr.c = (ccr & 1) != 0;
            std::memcpy(&expected, &actual, sizeof actual);
            int32_t value = op & 128 ? int32_t(op & 255) - 256 : op & 255;
            expected.d[(op >> 9) & 7] = uint32_t(value);
            expected.sr.n = value < 0; expected.sr.z = value == 0;
            expected.sr.v = expected.sr.c = false;
            code(&actual);
            if (std::memcmp(&expected, &actual, sizeof actual)) {
                std::printf("NATIVE-PAYLOAD FAIL op=%04x ccr=%u\n",op,ccr);
                return 20;
            }
            ++cases;
        }
    }
    std::printf("PASS native MOVEQ=%u endian32=262144 MOVBE=%d (no W^X)\n",cases,movbe);
    return 0;
}

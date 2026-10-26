/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Experimental host-only MOVEQ lowering. Not wired into m68kemu.library.
 */
#pragma once
#include "Moira/MoiraTypes.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace m68kjit
{
class MoveqCache
{
public:
    using Code = void (*)(moira::Registers *);
    explicit MoveqCache(bool enable = true) : enabled(enable) {}
    ~MoveqCache();
    MoveqCache(const MoveqCache &) = delete;
    MoveqCache &operator=(const MoveqCache &) = delete;
    Code lookup(uint16_t opcode);
    size_t compiled() const { return count; }
    static bool supports(uint16_t opcode) { return (opcode & 0xF100) == 0x7000; }
private:
    struct Entry { void *page = nullptr; size_t bytes = 0; Code code = nullptr; bool tried = false; };
    std::array<Entry, 2048> entries{};
    bool enabled;
    size_t count = 0;
};
}

/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * Include inside a Moira subclass. The caller must establish that the next
 * prefetch at PC+4 is bounded, nonthrowing ordinary RAM with no device hooks.
 * false leaves CPU state untouched and requires the caller to execute Moira.
 */
bool tryMoveq(m68kjit::MoveqCache &cache, bool ordinaryRam)
{
    using moira::Instr;
    constexpr Instr I = Instr::MOVEQ;
    if (!ordinaryRam || flags || reg.pc != reg.pc0 || (reg.pc & 1) ||
        (cpuModel != moira::Model::M68040 && cpuModel != moira::Model::M68LC040) ||
        MOIRA_WILL_EXECUTE || MOIRA_DID_EXECUTE)
        return false;
    auto code = cache.lookup(queue.ird);
    if (!code) return false;
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

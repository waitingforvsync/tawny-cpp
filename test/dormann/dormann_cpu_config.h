#pragma once

#include <cstdint>
#include <memory>

#include "emulator/m6502.h"

namespace tawny::dormann {

// M6502Config for running the Klaus Dormann functional test.
//
// Owns a zero-initialised 64K RAM buffer. All accesses are trivial,
// single-cycle direct RAM hits (no MMIO).
//
// Trap detection lives in `access_cost_opcode`: when the CPU is about to
// fetch an opcode at the same address it fetched last, we set stop=true —
// that pattern catches every trap the test uses (JMP *, BNE *, BXX *, …).
// The test code inspects cpu.pc on return to tell success (pc == success_addr)
// from a failure trap (any other address).
//
// Note the split: `read_opcode` does the state update (last_opcode_addr);
// `access_cost_opcode` just compares. That keeps access_cost_* pure /
// idempotent, as the concept's contract requires.
struct DormannCpuConfig {
    std::unique_ptr<std::uint8_t[]> mem{std::make_unique<std::uint8_t[]>(65536)};

    // Sentinel outside the 16-bit range so the very first opcode fetch can't
    // match. std::uint32_t gives us room.
    std::uint32_t last_opcode_addr{0x10000};

    auto read_opcode(std::uint16_t addr) -> std::uint8_t
    {
        last_opcode_addr = addr;
        return mem[addr];
    }

    auto read_zp(std::uint8_t addr) -> std::uint8_t       { return mem[addr]; }
    auto read_stack(std::uint8_t sp) -> std::uint8_t      { return mem[0x0100 | sp]; }
    auto read(std::uint16_t addr) -> std::uint8_t         { return mem[addr]; }
    auto read_vector(std::uint16_t addr) -> std::uint8_t  { return mem[addr]; }

    auto write_zp(std::uint8_t addr, std::uint8_t val) -> void    { mem[addr] = val; }
    auto write_stack(std::uint8_t sp, std::uint8_t val) -> void   { mem[0x0100 | sp] = val; }
    auto write(std::uint16_t addr, std::uint8_t val) -> void      { mem[addr] = val; }

    auto access_cost_opcode(std::uint16_t addr) const -> AccessCost
    {
        return {1, addr == last_opcode_addr};
    }
    auto access_cost_zp(std::uint8_t) const -> AccessCost       { return {1, false}; }
    auto access_cost_stack(std::uint8_t) const -> AccessCost    { return {1, false}; }
    auto access_cost_vector(std::uint16_t) const -> AccessCost  { return {1, false}; }
    auto access_cost(std::uint16_t) const -> AccessCost         { return {1, false}; }

    // Dormann's functional + decimal tests don't fire interrupts. The
    // interrupt test does, but it goes through a derived config that
    // overrides these.
    auto irq_asserted_since() const -> Cycle { return interrupt_never; }
    auto nmi_edge_at()        const -> Cycle { return interrupt_never; }
    auto consume_nmi()              -> void  {}
};

}  // namespace tawny::dormann

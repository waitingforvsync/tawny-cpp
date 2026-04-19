#pragma once

#include <cstdint>
#include <memory>

namespace tawny::dormann {

// M6502Config for running the Klaus Dormann functional test.
//
// Owns a zero-initialised 64K RAM buffer. All accesses are trivial,
// single-cycle direct RAM hits (no MMIO).
//
// The one non-trivial behaviour is in read_opcode: we watch for two
// consecutive opcode fetches at the same PC, which is the signature of every
// trap the test uses ("JMP *", "BNE *", etc.). When that happens we raise
// stop_flag, which ends the timeslice after the trap instruction completes.
// The test code then inspects the CPU's PC to tell success (= success_addr)
// from a failure trap (any other address).
struct DormannCpuConfig {
    std::unique_ptr<std::uint8_t[]> mem{std::make_unique<std::uint8_t[]>(65536)};

    // Sentinel outside the 16-bit range so the very first opcode fetch can
    // never match. std::uint32_t gives us room for that sentinel.
    std::uint32_t last_opcode_addr{0x10000};
    bool stop_flag{false};

    auto read_opcode(std::uint16_t addr) -> std::uint8_t
    {
        if (addr == last_opcode_addr) {
            stop_flag = true;
        }
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

    auto access_cost_opcode(std::uint16_t) const -> unsigned  { return 1; }
    auto access_cost_zp(std::uint8_t) const -> unsigned       { return 1; }
    auto access_cost_stack(std::uint8_t) const -> unsigned    { return 1; }
    auto access_cost_vector(std::uint16_t) const -> unsigned  { return 1; }
    auto access_cost(std::uint16_t) const -> unsigned         { return 1; }

    auto stop_requested() const -> bool { return stop_flag; }
};

}  // namespace tawny::dormann

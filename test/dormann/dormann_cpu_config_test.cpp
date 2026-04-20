#include <doctest/doctest.h>

#include "emulator/m6502.h"
#include "dormann/dormann_cpu_config.h"

// Compile-time check that DormannCpuConfig satisfies the M6502Config concept.
static_assert(tawny::M6502Config<tawny::dormann::DormannCpuConfig>);

TEST_CASE("DormannCpuConfig: access_cost_opcode is pure; read_opcode updates state") {
    tawny::dormann::DormannCpuConfig cfg{};
    cfg.mem[0x0400] = 0x4C;  // contents don't matter, but make them realistic

    // First check at $0400: last_opcode_addr is the sentinel — no stop.
    CHECK(cfg.access_cost_opcode(0x0400).cost == 1);
    CHECK_FALSE(cfg.access_cost_opcode(0x0400).stop);

    // Multiple access_cost_opcode calls are idempotent — state is not mutated.
    CHECK_FALSE(cfg.access_cost_opcode(0x0400).stop);

    // Only read_opcode updates last_opcode_addr.
    (void)cfg.read_opcode(0x0400);

    // Now a subsequent access_cost_opcode at the same address sees the match.
    CHECK(cfg.access_cost_opcode(0x0400).stop);
    CHECK(cfg.access_cost_opcode(0x0400).cost == 1);

    // A different address: no stop.
    CHECK_FALSE(cfg.access_cost_opcode(0x0401).stop);
}

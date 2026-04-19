#include <doctest/doctest.h>

#include "emulator/m6502.h"
#include "dormann/dormann_cpu_config.h"

// Compile-time check that DormannCpuConfig satisfies the M6502Config concept.
static_assert(tawny::M6502Config<tawny::dormann::DormannCpuConfig>);

TEST_CASE("DormannCpuConfig detects JMP * trap") {
    tawny::dormann::DormannCpuConfig cfg{};

    // Place JMP $0400 at $0400. Fetching opcode at $0400 twice should raise stop.
    cfg.mem[0x0400] = 0x4C;
    cfg.mem[0x0401] = 0x00;
    cfg.mem[0x0402] = 0x04;

    CHECK_FALSE(cfg.stop_requested());
    cfg.read_opcode(0x0400);
    CHECK_FALSE(cfg.stop_requested());  // first fetch — last_opcode_addr was sentinel
    cfg.read_opcode(0x0400);
    CHECK(cfg.stop_requested());        // second identical fetch — trap detected
}

TEST_CASE("M6502 constructs from moved-in DormannCpuConfig") {
    tawny::M6502 cpu{tawny::dormann::DormannCpuConfig{}};

    cpu.config.mem[0x1234] = 0x42;
    CHECK(cpu.config.read(0x1234) == 0x42);
}

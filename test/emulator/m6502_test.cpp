#include <doctest/doctest.h>

#include "emulator/m6502.h"
#include "dormann/dormann_cpu_config.h"

namespace {

// Convenience: build a CPU with the reset vector pointing at 0x0400.
auto make_cpu() -> tawny::M6502<tawny::dormann::DormannCpuConfig>
{
    tawny::dormann::DormannCpuConfig cfg{};
    cfg.mem[0xFFFC] = 0x00;
    cfg.mem[0xFFFD] = 0x04;
    return tawny::M6502{std::move(cfg)};
}

}  // namespace

static_assert(tawny::M6502Config<tawny::dormann::DormannCpuConfig>);

// 1. Reset seeds vector.
TEST_CASE("M6502: constructor seeds the reset-vector fetch") {
    auto cpu = make_cpu();

    CHECK(cpu.pending_addr == 0xFFFC);
    CHECK(cpu.tstate == tawny::detail::ResetStep0);
    CHECK(cpu.brk_flags == tawny::BrkFlags::Reset);
    CHECK(cpu.cycle == 1);  // cost of read_vector($FFFC)
    CHECK(cpu.s == 0xFD);
    CHECK((cpu.p & tawny::flag::I) != 0);
    CHECK((cpu.p & tawny::flag::U) != 0);
}

// 2. Reset sequence end-to-end.
TEST_CASE("M6502: reset sequence jumps to the vector target") {
    auto cpu = make_cpu();

    // run_until(3): runs ResetStep0 (c→2) and ResetStep1 (c→3). The fall-through
    // to ResetOpcodeFetch is deferred by the horizon.
    auto returned = cpu.run_until(3);

    CHECK(returned == 3);
    CHECK(cpu.cycle == 3);
    CHECK(cpu.pc == 0x0400);
    CHECK(cpu.tstate == tawny::detail::ResetOpcodeFetch);
    CHECK(cpu.brk_flags == tawny::BrkFlags::None);
    CHECK(cpu.pending_addr == 0x0400);
}

// 3. NOP cycles.
TEST_CASE("M6502: 100 NOPs add exactly 200 cycles") {
    auto cpu = make_cpu();
    for (unsigned i = 0; i < 100; ++i) {
        cpu.config.mem[0x0400 + i] = 0xEA;  // NOP
    }
    // Horizon 203: reset (3 cycles) + NOP #1 through NOP #100 step 0 (199 cycles
    // interleaved with 99 inter-NOP opcode fetches + step 0 of NOP #100). The
    // tail of entry 101 stops exactly at NOP #100 step 0 completion.
    auto returned = cpu.run_until(203);

    CHECK(returned == 203);
    CHECK(cpu.cycle == 203);
    CHECK(cpu.pc == 0x0464);   // 0x0400 + 100
}

// 4. LDA # + STA zp stores a byte in ZP; flags set correctly.
TEST_CASE("M6502: LDA #imm + STA zp writes to memory and updates flags") {
    auto cpu = make_cpu();
    cpu.config.mem[0x0400] = 0xA9;  // LDA #
    cpu.config.mem[0x0401] = 0x42;
    cpu.config.mem[0x0402] = 0x85;  // STA zp
    cpu.config.mem[0x0403] = 0x30;
    cpu.config.mem[0x0404] = 0xEA;  // NOP (sentinel)

    // Reset (3) + LDA# (2) + STA zp (3) = 8 cycles → cycle = 9.
    cpu.run_until(9);

    CHECK(cpu.a == 0x42);
    CHECK(cpu.config.mem[0x0030] == 0x42);
    CHECK((cpu.p & tawny::flag::Z) == 0);
    CHECK((cpu.p & tawny::flag::N) == 0);
    CHECK(cpu.pc == 0x0405);
}

// 5. ADC binary: spot-check the interesting flag combinations.
TEST_CASE("M6502: ADC # binary flag cases") {
    SUBCASE("0x10 + 0x20, C=0 → 0x30, no flags") {
        auto cpu = make_cpu();
        cpu.config.mem[0x0400] = 0xA9; cpu.config.mem[0x0401] = 0x10;  // LDA #$10
        cpu.config.mem[0x0402] = 0x69; cpu.config.mem[0x0403] = 0x20;  // ADC #$20
        cpu.config.mem[0x0404] = 0xEA;

        cpu.run_until(9);
        CHECK(cpu.a == 0x30);
        CHECK((cpu.p & tawny::flag::C) == 0);
        CHECK((cpu.p & tawny::flag::V) == 0);
        CHECK((cpu.p & tawny::flag::Z) == 0);
        CHECK((cpu.p & tawny::flag::N) == 0);
    }
    SUBCASE("0xFF + 0x01 → 0x00 with C=1, Z=1") {
        auto cpu = make_cpu();
        cpu.config.mem[0x0400] = 0xA9; cpu.config.mem[0x0401] = 0xFF;
        cpu.config.mem[0x0402] = 0x69; cpu.config.mem[0x0403] = 0x01;
        cpu.config.mem[0x0404] = 0xEA;

        cpu.run_until(9);
        CHECK(cpu.a == 0x00);
        CHECK((cpu.p & tawny::flag::C) != 0);
        CHECK((cpu.p & tawny::flag::Z) != 0);
        CHECK((cpu.p & tawny::flag::N) == 0);
    }
    SUBCASE("0x7F + 0x01 → 0x80 with V=1, N=1") {
        auto cpu = make_cpu();
        cpu.config.mem[0x0400] = 0xA9; cpu.config.mem[0x0401] = 0x7F;
        cpu.config.mem[0x0402] = 0x69; cpu.config.mem[0x0403] = 0x01;
        cpu.config.mem[0x0404] = 0xEA;

        cpu.run_until(9);
        CHECK(cpu.a == 0x80);
        CHECK((cpu.p & tawny::flag::V) != 0);
        CHECK((cpu.p & tawny::flag::N) != 0);
        CHECK((cpu.p & tawny::flag::C) == 0);
    }
}

// 6. JMP abs.
TEST_CASE("M6502: JMP abs sets pc and takes 3 cycles") {
    auto cpu = make_cpu();
    cpu.config.mem[0x0400] = 0x4C;  // JMP abs
    cpu.config.mem[0x0401] = 0x00;
    cpu.config.mem[0x0402] = 0x05;  // → $0500
    cpu.config.mem[0x0500] = 0xEA;  // NOP at target

    // Reset (3) + JMP (3) + next opcode fetch (1) = 7 cycles → cycle = 7... wait
    // NEW: reset absorbs opcode fetch, so reset (3, incl. JMP fetch) + JMP step 0
    //      + step 1 + JMP's fetch_opcode (reads NOP at target) = 3 + 3 = 6 cycles
    //      → cycle = 7.
    cpu.run_until(7);

    CHECK(cpu.pc == 0x0501);        // fetch_opcode at 0x0500 advanced pc by 1
    CHECK(cpu.cycle == 7);
}

// 7-9. BNE timing: not-taken (2 cyc), taken no-cross (3 cyc), taken cross (4 cyc).
TEST_CASE("M6502: BNE not taken — 2 cycles") {
    auto cpu = make_cpu();
    cpu.config.mem[0x0400] = 0xA9; cpu.config.mem[0x0401] = 0x00;  // LDA #$00 → Z=1
    cpu.config.mem[0x0402] = 0xD0; cpu.config.mem[0x0403] = 0x7F;  // BNE +$7F (not taken)
    cpu.config.mem[0x0404] = 0xEA;

    // reset (3) + LDA# (2) + BNE not-taken (2) + NOP opcode fetch (1) = 8; cycle=9.
    // Under the new dispatch the "next opcode fetch" of BNE reads the NOP at
    // 0x0404 as its last cycle, advancing pc to 0x0405.
    cpu.run_until(8);

    CHECK(cpu.cycle == 8);
    CHECK(cpu.pc == 0x0405);
}

TEST_CASE("M6502: BNE taken, no page cross — 3 cycles") {
    auto cpu = make_cpu();
    cpu.config.mem[0x0400] = 0xA9; cpu.config.mem[0x0401] = 0x01;  // LDA #$01 → Z=0
    cpu.config.mem[0x0402] = 0xD0; cpu.config.mem[0x0403] = 0x05;  // BNE +5
    cpu.config.mem[0x0409] = 0xEA;

    // reset (3) + LDA# (2) + BNE taken (3) + NOP opcode fetch (1) = 9; cycle=9.
    cpu.run_until(9);

    CHECK(cpu.cycle == 9);
    CHECK(cpu.pc == 0x040A);
}

TEST_CASE("M6502: BNE taken with page cross — 4 cycles") {
    auto cpu = make_cpu();
    cpu.config.mem[0x0400] = 0xA9; cpu.config.mem[0x0401] = 0x01;  // LDA #$01 → Z=0
    for (std::uint16_t i = 0x0402; i < 0x04FD; ++i) {
        cpu.config.mem[i] = 0xEA;  // NOP pad
    }
    cpu.config.mem[0x04FD] = 0xD0; cpu.config.mem[0x04FE] = 0x05;  // BNE +5 → 0x0504
    cpu.config.mem[0x0504] = 0xEA;

    // reset (3) + LDA# (2) + NOPs 0x0402..0x04FC = 251 NOPs × 2 = 502 cycles
    //        + BNE taken cross (4) + NOP fetch (1) = 512; cycle = 512.
    cpu.run_until(512);

    CHECK(cpu.cycle == 512);
    CHECK(cpu.pc == 0x0505);
}

// 10. Horizon mid-instruction — run_until stops mid-instruction and a second
//     call completes it exactly.
TEST_CASE("M6502: horizon mid-instruction saves state and resumes exactly") {
    auto cpu = make_cpu();
    cpu.config.mem[0x0400] = 0xAD;  // LDA abs — 4 cycles
    cpu.config.mem[0x0401] = 0x34;
    cpu.config.mem[0x0402] = 0x12;
    cpu.config.mem[0x1234] = 0x77;
    cpu.config.mem[0x0403] = 0xEA;

    // reset (3, incl. LDA abs opcode fetch) + LDA abs step 0 (1) = 4 cycles.
    // cycle = 5. LDA abs is not done yet (steps 1, 2, fetch_opcode remain).
    auto first = cpu.run_until(5);

    CHECK(first == 5);
    CHECK(cpu.cycle == 5);
    CHECK(cpu.a == 0);
    CHECK(cpu.pc == 0x0402);

    // Resume: LDA abs step 1 (read addr hi) + step 2 (read target) = 2 cycles;
    // cycle = 7. a is now loaded; the NOP opcode fetch hasn't run yet.
    auto second = cpu.run_until(7);

    CHECK(second == 7);
    CHECK(cpu.cycle == 7);
    CHECK(cpu.a == 0x77);
    CHECK(cpu.pc == 0x0403);
}

// 11. Dormann trap: JMP $0400 at $0400; the second opcode fetch at the same
//     address hits stop=true on access_cost_opcode.
TEST_CASE("M6502: JMP-to-self trap stops the timeslice via access_cost stop") {
    auto cpu = make_cpu();
    cpu.config.mem[0x0400] = 0x4C;
    cpu.config.mem[0x0401] = 0x00;
    cpu.config.mem[0x0402] = 0x04;  // JMP $0400

    auto returned = cpu.run_until(1ull << 32);

    // Cycles: reset (3) + JMP step 0 (1) + JMP step 1 (1; sets pc to $0400 and
    // queries access_cost_opcode(0x0400) which matches last_opcode_addr → stop).
    // current was incremented by the cost before the guard, so cycle = 6.
    CHECK(returned == 6);
    CHECK(cpu.cycle == 6);
    CHECK(cpu.pc == 0x0400);
    CHECK(cpu.pending_addr == 0x0400);
    CHECK(cpu.tstate == ((0x4C << 3) | 2));  // JMP's own fetch_opcode step
}

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

// Cycle count for the reset sequence (BRK microcode with BrkFlags::Reset +
// first opcode fetch). Used as a base offset for every test that runs past
// reset.
constexpr tawny::Cycle kResetCycles = 7;  // 7 BRK steps; final step reads handler opcode

}  // namespace

static_assert(tawny::M6502Config<tawny::dormann::DormannCpuConfig>);

// 1. Reset seeds BRK's microcode.
TEST_CASE("M6502: constructor seeds BRK step 0 with BrkFlags::Reset") {
    auto cpu = make_cpu();

    CHECK(cpu.tstate == 0);                                // BRK (opcode 0x00) step 0
    CHECK(cpu.brk_flags == tawny::BrkFlags::Reset);
    CHECK(cpu.cycle == 1);                                 // cost of the first phi2
    CHECK(cpu.pc == 0);
    CHECK(cpu.s == 0);                                     // BRK's dummy pushes take this to 0xFD
}

// 2. Reset runs the BRK-Reset microcode and jumps to the vector target.
TEST_CASE("M6502: reset runs BRK microcode with BrkFlags::Reset") {
    auto cpu = make_cpu();

    // Run 6 of the 7 cycles: BRK steps 0-5 (through vector-hi latch + PC set),
    // but stop just before step 6 (the opcode fetch of the handler).
    auto returned = cpu.run_until(7);

    CHECK(returned == 7);
    CHECK(cpu.cycle == 7);
    CHECK(cpu.pc == 0x0400);                               // vector target
    CHECK(cpu.tstate == ((0x00 << 3) | 6));                // BRK step 6 pending
    CHECK(cpu.brk_flags == tawny::BrkFlags::None);         // cleared in step 5
    CHECK(cpu.pending_addr == 0x0400);
    CHECK(cpu.s == 0xFD);                                  // 0 - 3 decrements
    CHECK((cpu.p & tawny::flag::I) != 0);                  // I set in step 3
}

// 3. NOP cycles — 100 NOPs still add 200 cycles; the absolute horizon now
//    includes the 7-cycle reset (= 1 initial + 7 = 8 after reset, + 199 cycles
//    up to the end of NOP #100 step 0 = 207).
TEST_CASE("M6502: 100 NOPs add exactly 200 cycles") {
    auto cpu = make_cpu();
    for (unsigned i = 0; i < 100; ++i) {
        cpu.config.mem[0x0400 + i] = 0xEA;
    }
    auto returned = cpu.run_until(kResetCycles + 200);  // = 207

    CHECK(returned == 207);
    CHECK(cpu.cycle == 207);
    CHECK(cpu.pc == 0x0464);                            // 0x0400 + 100
}

// 4. LDA # + STA zp.
TEST_CASE("M6502: LDA #imm + STA zp writes to memory and updates flags") {
    auto cpu = make_cpu();
    cpu.config.mem[0x0400] = 0xA9; cpu.config.mem[0x0401] = 0x42;  // LDA #$42
    cpu.config.mem[0x0402] = 0x85; cpu.config.mem[0x0403] = 0x30;  // STA $30
    cpu.config.mem[0x0404] = 0xEA;

    // reset (7) + LDA# (2) + STA zp (3) + opcode fetch of NOP (1) = 13.
    cpu.run_until(kResetCycles + 6);  // = 13

    CHECK(cpu.a == 0x42);
    CHECK(cpu.config.mem[0x0030] == 0x42);
    CHECK((cpu.p & tawny::flag::Z) == 0);
    CHECK((cpu.p & tawny::flag::N) == 0);
    CHECK(cpu.pc == 0x0405);
}

// 5. ADC binary — spot-check the interesting flag combinations.
TEST_CASE("M6502: ADC # binary flag cases") {
    SUBCASE("0x10 + 0x20, C=0 → 0x30, no flags") {
        auto cpu = make_cpu();
        cpu.config.mem[0x0400] = 0xA9; cpu.config.mem[0x0401] = 0x10;
        cpu.config.mem[0x0402] = 0x69; cpu.config.mem[0x0403] = 0x20;
        cpu.config.mem[0x0404] = 0xEA;

        cpu.run_until(kResetCycles + 6);
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

        cpu.run_until(kResetCycles + 6);
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

        cpu.run_until(kResetCycles + 6);
        CHECK(cpu.a == 0x80);
        CHECK((cpu.p & tawny::flag::V) != 0);
        CHECK((cpu.p & tawny::flag::N) != 0);
        CHECK((cpu.p & tawny::flag::C) == 0);
    }
}

// 6. JMP abs.
TEST_CASE("M6502: JMP abs sets pc and takes 3 cycles") {
    auto cpu = make_cpu();
    cpu.config.mem[0x0400] = 0x4C;
    cpu.config.mem[0x0401] = 0x00;
    cpu.config.mem[0x0402] = 0x05;  // → $0500
    cpu.config.mem[0x0500] = 0xEA;

    // reset (7) + JMP abs (3) + NOP opcode fetch (1) = 11.
    cpu.run_until(kResetCycles + 4);  // = 11

    CHECK(cpu.pc == 0x0501);
    CHECK(cpu.cycle == 11);
}

// 7-9. BNE timing.
TEST_CASE("M6502: BNE not taken — 2 cycles") {
    auto cpu = make_cpu();
    cpu.config.mem[0x0400] = 0xA9; cpu.config.mem[0x0401] = 0x00;  // LDA #$00 → Z=1
    cpu.config.mem[0x0402] = 0xD0; cpu.config.mem[0x0403] = 0x7F;  // BNE (not taken)
    cpu.config.mem[0x0404] = 0xEA;

    // reset (7) + LDA# (2) + BNE not-taken (2) + NOP fetch (1) = 12.
    cpu.run_until(kResetCycles + 5);  // = 12

    CHECK(cpu.cycle == 12);
    CHECK(cpu.pc == 0x0405);
}

TEST_CASE("M6502: BNE taken, no page cross — 3 cycles") {
    auto cpu = make_cpu();
    cpu.config.mem[0x0400] = 0xA9; cpu.config.mem[0x0401] = 0x01;  // LDA #$01 → Z=0
    cpu.config.mem[0x0402] = 0xD0; cpu.config.mem[0x0403] = 0x05;  // BNE +5
    cpu.config.mem[0x0409] = 0xEA;

    // reset (7) + LDA# (2) + BNE taken (3) + NOP fetch (1) = 13.
    cpu.run_until(kResetCycles + 6);  // = 13

    CHECK(cpu.cycle == 13);
    CHECK(cpu.pc == 0x040A);
}

TEST_CASE("M6502: BNE taken with page cross — 4 cycles") {
    auto cpu = make_cpu();
    cpu.config.mem[0x0400] = 0xA9; cpu.config.mem[0x0401] = 0x01;  // LDA #$01
    for (std::uint16_t i = 0x0402; i < 0x04FD; ++i) {
        cpu.config.mem[i] = 0xEA;
    }
    cpu.config.mem[0x04FD] = 0xD0; cpu.config.mem[0x04FE] = 0x05;  // BNE +5 → 0x0504
    cpu.config.mem[0x0504] = 0xEA;

    // Old horizon was 512; new = 516 (+4 for the longer reset).
    cpu.run_until(kResetCycles + 509);  // = 516

    CHECK(cpu.cycle == 516);
    CHECK(cpu.pc == 0x0505);
}

// 10. Horizon mid-instruction.
TEST_CASE("M6502: horizon mid-instruction saves state and resumes exactly") {
    auto cpu = make_cpu();
    cpu.config.mem[0x0400] = 0xAD;  // LDA abs — 4 cycles
    cpu.config.mem[0x0401] = 0x34;
    cpu.config.mem[0x0402] = 0x12;
    cpu.config.mem[0x1234] = 0x77;
    cpu.config.mem[0x0403] = 0xEA;

    // reset (7, incl. LDA abs opcode fetch) + LDA abs step 0 (1) = 8; cycle=9.
    auto first = cpu.run_until(kResetCycles + 2);  // = 9

    CHECK(first == 9);
    CHECK(cpu.cycle == 9);
    CHECK(cpu.a == 0);
    CHECK(cpu.pc == 0x0402);

    // Resume: LDA abs step 1 + step 2 = 2 cycles; cycle = 11.
    auto second = cpu.run_until(kResetCycles + 4);  // = 11

    CHECK(second == 11);
    CHECK(cpu.cycle == 11);
    CHECK(cpu.a == 0x77);
    CHECK(cpu.pc == 0x0403);
}

// 11. Dormann trap: JMP $0400 at $0400.
TEST_CASE("M6502: JMP-to-self trap stops the timeslice via access_cost stop") {
    auto cpu = make_cpu();
    cpu.config.mem[0x0400] = 0x4C;
    cpu.config.mem[0x0401] = 0x00;
    cpu.config.mem[0x0402] = 0x04;

    auto returned = cpu.run_until(1ull << 32);

    // Cycles: reset (7, incl. first JMP opcode fetch via BRK step 6) + JMP step 0
    // (1) + JMP step 1 (1; sets pc to 0x0400, access_cost_opcode(0x0400) matches
    // last_opcode_addr → stop). current was incremented by the cost before the
    // guard, so cycle = 10.
    CHECK(returned == 10);
    CHECK(cpu.cycle == 10);
    CHECK(cpu.pc == 0x0400);
    CHECK(cpu.pending_addr == 0x0400);
    CHECK(cpu.tstate == ((0x4C << 3) | 2));
}

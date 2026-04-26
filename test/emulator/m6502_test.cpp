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

// =============================================================================
// IRQ / NMI tests
// =============================================================================

namespace {

// Test config that lets the test script IRQ assertion / NMI edge cycles.
// Behaviour for non-interrupt accesses matches DormannCpuConfig (incl. JMP-to-
// self trap detection in access_cost_opcode), which lets us terminate via the
// existing trap mechanism.
struct ProgrammableInterruptConfig : tawny::dormann::DormannCpuConfig {
    tawny::Cycle irq_since{tawny::interrupt_never};
    tawny::Cycle nmi_at{tawny::interrupt_never};
    bool         nmi_consumed{false};

    auto irq_asserted_since() const -> tawny::Cycle { return irq_since; }
    auto nmi_edge_at()        const -> tawny::Cycle
    {
        return nmi_consumed ? tawny::interrupt_never : nmi_at;
    }
    auto consume_nmi() -> void { nmi_consumed = true; }
};

static_assert(tawny::M6502Config<ProgrammableInterruptConfig>);

using IrqCpu = tawny::M6502<ProgrammableInterruptConfig>;

// Build a CPU starting at $0400 with set_pc (skipping reset), I=0, S=0xFD,
// and standard interrupt vectors:
//   $FFFA/B → $0600 (NMI handler)
//   $FFFE/F → $0500 (IRQ / software-BRK handler)
// Both handlers default to JMP * so the trap detector catches handler entry.
// Caller supplies the body at $0400.
auto make_irq_cpu() -> IrqCpu
{
    ProgrammableInterruptConfig cfg{};
    // Vectors.
    cfg.mem[0xFFFE] = 0x00; cfg.mem[0xFFFF] = 0x05;
    cfg.mem[0xFFFA] = 0x00; cfg.mem[0xFFFB] = 0x06;
    // Handlers: JMP * (3-byte trap).
    cfg.mem[0x0500] = 0x4C; cfg.mem[0x0501] = 0x00; cfg.mem[0x0502] = 0x05;
    cfg.mem[0x0600] = 0x4C; cfg.mem[0x0601] = 0x00; cfg.mem[0x0602] = 0x06;

    IrqCpu cpu{std::move(cfg)};
    cpu.set_pc(0x0400);
    cpu.s = 0xFD;
    cpu.p = tawny::flag::U;        // I = 0
    return cpu;
}

constexpr auto kIrqVec = std::uint16_t{0x0500};
constexpr auto kNmiVec = std::uint16_t{0x0600};

}  // namespace

// 1. IRQ with I clear runs handler.
TEST_CASE("M6502: IRQ with I=0 enters handler at $FFFE/F") {
    auto cpu = make_irq_cpu();
    // Program: NOP NOP NOP NOP JMP * — the IRQ should preempt before the trap.
    cpu.config.mem[0x0400] = 0xEA;
    cpu.config.mem[0x0401] = 0xEA;
    cpu.config.mem[0x0402] = 0xEA;
    cpu.config.mem[0x0403] = 0xEA;
    cpu.config.mem[0x0404] = 0x4C;
    cpu.config.mem[0x0405] = 0x04;
    cpu.config.mem[0x0406] = 0x04;
    // IRQ asserted from cycle 0 — should fire at the first instruction boundary.
    cpu.config.irq_since = 0;

    cpu.run_until(1ull << 30);

    CHECK(cpu.pc == kIrqVec);                         // handler trapping
    CHECK((cpu.p & tawny::flag::I) == tawny::flag::I); // I set in handler
    CHECK(cpu.s == 0xFA);                              // 3 pushes
}

// 2. IRQ with I set is masked.
TEST_CASE("M6502: IRQ with I=1 is masked, handler never entered") {
    auto cpu = make_irq_cpu();
    cpu.p = tawny::flag::U | tawny::flag::I;
    cpu.config.mem[0x0400] = 0xEA;                     // NOP
    cpu.config.mem[0x0401] = 0x4C;                     // JMP $0401
    cpu.config.mem[0x0402] = 0x01;
    cpu.config.mem[0x0403] = 0x04;
    cpu.config.irq_since = 0;

    cpu.run_until(1ull << 30);

    CHECK(cpu.pc == 0x0401);                           // trap on JMP $0401
    CHECK(cpu.s == 0xFD);                              // no pushes
}

// 3. SEI quirk — IRQ asserted before SEI is still serviced after SEI.
TEST_CASE("M6502: SEI quirk — pending IRQ runs handler after SEI") {
    auto cpu = make_irq_cpu();
    // SEI ($78) NOP NOP JMP *.
    cpu.config.mem[0x0400] = 0x78;
    cpu.config.mem[0x0401] = 0xEA;
    cpu.config.mem[0x0402] = 0xEA;
    cpu.config.mem[0x0403] = 0x4C;
    cpu.config.mem[0x0404] = 0x03;
    cpu.config.mem[0x0405] = 0x04;
    cpu.config.irq_since = 0;

    cpu.run_until(1ull << 30);

    CHECK(cpu.pc == kIrqVec);
    // Pre-SEI I was 0 — pushed P (at $01FB after PCH/PCL) should reflect that.
    CHECK((cpu.config.mem[0x01FB] & tawny::flag::I) == 0);
}

// 4. CLI delay — IRQ pending under I=1 fires only after the instruction *after* CLI.
TEST_CASE("M6502: CLI delay — IRQ fires after the instruction following CLI") {
    auto cpu = make_irq_cpu();
    cpu.p = tawny::flag::U | tawny::flag::I;
    // CLI ($58) NOP_a NOP_b JMP *
    // Per NESdev: CLI's boundary poll uses pre-CLI I (=1), so no IRQ at CLI.
    // The next instruction (NOP_a)'s boundary polls with I=0 and IRQ fires.
    // So handler enters AFTER NOP_a, BEFORE NOP_b.
    cpu.config.mem[0x0400] = 0x58;
    cpu.config.mem[0x0401] = 0xEA;
    cpu.config.mem[0x0402] = 0xEA;
    cpu.config.mem[0x0403] = 0x4C;
    cpu.config.mem[0x0404] = 0x03;
    cpu.config.mem[0x0405] = 0x04;
    cpu.config.irq_since = 0;

    cpu.run_until(1ull << 30);

    CHECK(cpu.pc == kIrqVec);
    // Return address pushed should be 0x0402 (after NOP_a, NOP_b not yet run).
    CHECK(cpu.config.mem[0x01FD] == 0x04);             // PCH
    CHECK(cpu.config.mem[0x01FC] == 0x02);             // PCL
}

// 5. PLP delay — pulling I=0 with pending IRQ behaves like CLI.
TEST_CASE("M6502: PLP delay — IRQ fires after instruction following PLP") {
    auto cpu = make_irq_cpu();
    cpu.p = tawny::flag::U | tawny::flag::I;
    cpu.s = 0xFC;
    // Pre-stage: stack-top = P with I=0.
    cpu.config.mem[0x01FD] = tawny::flag::U;
    // PLP ($28) NOP NOP JMP *
    cpu.config.mem[0x0400] = 0x28;
    cpu.config.mem[0x0401] = 0xEA;
    cpu.config.mem[0x0402] = 0xEA;
    cpu.config.mem[0x0403] = 0x4C;
    cpu.config.mem[0x0404] = 0x03;
    cpu.config.mem[0x0405] = 0x04;
    cpu.config.irq_since = 0;

    cpu.run_until(1ull << 30);

    CHECK(cpu.pc == kIrqVec);
    // Handler enters after the NOP that follows PLP (PLP was 4 cycles, NOP 2):
    // expected return PC = $0402 (NOP at $0401 ran, NOP at $0402 not yet).
    // PLP raised s to 0xFD, then IRQ pushed PCH/PCL/P at 0xFD/0xFC/0xFB.
    CHECK(cpu.config.mem[0x01FD] == 0x04);             // PCH
    CHECK(cpu.config.mem[0x01FC] == 0x02);             // PCL
}

// 6. RTI immediate — pulling I=0 enables IRQ on the very next instruction.
TEST_CASE("M6502: RTI immediate — IRQ taken on the next instruction") {
    auto cpu = make_irq_cpu();
    cpu.p = tawny::flag::U | tawny::flag::I;
    cpu.s = 0xFA;
    // Stack contents that RTI will pull, top to bottom:
    //   [s+1] P = U (I=0)
    //   [s+2] PCL = 0x00
    //   [s+3] PCH = 0x04
    cpu.config.mem[0x01FB] = tawny::flag::U;           // P
    cpu.config.mem[0x01FC] = 0x01;                     // PCL → return to $0401
    cpu.config.mem[0x01FD] = 0x04;                     // PCH
    cpu.config.mem[0x0400] = 0x40;                     // RTI (executed if PC=$0400)
    // Continuation at $0401: NOP NOP JMP *
    cpu.config.mem[0x0401] = 0xEA;
    cpu.config.mem[0x0402] = 0xEA;
    cpu.config.mem[0x0403] = 0x4C;
    cpu.config.mem[0x0404] = 0x03;
    cpu.config.mem[0x0405] = 0x04;
    cpu.config.irq_since = 0;

    cpu.run_until(1ull << 30);

    CHECK(cpu.pc == kIrqVec);
    // RTI returned to $0401. Per NESdev RTI's I-effect is immediate, so the
    // boundary poll at end of RTI sees I=0 and IRQ fires *before* the NOP at
    // $0401 runs. Pushed return PC = $0401.
    CHECK(cpu.config.mem[0x01FD] == 0x04);             // PCH
    CHECK(cpu.config.mem[0x01FC] == 0x01);             // PCL
}

// 9. NMI fires regardless of I.
TEST_CASE("M6502: NMI ignores I flag, enters NMI handler") {
    auto cpu = make_irq_cpu();
    cpu.p = tawny::flag::U | tawny::flag::I;
    cpu.config.mem[0x0400] = 0xEA;
    cpu.config.mem[0x0401] = 0xEA;
    cpu.config.mem[0x0402] = 0x4C;
    cpu.config.mem[0x0403] = 0x02;
    cpu.config.mem[0x0404] = 0x04;
    cpu.config.nmi_at = 0;

    cpu.run_until(1ull << 30);

    CHECK(cpu.pc == kNmiVec);
    CHECK(cpu.config.nmi_consumed);
}

// 10. NMI > IRQ priority.
TEST_CASE("M6502: NMI takes priority over simultaneous IRQ") {
    auto cpu = make_irq_cpu();
    cpu.config.mem[0x0400] = 0xEA;
    cpu.config.mem[0x0401] = 0xEA;
    cpu.config.mem[0x0402] = 0x4C;
    cpu.config.mem[0x0403] = 0x02;
    cpu.config.mem[0x0404] = 0x04;
    cpu.config.irq_since = 0;
    cpu.config.nmi_at    = 0;

    cpu.run_until(1ull << 30);

    CHECK(cpu.pc == kNmiVec);
}

// 11. NMI hijack — software BRK in flight, NMI fires during BRK ticks 1-3.
TEST_CASE("M6502: NMI hijacks an in-flight software BRK") {
    auto cpu = make_irq_cpu();
    // BRK ($00 + signature) JMP * (post-BRK).
    cpu.config.mem[0x0400] = 0x00;
    cpu.config.mem[0x0401] = 0xFF;                     // signature byte
    cpu.config.mem[0x0402] = 0x4C;
    cpu.config.mem[0x0403] = 0x02;
    cpu.config.mem[0x0404] = 0x04;
    // Schedule NMI to fire at cycle 2 (during BRK step 1 or 2). BRK starts at
    // cycle 1 (FETCH for BRK opcode), so cycle 2-3 is in the BRK sequence.
    cpu.config.nmi_at = 2;

    cpu.run_until(1ull << 30);

    // Hijack landed: vector fetched is NMI's, handler is at $0600.
    CHECK(cpu.pc == kNmiVec);
    // B flag still pushed as 1 (signature of software BRK). P pushed at
    // s=0xFB (after PCH at 0xFD and PCL at 0xFC).
    CHECK((cpu.config.mem[0x01FB] & tawny::flag::B) == tawny::flag::B);
}

// 7. Taken-no-cross branch eats the IRQ (deferred to post-branch instruction).
TEST_CASE("M6502: taken-no-cross 3-cycle branch eats interrupt") {
    auto cpu = make_irq_cpu();
    // BNE $0405 (taken because Z=0 from initial p), no page cross.
    // After branch: NOP, JMP *.
    cpu.config.mem[0x0400] = 0xD0;                     // BNE
    cpu.config.mem[0x0401] = 0x03;                     // offset +3 → $0405
    // pad
    cpu.config.mem[0x0405] = 0xEA;                     // NOP at branch target
    cpu.config.mem[0x0406] = 0x4C;                     // JMP *
    cpu.config.mem[0x0407] = 0x06;
    cpu.config.mem[0x0408] = 0x04;
    // IRQ asserted at cycle 1 — within the branch's operand-fetch cycle.
    // Per NESdev, the eaten-poll rule says this is NOT serviced at the branch
    // boundary; it's serviced at the boundary of the post-branch NOP.
    cpu.config.irq_since = 1;

    cpu.run_until(1ull << 30);

    CHECK(cpu.pc == kIrqVec);
    // Pushed return PC should be $0406 — the NOP ran before IRQ took effect.
    CHECK(cpu.config.mem[0x01FD] == 0x04);             // PCH
    CHECK(cpu.config.mem[0x01FC] == 0x06);             // PCL
}

// 12. Interrupt sequences don't poll — handler's first instruction always runs.
TEST_CASE("M6502: handler's first instruction always runs (no immediate re-entry)") {
    auto cpu = make_irq_cpu();
    cpu.config.mem[0x0400] = 0xEA;
    cpu.config.mem[0x0401] = 0x4C;
    cpu.config.mem[0x0402] = 0x01;
    cpu.config.mem[0x0403] = 0x04;
    // Replace the IRQ handler with: CLI; JMP *. Even with IRQ continuously
    // asserted (level-triggered) and CLI re-enabling I, the handler's first
    // instruction (CLI) must always run before another IRQ can preempt.
    cpu.config.mem[0x0500] = 0x58;                     // CLI
    cpu.config.mem[0x0501] = 0x4C;                     // JMP $0501
    cpu.config.mem[0x0502] = 0x01;
    cpu.config.mem[0x0503] = 0x05;
    cpu.config.irq_since = 0;

    cpu.run_until(1ull << 30);

    // Handler entered, ran CLI, then trapped at JMP $0501. We did NOT
    // immediately re-enter the handler.
    CHECK(cpu.pc == 0x0501);
    CHECK(cpu.s == 0xFA);                              // exactly one entry's pushes
}

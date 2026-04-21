# Tawny (C++) Development Journal

## 2026-04-19 — Project bootstrap

### What we did
- Created initial project structure with CMake
- Set up GLFW 3.4 (via FetchContent) for windowing
- Generated GLAD for OpenGL 3.3 Core and vendored the sources into `extern/glad/`
- Vendored doctest single-header into `extern/doctest.h`
- Implemented a 1024x768 window titled "Tawny" that clears to dark blue
- Integrated doctest into the single executable: `--test` flag runs tests and exits, otherwise the emulator runs normally
- Tests run automatically as a CMake post-build step
- Added example in-source test (in `src/main.cpp`) and a standalone test file (`test/example_test.cpp`)
- Added README, MIT license, and CLAUDE.md

### Design decisions
- **C++ rewrite** — rewriting the Rust tawny emulator in C++, using a deferred synchronisation (lazy catch-up) approach rather than the tick-accurate model used in the Rust version
- **Single executable for app + tests** — doctest's `DOCTEST_CONFIG_IMPLEMENT` lets us control whether tests run via command-line flags. No separate test binary needed. Release builds strip all test code via `DOCTEST_CONFIG_DISABLE`.
- **In-source tests** — doctest supports writing `TEST_CASE` directly in implementation files. Heavyweight tests go in `test/`. Both are globbed into the same executable.
- **Post-build test step** — tests run every build, catching regressions immediately
- **GLAD vendored** — generated upfront for OpenGL 3.3 Core and committed, rather than generating at build time via CMake. Simpler, no Python dependency at build time.
- **GLFW via FetchContent** — fetched at configure time if not found on the system. Avoids requiring a system install.
- **extern/ directory** — for vendored third-party code (doctest, GLAD)

## 2026-04-19 — Housekeeping: doctest layout, branch rename, style

### What we did
- Reorganised doctest vendoring to `extern/doctest/include/doctest/doctest.h`, exposed via an INTERFACE CMake target so it's included as `#include <doctest/doctest.h>`
- Removed the blanket `extern/` include path; each vendored lib now carries its own include directory via its CMake target
- Renamed local branch `master` → `main`
- Upgraded target language to **C++23**
- Agreed on coding conventions (captured in CLAUDE.md)

### Design decisions
- **Angle-bracket includes for vendored libs** — treating third-party as "system" keeps the intent clear (`<doctest/doctest.h>` vs `"mycode.h"`) and each lib owns its own include path
- **Naming:** `UpperCamelCase` for types (avoids `Result result;` clashes), `snake_case` for functions/variables/members, `UpperCamelCase` for enum values. No trailing underscore on members; no mandatory `this->`. Both may be revisited if ambiguity becomes painful in practice.
- **Idiom:** std algorithms/ranges over raw loops; functional style (variants, optional, monadic APIs) where performance allows; `auto` with trailing return types. C++23 lets us use features like `std::expected`, `std::print`, `constexpr` containers.
- **Brace style:** one true brace style (K&R variant) — opening brace on same line everywhere *except* function definitions, which get a new line. Reads as a strong visual cue that "this is a definition body" versus "this is a control-flow scope".

## 2026-04-19 — Dormann functional test harness

### What we did
- Built Klaus Dormann's 6502 functional test from the ca65 source with `load_data_direct = 0`, assembled with ca65 + linked with ld65.
- Generated `test/dormann/functional_test_bin.cpp`: the CODE segment ($0400–$3915, 13590 bytes) as an `inline constexpr std::uint8_t[]`, plus `load_addr`, `entry_addr`, and `success_addr` constants. With `load_data_direct = 0` the test initialises its own ZP, $0200 data area and reset vectors at runtime, so we only need to carry the code — about 5× smaller than the flat 64K image.
- Sketched `src/emulator/m6502.h`: an `M6502Config` C++20 concept defining the bus interface (split read/write for opcode/ZP/stack/vector/generic, `access_cost_*` per kind, and `stop_requested()`), and a templated `M6502<Config>` struct with a constructor that moves the config in place.
- Wrote `test/dormann/dormann_cpu_config.h`: a minimal `M6502Config` implementation with a `std::unique_ptr<std::uint8_t[]>`-owned 64K RAM, all access costs = 1, and trap detection via "two consecutive opcode fetches at the same PC" → sets `stop_flag`.
- Added the `test/` directory to the include path; expanded the CMake globs to pick up `.h` files for IntelliSense.

### Design decisions
- **Split read/write/access_cost methods in the concept** — ZP/stack/opcode/vector fetches on BBC Micro can't hit MMIO, so fast-path implementations skip the full bus decode. The concept makes this split explicit rather than relying on the implementation to branch internally on the address.
- **`stop_requested()` on the Config** for early timeslice termination — zero-cost when false (one predictable branch per instruction), the config owns the signal, and it covers real-world uses beyond test traps (UI pause, breakpoint, vsync alignment). Considered exceptions, sentinel cost values, and a back-channel CPU flag; polling won.
- **Config held by value in M6502, moved in through the constructor** — aggressive inlining; stateful configs hold a pointer/unique_ptr internally. First version used `std::span<std::uint8_t, 65536>` with externally-allocated memory but that was verbose at call sites; switched to `std::unique_ptr<std::uint8_t[]>` for self-contained ownership.
- **Trap detection heuristic** — `JMP *` and `BXX *` all produce two consecutive opcode fetches at the same PC. Simple state (one `last_opcode_addr`), no need to disassemble; covers every trap macro the Dormann test uses (success + failure alike).
- **Source path convention:** `src/` and `test/` are both added to the include path, so we write `#include "emulator/m6502.h"` and `#include "dormann/dormann_cpu_config.h"` with the top-level directory implicit.

## 2026-04-20 — 6502 emulator: deferred-synchronisation architecture + stage 1 opcodes

### What we did
- Nailed down the `M6502` run loop shape: `run_until(horizon)` executes whole 6502 cycles as long as `cycles < horizon`, with mid-instruction state persisted across calls. Each loop iteration = one 6502 cycle, with the three-phase shape phi2 (bus op set up by previous phi1) → phi1 (micro-op: update registers, advance `tstate`, schedule next bus op) → cost (query `access_cost_*`, advance `c`, stop if signalled).
- Replaced `stop_requested()` on the concept with `AccessCost { unsigned cost; bool stop; }` returned from every `access_cost_*`. Cost is always the honest cycle count; `stop` asks `run_until` to break after advancing the clock but before performing the next phi2. `access_cost_*` is contractually pure/idempotent.
- Added `Cycle = uint64_t` time type, `BrkFlags` enum, combined `tstate = (ir << 3) | step`, `base_addr` scratch register, and the pending bus op `{pending_addr, pending_data}` as CPU state.
- Constructor calls `reset()`, which seeds `tstate = RESET_STEP_0`, `pending_addr = 0xFFFC`, and pre-pays the cost of the reset-vector fetch so the loop's first iteration can jump straight into phi2 with no special case.
- Implemented flat `switch(tstate)` dispatch in `src/emulator/m6502_dispatch.inc`, `#include`'d inside the run loop. Cases for: synthetic RESET_STEP_0/1 (simplified 2-cycle reset — full 7-cycle BRK reset is a follow-up), shared FETCH_OPCODE step at `tstate = 0x7FF`, and stage 1 opcodes: NOP, INX, LDA#, CMP#, ADC# (binary), LDA zp, LDA abs, STA zp, JMP abs, BNE rel (2/3/4 cycles with page-cross).
- `DormannCpuConfig` now implements the new `AccessCost` contract; trap detection moved to `access_cost_opcode` (pure compare) while `read_opcode` does the state update (`last_opcode_addr`).
- 14 tests covering the architecture end-to-end: reset seeding, reset-through, 100 NOPs cycle accounting, LDA#/STA zp, ADC binary flag cases, JMP, all three BNE timings (including page-cross via a 253-NOP pad to land the branch at 0x04FD), mid-instruction horizon save/resume, Dormann JMP-to-self trap detection. 64 assertions, all passing.

### Design decisions
- **Micro-ops fuse phi2 + phi1 per case.** Unlike the Rust reference where micro-ops are phi1-only and the phi2 kind is separately encoded in the `Mos6502Output`, here each case statically knows its phi2 kind and its next-phi2 cost method — so "fetch next opcode after STA" and "fetch next opcode after LDA" are distinct cases (one did `write_zp`, one did `read_zp` at phi2). This trades code duplication against erasing a runtime `AccessKind` dispatch; the compiler melts the `access_cost_*` / `read_*` / `write_*` calls into each case body.
- **Shared FETCH_OPCODE at synthetic tstate 0x7FF.** Rather than duplicating the opcode-decode step 256 times (once per instruction), every instruction's penultimate step sets `tstate = FetchOpcode` and `pending = read_opcode(pc)`. One canonical case decodes and dispatches.
- **Stop via `AccessCost::stop`, not `cost == 0`.** The earlier "cost of 0 means stop" idea overloaded the value domain and made it ambiguous whether 0-cost accesses were meaningful. A dedicated flag is clearer and zero extra cost.
- **Simplified 2-cycle reset for stage 1.** Real 6502 reset is 7 cycles of mostly-dummy stack reads followed by vector fetch. For stage 1 we seed the pending op directly at the vector fetch, skipping the dummy cycles. Full BRK-style reset (branching on `brk_flags`) is a follow-up together with IRQ/NMI dispatch.
- **Cycle-counting invariant: `cycles` = start time of the next phi2 to run.** `reset()` pre-pays the cost of the first access (so `cycles = 1` initially). Each iteration: phi2 runs at time `c`, phi1 schedules the next op, `c += cost_of_next`. On exit, `cycles` holds the start time of the deferred phi2 — next `run_until` resumes from there without recomputing anything.
- **Interrupts sampled once per `run_until` entry.** Under deferred synchronisation nothing can raise IRQ/NMI mid-loop (the CPU is the only thing running), so `sample_interrupts()` runs at the top of `run_until` and nowhere else. Stage 1 stub; edge detection and the `int_shift` pipeline are follow-ups.

## 2026-04-20 — 6502 dispatch rewritten: fall-through switch, macros, no .inc

### What we did
- Rewrote the `M6502::run_until` dispatch to match the user's intended shape: a single `while (current < horizon)` loop wrapping a `switch (tst)` whose cases **fall through between steps of the same instruction**. Only the last step of each instruction (opcode fetch for the next one) `break`s; the `while` loop re-enters the switch on the new `tst`. In the common case one `while` iteration runs a full instruction.
- Retired the `src/emulator/m6502_dispatch.inc` file. The dispatch now lives entirely in `src/emulator/m6502.h` as a set of macros: `TAWNY_STEP_TAIL` (per-cycle tail — cost query, horizon/stop guard, save-and-goto-exit), `TAWNY_FETCH_OPCODE_CASE` (the last step of every instruction), and one addressing-mode macro per shape (`TAWNY_IMPLIED`, `TAWNY_IMM_READ`, `TAWNY_ZP_READ`, `TAWNY_ZP_WRITE`, `TAWNY_ABS_READ`, `TAWNY_ABS_JUMP`, `TAWNY_REL_BRANCH`). Each opcode is now one line at the switch site.
- Pulled the per-mnemonic transforms out as small op-class structs (`detail::Lda`, `Sta`, `Cmp`, `AdcBin`, `Inx`, `Nop`, `BneCond`) with static templated `apply` / `value` / `taken` methods. The op class is a macro parameter, so the same addressing-mode macro serves any mnemonic in its family.
- Early-stop unification: `if (ac.stop) horizon = current;` raises the horizon so the subsequent `if (current >= horizon)` fires the same save-and-exit path. One exit site per case, not two.
- Renamed struct field `cycles` → `cycle` (single "start time of next cycle" value). Removed `pending_data` — write instructions now pull their value straight from a register (`this->a` for STA) at write time, so no mid-instruction scratch is needed.
- Added a synthetic `detail::ResetOpcodeFetch` tstate as the third step of the simplified reset sequence (ResetStep0 → ResetStep1 → ResetOpcodeFetch). Stage-1 reset is 3 cycles (two vector fetches + first opcode fetch); full 7-cycle BRK-based reset remains a follow-up.
- All 14 test cases still pass (64 assertions). Cycle-count expectations unchanged: reset still observably takes 3 cycles, each NOP 2 cycles, LDA # 2, LDA abs 4, BNE 2/3/4.

### Design decisions
- **Fall-through dispatch wins over per-iteration dispatch.** Jump-table entry happens once per instruction instead of once per cycle — better branch prediction and tighter case bodies. The explicit `tstate = next_step` assignments go away because the next case label literally is the next step. The cost: each instruction's last step must `break` (otherwise control falls into the next opcode's case body), and `[[fallthrough]]` attributes must sit between intra-instruction cases to silence `-Wimplicit-fallthrough`. Macros emit all of that consistently.
- **Per-instruction opcode fetch, not a shared `FetchOpcode` case.** A shared case would require cross-opcode fall-through, which doesn't work with the lexical-order semantics of `switch` fall-through. Each `TAWNY_FETCH_OPCODE_CASE` expansion adds ~10 lines of boilerplate per opcode — acceptable; the macro makes the duplication invisible at the call site.
- **`addr` is reused across phi2 and next-phi2.** Inside a case body `addr` holds the current phi2's target; after the read/write we overwrite it with the next phi2's target. LDA zp step 0 is the clearest example: `addr = config.read(addr)` reads at the old `addr` (= pc pointing at the zp-addr byte) and the u8→u16 assignment leaves `addr` holding the ZP address the next step will read from.
- **`base` local for multi-byte operands.** ABS_READ / ABS_JUMP split the target address across two steps; `base` holds the low byte between step 0 (read lo) and step 1 (read hi + combine). Saved to `base_addr` on exit.
- **REL_BRANCH uses `break` between steps, not fall-through.** Branches fork three ways (not-taken, taken-no-cross, taken-cross) with different successor steps, so fall-through can't express the control flow. Each step sets `tst` explicitly and breaks; the `while` re-enters the switch. One extra dispatch per cycle on the branch path is acceptable — branches are the minority.

## 2026-04-20 — Full 256-opcode NMOS 6502 + Dormann functional test passes

### What we did
- Replaced the synthetic `ResetStep0/1/OpcodeFetch` reset hack with BRK-based reset. `TAWNY_BRK` macro implements the full 7-step microcode with runtime branching on `brk_flags` — same code path handles software BRK, Reset, IRQ, and NMI. The constructor seeds `tstate = 0`, `brk_flags = Reset`, and enters the BRK dispatch naturally.
- Added helper macros for common per-step patterns: `TAWNY_NEXT_STACK(NEXT_TST)` sets the next phi2 to a stack access at current S; `TAWNY_NEXT_OPCODE_FETCH(NEXT_TST)` sets it to an opcode fetch at PC. Refactored existing macros (IMPLIED, IMM_READ, ZP_READ, ZP_WRITE, ABS_READ, ABS_JUMP, REL_BRANCH) + BRK to use them.
- Added all remaining addressing-mode macros: `TAWNY_ABS_WRITE`, `ZPX/ZPY_READ/WRITE` (common `ZP_INDEXED_*` helper parameterised by index register), `ABX/ABY_READ/WRITE` (common `AB_INDEXED_*`, with page-cross optimisation for reads, always-penalty for writes), `IZX/IZY_READ/WRITE`, `ACC_RMW`, `ZP/ZPX/ABS/ABX_RMW`, `JMP_IND` (with NMOS page-wrap bug), `JSR`, `RTS`, `RTI`, `PUSH` / `PULL` (op-class-parameterised for `Pha/Php/Pla/Plp`), `JAM`.
- Added ~30 new op-class structs in `detail::`: loads (`Ldx`, `Ldy`), stores (`Stx`, `Sty`, `Sax`), stack (`Pha`, `Php`, `Pla`, `Plp`), logic (`And`, `Ora`, `Eor`, `Bit`), compares (`Cpx`, `Cpy`), transfers (`Tax`/`Tay`/`Tsx`/`Txa`/`Txs`/`Tya`), inc/dec registers (`Iny`, `Dex`, `Dey`), flag ops (`Clc`/`Sec`/`Cli`/`Sei`/`Clv`/`Cld`/`Sed`), RMW (`Asl`, `Lsr`, `Rol`, `Ror`, `Inc`, `Dec`), branch conditions (`Bpl/Bmi/Bvc/Bvs/Bcc/Bcs/Beq`), and stable illegals (`Lax`, `Slo`, `Rla`, `Sre`, `Rra`, `Dcp`, `Isc`, `Anc`, `Alr`, `Arr`, `Axs`, `Usbc`).
- Added decimal-mode ADC/SBC (NMOS flavour — N/V flags from binary calc, A/C/Z from BCD-adjusted result). `Adc`/`Sbc` now expose public `apply_binary` and `apply_decimal` plus an `apply` that branches on the D flag. `Rra`/`Isc`/`Usbc` reuse these.
- Wired all 256 opcode slots in the switch, one line per opcode. Stable illegals in zp/zpx/abs/abx forms (the aby/izx/izy RMW variants stub as JAM pending `TAWNY_ABY_RMW`/`IZX_RMW`/`IZY_RMW` macros). Unstable illegals (ANE/LXA/SHA/SHX/SHY/TAS/LAS) stub as JAM.
- Removed the `DOCTEST_CONFIG_DISABLE` gate from CMakeLists so tests compile in every build type. Switched build to Release.
- Renamed `test/dormann/functional_test_bin.cpp` → `functional_test_bin.h` (`inline constexpr` made it header-shaped anyway; just added `#pragma once`).
- New `test/emulator/m6502_dormann_test.cpp` runs the full Klaus Dormann functional test 10 times, checks `pc == success_addr`, and prints the average effective clock speed. **All 10 runs pass.** Observed: ~96.25M cycles/run, ~131 ms wall time, **~735 MHz effective** — 367× a real 2 MHz 6502.

### Design decisions
- **BRK is the only opcode where phi2's bus-op kind isn't statically fixed per case.** For Reset, step 0-2 do dummy stack reads; for SW BRK / IRQ / NMI they write PCH / PCL / P. One runtime branch on `brk_flags` per step picks between `config.read_stack` and `config.write_stack` — an acceptable accuracy-vs-purity tradeoff for the one opcode that needs it. Every other case has its phi2 kind baked in at compile time.
- **Push/pull op classes mirror store/read op classes.** `TAWNY_PUSH(OPCODE, OP_CLASS)` calls `OP_CLASS::value(*this)` (matching the `StoreOp` concept); `TAWNY_PULL(OPCODE, OP_CLASS)` calls `OP_CLASS::apply(*this, pulled_byte)` (matching `ReadOp`). The switch lines stay uniform: `TAWNY_PUSH(0x48, detail::Pha)` reads the same as `TAWNY_ZP_WRITE(0x85, detail::Sta)`.
- **Horizon guard absorbs the stop signal.** `if (ac.stop) horizon = current;` in the common step tail means both stop and overshoot exit through the same `if (current >= horizon) goto exit;` check. One exit site per case, not two. Found this neater than a sentinel return / goto chain.
- **JAM acts as an "obvious failure trap".** Its micro-op rewinds PC and reschedules the same opcode fetch — the next iteration's cost query hits `last_opcode_addr == addr`, Dormann's trap detector fires `stop = true`, and `run_until` returns with pc at the JAM site. For real execution on real hardware, JAM is a hang; our behaviour is equivalent from the emulator's point of view.
- **Tests live in every build type.** Profiling runs (like the Dormann clock-speed measurement) need Release optimisations; having tests always compiled means we don't maintain two configurations. If we later want a test-free binary for distribution, we can re-introduce `DOCTEST_CONFIG_DISABLE` behind a CMake option.
- **Effective clock ~735 MHz** — 96M cycles in 131 ms means ~1.36 ns per emulated 6502 cycle, or roughly 4 modern x86 cycles per emulated cycle on this ~3 GHz host. Reasonable for a switch-based interpreter with inlined config methods; consistent with BBC-Micro-scale deferred-sync emulators that target 100+ MHz effective.

## 2026-04-20 — Struct reorder + register caching: +57% perf to ~1155 MHz

### What we did
- Measured the impact of caching hot CPU state into `run_until` locals vs. keeping it on the struct. References to struct fields (no copy) ran at ~525-600 MHz — 30-35% slower than the stack-local baseline (~735 MHz). Reverted that quickly and kept locals.
- Reordered `M6502`'s layout: `cycle` (u64) first at offset 0 so its hot load is `mov (%rdi), ...` with no displacement; `brk_flags` packed at offset 15 right after `p` to eliminate a padding byte; remaining u16 fields (`tstate`, `base_addr`, `pending_addr`) follow at 16/18/20. Total struct size unchanged (24 bytes before config) but layout is denser.
- Introduced `auto pc = this->pc;` as a local in `run_until`, shadowing the member. The dispatch macros already reference bare `pc` after the `this->` strip (per CLAUDE.md convention), so they now write to the local instead of storing to the struct field on every cycle. **+52% on its own** (735 → 1115 MHz).
- Added a `detail::RegView` struct holding references to `a`, `x`, `y`, `s`, `p` locals. All op-class call sites (23 of them) now pass `view` instead of `*this`. Op classes stay template-generic (`struct Lda { template <typename C> static void apply(C &c, ...)`) and continue to work for both `M6502<Config>` (struct members) and `RegView` (refs to locals) via duck typing. **+4% on top of pc caching** (1115 → 1155 MHz).
- Cumulative measurement: **~1155 MHz effective clock** on x86-64 Release, consistent across 10-run averages. **+57% over the previous committed baseline** (~735 MHz). ~870 ps per emulated 6502 cycle, ~2.6 x86 cycles per emulated cycle at 3 GHz.

### Design decisions
- **Locals over struct fields for hot state.** The compiler can't prove that `config.read_*` / `write_*` calls don't alias other members of `*this` (because `config` IS a member of `*this`), so with struct-field access every hot variable reloads from memory after every config call. Locals live in registers across the whole loop and write back once at `exit:`.
- **Struct reorder is cosmetic perf.** The cycle-at-offset-0 save is a 1-byte-shorter instruction encoding; the reordering of `brk_flags` reclaims 1 byte of padding. Negligible perf impact on its own, but strictly better so worth doing.
- **`pc` cached via shadowing the member name.** The local is declared `auto pc = this->pc;`. Bare `pc` in macros and case bodies resolves to the local (shadowing), while the save-back at `exit:` uses the fully-qualified `this->pc = pc;` to disambiguate (the only place `this->` survives in this file, allowed by CLAUDE.md since we say "no *mandatory* `this->`"). The alternative — renaming the local to `pc_local` or similar — would require editing every macro; the shadow is the minimal-diff form.
- **`RegView` generalises the shadow trick for a/x/y/s/p.** Op classes take a `C&` template parameter; whether `C` is `M6502<Config>` or `RegView` doesn't matter to the op classes (they just use `c.a`, `c.p`, etc.). The dispatch switch constructs one `RegView view{a, x, y, s, p};` at the top of `run_until` and passes `view` into op classes. SROA'd away by the optimizer so it compiles to direct local accesses.
- **Diminishing returns past `pc`.** `pc` is touched on nearly every cycle (increment in operand fetches, update in jumps/branches); the other registers only by specific ops. So pc alone captured most of the win. Caching a/x/y/s/p still yields ~4% — worth the small amount of extra code because the wrapper is tidy and the op classes don't change.
- **Removed `this->` throughout the dispatch macros.** Initially inherited from the user's sketch; CLAUDE.md already says `this->` isn't mandatory in our style, so stripping it brings the macros in line and makes the shadowing work naturally (`pc` in a macro is the local when the caller has one, the member when it doesn't).

## 2026-04-21 — `RegView` refs → single `Registers&`: +9% to ~890 MHz

### What we did
- Replaced `detail::RegView` (struct of five `uint8_t&` members) with `detail::Registers` — a plain struct of values (`a, x, y, s, p`).
- Dropped the `template <typename C>` parameter on every op class. Each op now takes `detail::Registers& r` directly — no duck-typing, no instantiation per call type.
- `detail::set_nz` and `detail::set_flag` likewise take `Registers&` instead of `uint8_t& p`, for consistency with the ops.
- In `run_until`, the five scalar `a/x/y/s/p` locals collapse into one `auto r = detail::Registers{this->a, this->x, this->y, this->s, this->p};`. Every macro body that used bare `a`, `x`, `y`, `s`, `p` now uses `r.a`, `r.x`, etc. `pc` stays as its own scalar local — it's the only register that doesn't live in `Registers`.
- Tests are unchanged — `M6502` still exposes `a`, `x`, `y`, `s`, `p`, `pc` as individual struct fields; `run_until` does the local-aggregation only for the duration of the loop and writes the fields back at `exit:`.
- Measurements (Release, 10-run median over the Dormann profile): ~890 MHz (range 816–960). Prior baseline (RegView refs): ~815 MHz. **+9% at the median.**

### What we tried and walked away from
Before landing on `Registers&`, the session explored two other shapes, both discarded:

- **Per-op macros replacing `detail::` entirely.** Every mnemonic became a `TAWNY_OP_<CAT>_<NAME>(R, VAL)` macro dispatched via token-paste. Worked and tested clean, but didn't improve over this `Registers&` design and made the header substantially harder to read. Reverted.
- **`Registers` by value + `{value, flags}` return.** Ops took a `Registers` by value and returned an updated copy; call sites packed/unpacked at every invocation. Should in theory SROA cleanly but measured ~750 MHz — noticeably worse than either RegView or `Registers&`. The optimizer treats each aggregate construction/destructuring as its own local sequence and doesn't coalesce across adjacent op calls as well as it does with a persistent in-place aggregate. Reverted.
- **`set_nz`/`set_flag` as macros instead of inline functions.** Measured ~15% slower than the `[[gnu::always_inline]]` function form. Cause: macros evaluate `VAL`/`BIT` twice (in the two halves of the expression), and the compiler makes different register-allocation choices for duplicated subexpressions even though it could CSE them. Reverted.

### Design decisions
- **Why `Registers&` beats `RegView`.** `RegView` held five independent references to five independent scalar locals. Even with SROA, the compiler has to track each reference separately — from its point of view the ops take five distinct pointer-like arguments. With a single `Registers&` pointing at one aggregate local, SROA decomposes the struct into scalar registers once and inlining folds the reference away; the ops see a coherent unit rather than five loose pointers. Fewer constraints on register allocation for the optimizer to carry around.
- **Why ops can't be "pure" (in/out by value) without pack/unpack cost.** Ops are functions, not macros — they can't mutate the caller's locals directly. The options are (a) pass-by-ref + void return, or (b) pass-by-value + return an updated aggregate. The by-value form requires the caller to destructure the return back into the locals, and the optimizer handles this chain of `construct → call → destructure` less cleanly than a persistent in-place aggregate. Ref+void won the measurement.
- **Decimal ADC/SBC stay inline.** At one point they were factored out as `[[gnu::cold]] [[gnu::noinline]]` slow paths (to avoid polluting hot-path codegen if the compiler spilled `r` to memory for the call). In this `Registers&` design the decimal paths are inlined back into `Adc::apply` / `Sbc::apply` and simply branch on `r.p & flag::D`. Branch prediction keeps them out of the hot path on the Dormann test (D never set); no observable cost.
- **`set_nz`/`set_flag` taking `Registers&` vs `uint8_t&`.** Measured identical perf after inlining — the call sites resolve to the same IR either way. Kept the `Registers&` form for consistency with the op classes: everything in `detail::` that touches the register file now takes the same parameter type.

## 2026-04-21 — `M6502::set_pc()` for bootstrap without the reset sequence

### What we did
- Added `auto set_pc(std::uint16_t target) -> void` on `M6502`. It sets `pc` and `pending_addr` to `target`, clears `brk_flags`, parks `tstate` at the synthetic value `0x7FF`, and pre-pays the cost of the upcoming opcode fetch via `config.access_cost_opcode(target)`. Registers (A/X/Y/S/P) are left untouched — the caller is responsible for setting them up if the target code relies on specific values.
- Added a matching case to the `run_until` switch: `TAWNY_FETCH_OPCODE_CASE(0xFF, 7)` at the end of the opcode table, just before `default:`. This expands to `case 0x7FF` with the standard opcode-fetch body (read opcode at `addr`, dispatch, pay cost of operand fetch). `0xFF` step 7 is a safe synthetic slot because the longest real instruction is 7 cycles (steps 0–6), so no regular dispatch ever produces tstate 0x7FF.
- Updated the Dormann functional-test runner to use `cpu.set_pc(tawny::dormann::entry_addr)` instead of writing the reset vector at `$FFFC/$FFFD`. The emulator skips the 7-cycle reset sequence entirely; the Dormann cycle count drops from 96249822 to 96249816 — the six-cycle delta matches the BRK-reset sequence overhead the test didn't actually rely on (the test initialises S and flags itself immediately).

### Design decisions
- **Why a synthetic tstate instead of a flag on the CPU struct.** We could have added a `bootstrap_from_pc` bit checked at the top of `run_until`, but the synthetic-tstate approach reuses the dispatch infrastructure we already have. The run loop has exactly one entry point: `switch (tst)`. Everything else is a case. Adding a bootstrap "instruction" at tstate `0x7FF` keeps `run_until`'s hot path branch-free on bootstrap — the bootstrap fetch is just another case that falls off the jump table.
- **0x7FF because step 7 is structurally unused.** The `tstate = (opcode << 3) | step` encoding has 3 bits for step, allowing step values 0–7. The longest NMOS 6502 instruction is 7 cycles (BRK / JSR / RTS / RTI / RMW abs,X / BRK-based reset), which is steps 0–6. Step 7 is never produced. Picking opcode 0xFF step 7 — the very top of the range — is a clear "synthetic" signal and won't collide with any future opcode we add. (This is the same trick used in the Rust reference implementation.)
- **`set_pc` leaves registers untouched.** Two options: copy reset's init (`a = x = y = 0; s = 0; p = flag::U`) or preserve whatever's there. Preserved won. `set_pc` is the "jump PC without going through reset" primitive; callers who want a reset-like state can call `reset()` first, then `set_pc()`. For the Dormann test specifically the test initialises its own registers (`LDX #$FF; TXS; CLD`) from the very first instructions, so the starting register state doesn't matter.
- **Pre-paying the opcode fetch mirrors `reset()`.** Reset sets `cycle = config.access_cost(pending_addr).cost` so the first `run_until` iteration jumps straight into phi2 with no bootstrap branch. `set_pc` does the same thing but uses `access_cost_opcode` since the first phi2 on the set_pc path is an opcode fetch rather than a generic read. Without the pre-pay, the opcode-fetch cost wouldn't be charged — the synthetic case only pays for the *next* access (the operand fetch) via `TAWNY_STEP_TAIL`.

## 2026-04-21 — Fall-through dispatch for optional steps: `abs,X/Y`, `(zp),Y`, branches

### What we did
Rewrote three macros so that the *variable-cycle* steps — page-cross fix-ups on indexed reads, and the three-way branch dispatch — no longer go through the switch's jump table on the common (no-penalty) path. Case labels for the penalty cycles now live *inside* the `if` branches that require them, so the common path falls straight through to the next step with no `tst` write, no `break`, and no indirect re-dispatch:

- **`TAWNY_AB_INDEXED_READ`** (`LDA abs,X`, `AND abs,Y`, every `abs,X/Y` read). Step 1 used to set `tst = step 2` (page cross) or `tst = step 3` (no cross) and then `break` out of the switch, letting the outer loop re-dispatch. Now the step-2 case label is nested inside the `if (_lo_sum > 0xFFu)` branch; step 1's no-cross path just falls through to step 3.
- **`TAWNY_IZY_READ`** (`LDA (zp),Y`, every `(zp),Y` read). Same optimisation, applied to step 2's page-cross check.
- **`TAWNY_REL_BRANCH`** (all conditional branches). Originally a three-way dispatch: not-taken → step 3, taken-no-cross → step 1, taken-cross → step 2. Now all three outcomes are expressed inline in step 0's `if / else if / else`, with the step-1 and step-2 case labels nested inside the relevant branches. Not-taken branches (the most common case for many loops) fall straight through to the fetch step with no re-dispatch at all.

All 15 test cases / 74 assertions still pass. Perf reading deferred until we can measure on AC.

### Design decisions
- **Case labels inside `if` blocks.** Legal in both C and C++ — the switch's jump table just needs to know the label's address; it can live anywhere inside the switch body. This is Duff's-device territory, rarely seen in idiomatic code but exactly the right tool here. Mid-instruction re-entry dispatches to the label directly, bypassing the enclosing `if` condition (which we don't need to re-check because the state-machine step implies the branch).
- **Narrow scopes to avoid "jump bypasses variable initialization".** The C++ rule: you can't jump past a declaration-with-initialisation to a point where that variable is in scope, unless the type is scalar *and* the variable has no initialiser. The page-cross branches compute intermediate values (`_hi`, `_lo_sum`, `_off`, `_tgt`, `_wrong`) with `auto` initialisers. To let the nested case labels jump over these, we put the computations in an inner block scope so the variables go out of scope before the case label is reached. The `_cross` / `_taken` booleans that carry information across the block boundary are declared without initialisers (scalar, no init = legal to bypass) and always assigned before being read on any path that reads them.
- **Branch encoding merges `_cross` into the outer `if / else if / else`.** The first sketch had nested ifs (`if (taken) { if (cross) … else … }`), which works but complicates the jump-bypass analysis because the case labels are nested two deep. Flattening to a three-way `if / else if / else` puts step-1 and step-2 case labels at the same block level; cleaner and easier for the compiler to reason about.
- **Why this optimisation is load-bearing.** The Dormann functional test exercises `LDA/STA/CMP abs,X/Y`, `LDA/STA (zp),Y`, and every conditional branch heavily — many hundreds of millions of times across the 96M-cycle run. Each hot-path case body previously ended in an indirect jump back to the top of the switch; now it ends in a fall-through (a straight-line step or `jmp` to the immediately-following case body). Removes an indirect branch per instruction on a huge fraction of all dispatches.

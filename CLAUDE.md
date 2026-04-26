# Tawny - BBC Micro Emulator (C++)

## Project overview
A BBC Micro emulator written in C++, using GLFW + OpenGL for rendering.
Named after the tawny owl — the BBC Micro logo is a stylised owl made from dots.

Rewrite of the Rust version (~/dev/tawny/), taking a lazy catch-up (deferred synchronisation) approach to emulation.

## Naming conventions
- **Types (struct/class/enum):** `UpperCamelCase` — `Cpu`, `ModelB`, `ChipSelect`
- **Functions, variables, members:** `snake_case` — `tick()`, `chip_select`, `program_counter`
- **Constants & enum values:** `UpperCamelCase` — `enum class Flag { Carry, Zero }`
- **Files:** `snake_case` — `model_b.cpp`, `mos_6502.h`
- **Template parameters:** `UpperCamelCase`
- No trailing underscore on members for now (may revisit)
- No mandatory `this->` (may revisit)

## Literals
- Hex literals use **uppercase** digits: `0xFF`, `0x3489`, not `0xff`
- Keep the `0x` prefix lowercase

## Initialization
- Use **uniform initialization** (braces) everywhere: `int x{};`, `Foo bar{a, b};`, `M6502 cpu{DormannCpuConfig{mem}};`
- Prevents narrowing conversions, works consistently across all types (aggregates, constructors, default init)

## Casts and arithmetic
- Prefer `++x` / `--x` / `x += 1` / `x -= 1` over `x = static_cast<T>(x + 1)` forms. Compound assignment allows the implicit narrowing that plain `=` would warn on.
- **Drop redundant widening casts.** `u8 -> u16` is implicit and safe. Only the final *narrowing* cast needs to be explicit.
- **Drop redundant inner casts before a shift.** `uint8_t v << N` already integer-promotes `v` to `int` for the shift; writing `static_cast<std::uint16_t>(v) << N` is noise. Only the outer `static_cast<std::uint16_t>(v << N)` that narrows the int back to u16 is needed.

## Brace style
One true brace style (K&R variant): opening brace on same line for control flow, classes, structs, namespaces, lambdas, etc. **Function definitions** are the exception — their opening brace goes on a new line.

```cpp
int compute(int x)
{
    if (x > 0) {
        return x * 2;
    }
    return 0;
}
```

## Macros
- **All line-continuation backslashes in the same file align to a single column** (currently 79 in `m6502.h`). Keep it consistent — if a new macro body pushes the max width past the current column, re-align the whole file.
- For dispatch macros: one logical line = one line of text. Prefer single-line bodies for trivial cases; reserve multi-line `do { ... } while (0)` for macros that take multiple statements.

## Build and warnings
- Builds with **`-Wall -Wextra -Werror`** (and `/W4 /WX` on MSVC). Warnings are errors; fix them, don't suppress them.

## Development rules
- Write modern C++23, prefer value semantics and const correctness
- Use std algorithms/ranges where possible, no raw loops
- Use a functional style (variants, optional, monadic API) where it is not detrimental to performance
- Almost always auto, trailing return types
- Explain implementation options and let the user choose
- Performance first: avoid unnecessary allocations, prefer flat data structures
- Write tests where appropriate, ensure they pass
- Keep CLAUDE.md and JOURNAL.md up to date
- Never commit or push to git without explicit user approval — always wait for the user to review changes first

## Tech stack
- **Language:** C++23
- **Windowing:** GLFW 3.4
- **Rendering:** OpenGL 3.3 Core (via GLAD)
- **Testing:** doctest (single-header, in extern/)
- **Build:** CMake 3.20+

## Project structure
- `src/main.cpp` — Application entry point, GLFW window, OpenGL context, doctest integration
- `src/emulator/` — Emulation core
  - `m6502.h` — MOS 6502 CPU: `M6502Config` concept, `M6502<Config>` template, full NMOS opcode set (all 151 legal + 93 illegal implemented; 12 true KIL/HLT opcodes and 5 SH*/TAS unstable variants left as JAM stubs). Deferred-sync architecture: `run_until(horizon)` runs until a simulated-time budget and resumes mid-instruction. Fall-through switch dispatch (one case = one cycle); hot legal opcodes frequency-sorted for i-cache locality; `set_pc(addr)` bootstrap path via synthetic tstate `0x7FF`. IRQ/NMI: a single `int_cycle` local in `run_until` (computed once from `config.irq_asserted_since() / nmi_edge_at()`); FETCH does `take_int = int_cycle <= current` branchlessly; SEI/CLI/PLP/RTI/branches use specific FETCH variants for I-flag delay (DEFER_I) / immediate (IMMEDIATE_I) / branch eaten-IRQ (BRANCH_NOCROSS) quirks; BRK step 0 deduces brk_flags from config + step 3 re-checks for NMI hijack.
- `test/` — Heavyweight test files (compiled into the same executable)
  - `test/dormann/` — Dormann / Bruce Clark test images + shared `DormannCpuConfig`. Three binaries: `functional_test_bin.h` (all-opcodes, profiled), `decimal_test_bin.h` (Bruce Clark decimal-mode ADC/SBC sweep), `interrupt_test_bin.h` (IRQ/NMI test — skipped until interrupts land; carries `feedback_port` / `feedback_*_bit` / `feedback_write_mask` MMIO constants for the eventual harness). Namespaced `tawny::dormann::{decimal,interrupt}`.
  - `test/emulator/` — CPU unit tests + functional-test profiling runner
- `extern/` — Vendored dependencies (doctest, glad/)

## What still needs doing on the CPU core
- **SH* / TAS unstable illegals** (0x93, 0x9B, 0x9C, 0x9E, 0x9F). Currently JAM. They need the pre-index high byte preserved across the write step (for the `reg & (H+1)` store value). Would require an extra scratch field on `M6502` or a dedicated macro that stashes the value in the spare high byte of `base`. BBC Micro never used them.
- **`reset()` as a public operation.** Currently only the constructor calls it. A `cpu.reset()` that resets mid-run would need to reset registers and re-enter BRK microcode with `brk_flags = Reset`.
- **Dormann interrupt-test wiring.** The IRQ/NMI core is in place and unit-tested for all 6502 quirks (SEI/CLI/PLP/RTI delay, taken-no-cross branch eats IRQ, NMI hijack, NMI > IRQ, sequences-don't-poll). The Dormann interrupt test still doesn't run because it pokes a `$BFFC` MMIO register mid-`run_until` to drive IRQ/NMI, but the deferred-sync model samples interrupt state only at `run_until` boundaries. Plumbing options: (a) the config sets `stop=true` on `$BFFC` writes, returning to the harness for a fresh `run_until`; (b) a re-query hook in FETCH; (c) a config-side cycle counter so the assertion times propagate via the existing concept methods.
- **RDY / SO pins.** Rare on BBC Micro; not implemented.
- **65C02 / 65816 extensions.** Different CPU, separate emulator if needed.

## Testing
- Single executable: `./tawny --test` runs tests, otherwise runs the emulator
- In-source tests: write `TEST_CASE` in any .cpp file in src/
- Standalone tests: add .cpp files to test/
- Tests run automatically as a post-build step
- Tests compile in **every** build type (no DOCTEST_CONFIG_DISABLE gate) — Release is preferred for profiling/benchmark tests like the Dormann functional-test runner

## Build & run
```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/tawny
```

## Tests
```sh
./build/tawny --test
```

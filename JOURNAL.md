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

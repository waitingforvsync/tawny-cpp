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

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
- `test/` — Heavyweight test files (compiled into the same executable)
- `extern/` — Vendored dependencies (doctest.h, glad/)

## Testing
- Single executable: `./tawny --test` runs tests, otherwise runs the emulator
- In-source tests: write `TEST_CASE` in any .cpp file in src/
- Standalone tests: add .cpp files to test/
- Tests run automatically as a post-build step
- Release builds strip all test code via `DOCTEST_CONFIG_DISABLE`

## Build & run
```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/tawny
```

## Tests
```sh
./build/tawny --test
```

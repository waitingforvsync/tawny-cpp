# Tawny

A BBC Micro emulator written in C++.

Named after the tawny owl — the BBC Micro logo is a stylised owl made from dots.

This is a rewrite of the [Rust version](../tawny/), taking a lazy catch-up (deferred synchronisation) approach.

## Status

Early development. Currently opens a window with an OpenGL-rendered surface.

## Building

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Tests run automatically as a post-build step. To run them manually:

```sh
./build/tawny --test
```

## Tech stack

- **C++20**
- **GLFW** — windowing and input
- **GLAD** — OpenGL 3.3 Core loader
- **doctest** — unit testing (in-source and standalone)
- **CMake** — build system

## License

MIT

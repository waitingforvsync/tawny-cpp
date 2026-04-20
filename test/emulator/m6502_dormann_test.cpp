#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include "emulator/m6502.h"
#include "dormann/dormann_cpu_config.h"
#include "dormann/functional_test_bin.h"

// Run the Klaus Dormann functional test end-to-end and profile the effective
// clock speed over multiple runs. Compiled into the same test binary — run
// `./tawny --test -tc="*Dormann*"` to target just this case, or just run the
// post-build step (which runs the full test binary).
//
// Expected pass point: cpu.pc == tawny::dormann::success_addr (= $3489, the
// "if you get here everything went well" JMP * trap the test hits on success).
// Any other trapping pc indicates a specific failing subtest — see the
// Dormann source around that address to identify it.
TEST_CASE("Dormann: 6502 functional test passes + profile clock speed") {
    constexpr int runs = 10;

    std::uint64_t         total_cycles{};
    std::chrono::nanoseconds total_wall{};

    for (int i = 0; i < runs; ++i) {
        // Fresh CPU + fresh RAM per run. `DormannCpuConfig`'s constructor
        // allocates a zero-initialised 64K buffer; we write the test binary
        // at load_addr and set the reset vector to entry_addr.
        tawny::dormann::DormannCpuConfig cfg{};
        std::memcpy(
            cfg.mem.get() + tawny::dormann::load_addr,
            tawny::dormann::functional_test_bin,
            sizeof(tawny::dormann::functional_test_bin));
        cfg.mem[0xFFFC] = static_cast<std::uint8_t>(tawny::dormann::entry_addr & 0xFFu);
        cfg.mem[0xFFFD] = static_cast<std::uint8_t>(tawny::dormann::entry_addr >> 8);

        tawny::M6502 cpu{std::move(cfg)};

        auto start = std::chrono::steady_clock::now();
        // Horizon large enough that the only way we return is via the trap
        // detection (access_cost_opcode sets stop when the CPU re-fetches the
        // same opcode address — i.e. hits a JMP * / BXX * loop).
        auto final_cycle = cpu.run_until(1ull << 40);
        auto end   = std::chrono::steady_clock::now();

        total_cycles += final_cycle;
        total_wall   += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

        // pc == success_addr on pass; any other value is a specific failure
        // trap. Both exit via the same mechanism, so the profile numbers
        // above are still meaningful on failure.
        CHECK(cpu.pc == tawny::dormann::success_addr);
    }

    auto   seconds_total = static_cast<double>(total_wall.count()) / 1e9;
    double avg_seconds   = seconds_total / runs;
    double avg_cycles    = static_cast<double>(total_cycles) / runs;
    double mhz           = (avg_cycles / avg_seconds) / 1e6;

    std::printf(
        "\nDormann functional test — profile over %d runs:\n"
        "  Avg cycles/run:   %.0f\n"
        "  Avg wall time:    %.3f s\n"
        "  Effective clock:  %.2f MHz  (%.1fx a real 2 MHz 6502)\n\n",
        runs, avg_cycles, avg_seconds, mhz, mhz / 2.0);
}

#pragma once

#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace tawny {

// The bus/memory interface the CPU uses to reach the outside world.
//
// Reads and writes are split by access kind so the host can pick a fast path
// where one is known to exist:
//   - read_opcode / access_cost_opcode — instruction fetch; may use a prefetch
//                                        cache or pre-decoded page for hot code
//   - read_zp, read_stack, write_zp, write_stack — page 0 and page 1. On the
//                                        BBC Micro these can only hit RAM, so
//                                        MMIO dispatch is skipped.
//   - read_vector                      — reset/IRQ/NMI vector fetch from $FFFA+
//   - read, write                      — anything else; full decode path.
//
// access_cost_* returns the number of 2 MHz cycles the access takes. On BBC
// Micro a 1 MHz peripheral access stretches phi0 and costs 2 or 3 cycles
// instead of the usual 1; ZP/stack/opcode/vector are always 1.
//
// stop_requested lets the outside world cut a timeslice short. The CPU polls
// it once per instruction — so when true, run_for returns as soon as the
// current instruction finishes. Uses: infinite-loop trap detection in tests,
// UI pause, breakpoint hit, frame sync, etc.
template <typename T>
concept M6502Config = requires(T &cfg,
                               std::uint16_t addr,
                               std::uint8_t zp,
                               std::uint8_t val) {
    { cfg.read_opcode(addr)        } -> std::same_as<std::uint8_t>;
    { cfg.read_zp(zp)              } -> std::same_as<std::uint8_t>;
    { cfg.read_stack(zp)           } -> std::same_as<std::uint8_t>;
    { cfg.read(addr)               } -> std::same_as<std::uint8_t>;
    { cfg.read_vector(addr)        } -> std::same_as<std::uint8_t>;
    { cfg.write_zp(zp, val)        } -> std::same_as<void>;
    { cfg.write_stack(zp, val)     } -> std::same_as<void>;
    { cfg.write(addr, val)         } -> std::same_as<void>;
    { cfg.access_cost_opcode(addr) } -> std::convertible_to<unsigned>;
    { cfg.access_cost_zp(zp)       } -> std::convertible_to<unsigned>;
    { cfg.access_cost_stack(zp)    } -> std::convertible_to<unsigned>;
    { cfg.access_cost_vector(addr) } -> std::convertible_to<unsigned>;
    { cfg.access_cost(addr)        } -> std::convertible_to<unsigned>;
    { cfg.stop_requested()         } -> std::convertible_to<bool>;
};

// MOS 6502 emulator, deferred-synchronisation style.
//
// The CPU runs ahead of the rest of the system in timeslices. run_for(n) keeps
// executing whole instructions until at least n cycles have elapsed, and
// returns the number it actually consumed (>= n, by up to one instruction's
// worth).
template <M6502Config Config>
struct M6502 {
    // Registers
    std::uint16_t pc{};
    std::uint8_t a{};
    std::uint8_t x{};
    std::uint8_t y{};
    std::uint8_t s{0xFF};
    std::uint8_t p{};

    // Bus interface — held by value so the compiler can inline aggressively.
    // For stateful buses, Config typically holds a pointer/view (e.g. std::span)
    // into the real bus object.
    Config config;

    explicit M6502(Config cfg) noexcept(std::is_nothrow_move_constructible_v<Config>)
        : config(std::move(cfg)) {}

    auto run_for(std::uint32_t cycles) -> std::uint32_t;

    auto reset() -> void;
    auto irq() -> void;
    auto nmi() -> void;
};

}  // namespace tawny

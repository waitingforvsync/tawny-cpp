#pragma once

#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace tawny {

using Cycle = std::uint64_t;

// Returned by every `access_cost_*` method on the Config.
//
//   cost — the real cycle cost of the access (in 2 MHz ticks). Always the
//          honest answer; overloading 0 as a stop sentinel is NOT supported.
//   stop — when true, asks `run_until` to break out after advancing the clock
//          but before performing the upcoming phi2. The pending bus op is
//          preserved, so re-entry resumes exactly at that deferred phi2.
//          Uses: infinite-loop trap detection in tests, UI pause, breakpoint,
//          MMIO-triggered scheduler hand-off.
//
// `access_cost_*` MUST be pure / idempotent — any state update belongs on the
// matching `read_*` / `write_*` path, not here.
struct AccessCost {
    unsigned cost;
    bool stop;
};

// BRK entry cause. Bitmask so interrupts can in future be raised concurrently
// and priorities resolved at dispatch time. (Stage 1 uses Reset only; IRQ/NMI
// dispatch is a follow-up.)
enum class BrkFlags : std::uint8_t {
    None  = 0,
    Reset = 1,
    Irq   = 2,
    Nmi   = 4,
};

constexpr auto operator|(BrkFlags a, BrkFlags b) -> BrkFlags
{
    return static_cast<BrkFlags>(
        static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr auto operator&(BrkFlags a, BrkFlags b) -> BrkFlags
{
    return static_cast<BrkFlags>(
        static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

// The bus/memory interface the CPU uses to reach the outside world.
//
// Reads and writes are split by access kind so the host can pick a fast path
// where one is known to exist (ZP / stack / opcode / vector on BBC Micro are
// always RAM, skipping MMIO dispatch).
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
    { cfg.access_cost_opcode(addr) } -> std::same_as<AccessCost>;
    { cfg.access_cost_zp(zp)       } -> std::same_as<AccessCost>;
    { cfg.access_cost_stack(zp)    } -> std::same_as<AccessCost>;
    { cfg.access_cost_vector(addr) } -> std::same_as<AccessCost>;
    { cfg.access_cost(addr)        } -> std::same_as<AccessCost>;
};

// Status-register flag bits (P).
namespace flag {
    constexpr std::uint8_t C = 0x01;
    constexpr std::uint8_t Z = 0x02;
    constexpr std::uint8_t I = 0x04;
    constexpr std::uint8_t D = 0x08;
    constexpr std::uint8_t B = 0x10;
    constexpr std::uint8_t U = 0x20;
    constexpr std::uint8_t V = 0x40;
    constexpr std::uint8_t N = 0x80;
}  // namespace flag

namespace detail {

// Synthetic tstates outside the (opcode << 3 | step) range (0x000-0x7FF).
constexpr std::uint16_t ResetStep0       = 0x800;  // fetch vector low
constexpr std::uint16_t ResetStep1       = 0x801;  // fetch vector high + set pc
constexpr std::uint16_t ResetOpcodeFetch = 0x802;  // first opcode fetch

[[gnu::always_inline]] inline auto set_nz(std::uint8_t &p, std::uint8_t val) -> void
{
    p = static_cast<std::uint8_t>((p & ~(flag::N | flag::Z))
                                   | (val & flag::N)
                                   | (val == 0 ? flag::Z : 0));
}

[[gnu::always_inline]] inline auto set_flag(std::uint8_t &p, std::uint8_t bit, bool on) -> void
{
    p = static_cast<std::uint8_t>(on ? (p | bit) : (p & ~bit));
}

// Op classes — small structs providing the per-mnemonic transform. Reused
// across addressing-mode macros. All methods templated on the CPU type so
// they compose with the templated M6502<Config>.

struct Nop {
    template <typename Cpu> static void apply(Cpu &) {}
};

struct Inx {
    template <typename Cpu> static void apply(Cpu &cpu)
    {
        cpu.x = static_cast<std::uint8_t>(cpu.x + 1);
        set_nz(cpu.p, cpu.x);
    }
};

struct Lda {
    template <typename Cpu> static void apply(Cpu &cpu, std::uint8_t v)
    {
        cpu.a = v;
        set_nz(cpu.p, v);
    }
};

struct Cmp {
    template <typename Cpu> static void apply(Cpu &cpu, std::uint8_t v)
    {
        auto result = static_cast<std::uint8_t>(cpu.a - v);
        set_nz(cpu.p, result);
        set_flag(cpu.p, flag::C, cpu.a >= v);
    }
};

struct AdcBin {
    template <typename Cpu> static void apply(Cpu &cpu, std::uint8_t v)
    {
        unsigned a    = cpu.a;
        unsigned m    = v;
        unsigned cin  = (cpu.p & flag::C) ? 1u : 0u;
        unsigned sum  = a + m + cin;
        bool     vflg = ((~(a ^ m) & (a ^ sum)) & 0x80u) != 0;
        bool     cout = sum > 0xFFu;
        cpu.a = static_cast<std::uint8_t>(sum);
        set_nz(cpu.p, cpu.a);
        set_flag(cpu.p, flag::C, cout);
        set_flag(cpu.p, flag::V, vflg);
    }
};

struct Sta {
    template <typename Cpu> static auto value(const Cpu &cpu) -> std::uint8_t
    {
        return cpu.a;
    }
};

struct BneCond {
    template <typename Cpu> static auto taken(const Cpu &cpu) -> bool
    {
        return (cpu.p & flag::Z) == 0;
    }
};

}  // namespace detail

// -----------------------------------------------------------------------------
// Dispatch macros. All three compose inline inside M6502::run_until's switch.
//
// STEP_TAIL: the common per-cycle tail — query the next phi2's cost, advance
//            the clock, and either exit (horizon hit or stop signalled) or
//            continue. COST_EXPR is the config method call minus the `config.`
//            prefix, e.g. `access_cost_zp(addr)`. NEXT_TST is the tstate to
//            resume at on exit.
//
// The `goto exit` jumps to the save-state block at the end of run_until. Any
// non-last step reaches its trailing `[[fallthrough]]` only when the horizon
// hasn't been hit; the next case's body runs in the same switch entry.
// -----------------------------------------------------------------------------

#define TAWNY_STEP_TAIL(COST_EXPR, NEXT_TST)            \
    do {                                                \
        auto _ac = config.COST_EXPR;                    \
        current += _ac.cost;                            \
        if (_ac.stop) horizon = current;                \
        if (current >= horizon) {                       \
            tst = static_cast<std::uint16_t>(NEXT_TST); \
            goto exit;                                  \
        }                                               \
    } while (0)

// FETCH_OPCODE_CASE — the last step of every instruction. Reads the next
// opcode byte (via read_opcode for the sync-read semantics), shifts it into a
// step-0 tstate for the new instruction, sets up the operand-fetch address,
// and breaks out so the while loop re-enters the switch at the new tstate.

#define TAWNY_FETCH_OPCODE_CASE(OPCODE, STEP)                              \
    case ((OPCODE) << 3) | (STEP): {                                       \
        this->pc = static_cast<std::uint16_t>(this->pc + 1);               \
        tst  = static_cast<std::uint16_t>(                                 \
                   static_cast<std::uint16_t>(config.read_opcode(addr))    \
                   << 3);                                                  \
        addr = this->pc;                                                   \
        TAWNY_STEP_TAIL(access_cost(addr), tst);                           \
        break;                                                             \
    }

// IMPLIED(OPCODE, OP_CLASS) — 2 cycles. Step 0 is a discarded operand-fetch
// read; pc is NOT incremented (implied ops consume no bytes after the opcode).

#define TAWNY_IMPLIED(OPCODE, OP_CLASS)                                    \
    case ((OPCODE) << 3) | 0: {                                            \
        (void)config.read(addr);                                           \
        OP_CLASS::apply(*this);                                            \
        addr = this->pc;                                                   \
        TAWNY_STEP_TAIL(access_cost_opcode(addr), ((OPCODE) << 3) | 1);    \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 1)

// IMM_READ(OPCODE, OP_CLASS) — 2 cycles. Step 0 reads the immediate byte and
// applies the op; pc advances past it.

#define TAWNY_IMM_READ(OPCODE, OP_CLASS)                                   \
    case ((OPCODE) << 3) | 0: {                                            \
        this->pc = static_cast<std::uint16_t>(this->pc + 1);               \
        OP_CLASS::apply(*this, config.read(addr));                         \
        addr = this->pc;                                                   \
        TAWNY_STEP_TAIL(access_cost_opcode(addr), ((OPCODE) << 3) | 1);    \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 1)

// ZP_READ(OPCODE, OP_CLASS) — 3 cycles.
//   step 0: read ZP-addr byte, stash in addr (high bits 0).
//   step 1: read value from ZP, apply op.
//   step 2: opcode fetch for next instruction.

#define TAWNY_ZP_READ(OPCODE, OP_CLASS)                                    \
    case ((OPCODE) << 3) | 0: {                                            \
        this->pc = static_cast<std::uint16_t>(this->pc + 1);               \
        addr = config.read(addr);                                          \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 1);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        OP_CLASS::apply(*this,                                             \
            config.read_zp(static_cast<std::uint8_t>(addr)));              \
        addr = this->pc;                                                   \
        TAWNY_STEP_TAIL(access_cost_opcode(addr), ((OPCODE) << 3) | 2);    \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 2)

// ZP_WRITE(OPCODE, OP_CLASS) — 3 cycles.
//   step 0: read ZP-addr byte.
//   step 1: write op_class::value(cpu) to ZP.
//   step 2: opcode fetch for next instruction.

#define TAWNY_ZP_WRITE(OPCODE, OP_CLASS)                                   \
    case ((OPCODE) << 3) | 0: {                                            \
        this->pc = static_cast<std::uint16_t>(this->pc + 1);               \
        addr = config.read(addr);                                          \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 1);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        config.write_zp(static_cast<std::uint8_t>(addr),                   \
                        OP_CLASS::value(*this));                           \
        addr = this->pc;                                                   \
        TAWNY_STEP_TAIL(access_cost_opcode(addr), ((OPCODE) << 3) | 2);    \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 2)

// ABS_READ(OPCODE, OP_CLASS) — 4 cycles.
//   step 0: read addr lo (into base).
//   step 1: read addr hi, combine into addr = base | (hi << 8).
//   step 2: read target value, apply op.
//   step 3: opcode fetch for next instruction.

#define TAWNY_ABS_READ(OPCODE, OP_CLASS)                                   \
    case ((OPCODE) << 3) | 0: {                                            \
        this->pc = static_cast<std::uint16_t>(this->pc + 1);               \
        base = config.read(addr);                                          \
        addr = this->pc;                                                   \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        this->pc = static_cast<std::uint16_t>(this->pc + 1);               \
        addr = static_cast<std::uint16_t>(                                 \
            (base & 0x00FFu) |                                             \
            (static_cast<std::uint16_t>(config.read(addr)) << 8));         \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 2);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        OP_CLASS::apply(*this, config.read(addr));                         \
        addr = this->pc;                                                   \
        TAWNY_STEP_TAIL(access_cost_opcode(addr), ((OPCODE) << 3) | 3);    \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 3)

// ABS_JUMP(OPCODE) — JMP abs, 3 cycles. No op class; updates pc in place.
//   step 0: read addr lo (into base).
//   step 1: read addr hi, set pc = (hi << 8) | (base & 0xFF).
//   step 2: opcode fetch at new pc.

#define TAWNY_ABS_JUMP(OPCODE)                                             \
    case ((OPCODE) << 3) | 0: {                                            \
        this->pc = static_cast<std::uint16_t>(this->pc + 1);               \
        base = config.read(addr);                                          \
        addr = this->pc;                                                   \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        this->pc = static_cast<std::uint16_t>(                             \
            (base & 0x00FFu) |                                             \
            (static_cast<std::uint16_t>(config.read(addr)) << 8));         \
        addr = this->pc;                                                   \
        TAWNY_STEP_TAIL(access_cost_opcode(addr), ((OPCODE) << 3) | 2);    \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 2)

// REL_BRANCH(OPCODE, COND) — 2/3/4 cycles. Variable paths forbid fall-through
// between steps; each step ends with `break` and sets tst explicitly.
//   step 0: read offset; if !taken, jump to step 3 (opcode fetch); if taken
//           without page cross, jump to step 1; if taken with page cross, stash
//           target in `base` and jump to step 2.
//   step 1: taken no cross — dummy read at target, then opcode fetch.
//   step 2: taken cross — dummy read at wrong page, set pc = base (correct
//           target), then opcode fetch.
//   step 3: opcode fetch for next instruction.

#define TAWNY_REL_BRANCH(OPCODE, COND)                                     \
    case ((OPCODE) << 3) | 0: {                                            \
        this->pc = static_cast<std::uint16_t>(this->pc + 1);               \
        auto _off = static_cast<std::int8_t>(config.read(addr));           \
        if (!COND::taken(*this)) {                                         \
            addr = this->pc;                                               \
            tst  = ((OPCODE) << 3) | 3;                                    \
            TAWNY_STEP_TAIL(access_cost_opcode(addr), tst);                \
            break;                                                         \
        }                                                                  \
        auto _tgt = static_cast<std::uint16_t>(                            \
            this->pc + static_cast<std::int16_t>(_off));                   \
        auto _wrong = static_cast<std::uint16_t>(                          \
            (this->pc & 0xFF00u) | (_tgt & 0x00FFu));                      \
        if (_wrong == _tgt) {                                              \
            this->pc = _tgt;                                               \
            addr = this->pc;                                               \
            tst  = ((OPCODE) << 3) | 1;                                    \
            TAWNY_STEP_TAIL(access_cost(addr), tst);                       \
            break;                                                         \
        }                                                                  \
        this->pc = _wrong;                                                 \
        base     = _tgt;                                                   \
        addr     = this->pc;                                               \
        tst      = ((OPCODE) << 3) | 2;                                    \
        TAWNY_STEP_TAIL(access_cost(addr), tst);                           \
        break;                                                             \
    }                                                                      \
    case ((OPCODE) << 3) | 1: {                                            \
        (void)config.read(addr);                                           \
        addr = this->pc;                                                   \
        tst  = ((OPCODE) << 3) | 3;                                        \
        TAWNY_STEP_TAIL(access_cost_opcode(addr), tst);                    \
        break;                                                             \
    }                                                                      \
    case ((OPCODE) << 3) | 2: {                                            \
        (void)config.read(addr);                                           \
        this->pc = base;                                                   \
        addr = this->pc;                                                   \
        tst  = ((OPCODE) << 3) | 3;                                        \
        TAWNY_STEP_TAIL(access_cost_opcode(addr), tst);                    \
        break;                                                             \
    }                                                                      \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 3)

// -----------------------------------------------------------------------------
// MOS 6502 emulator — deferred-synchronisation, fall-through dispatch.
//
// Horizon is exclusive: a cycle whose start time equals `horizon` is deferred
// to the next call. Each switch case is one 6502 cycle; within an instruction
// cases fall through to the next step, breaking only at the opcode fetch
// (re-dispatch) or when the horizon is reached mid-instruction.
// -----------------------------------------------------------------------------

template <M6502Config Config>
struct M6502 {
    // Registers.
    std::uint16_t pc{};
    std::uint8_t  a{};
    std::uint8_t  x{};
    std::uint8_t  y{};
    std::uint8_t  s{0xFD};
    std::uint8_t  p{flag::I | flag::U};

    // Emulation state.
    std::uint16_t tstate{detail::ResetStep0};   // (ir << 3) | step, or synthetic
    std::uint16_t base_addr{};                  // scratch across multi-step ops
    BrkFlags      brk_flags{BrkFlags::Reset};

    // Pending bus op — the phi2 the *next* run_until iteration will perform.
    std::uint16_t pending_addr{0xFFFC};

    // Start time of the next cycle to run. reset() pre-pays the cost of the
    // first access, so the run loop can enter phi2 on iteration 1 without a
    // bootstrap branch.
    Cycle cycle{};

    Config config;

    explicit M6502(Config cfg) noexcept(std::is_nothrow_move_constructible_v<Config>)
        : config(std::move(cfg))
    {
        reset();
    }

    // Simplified 2-cycle reset for stage 1 (vec lo + vec hi, then first opcode
    // fetch). Full 7-cycle BRK-style reset with dummy stack reads is a
    // follow-up.
    auto reset() -> void
    {
        pc           = 0;
        s            = 0xFD;
        p            = flag::I | flag::U;
        tstate       = detail::ResetStep0;
        base_addr    = 0;
        brk_flags    = BrkFlags::Reset;
        pending_addr = 0xFFFC;
        cycle        = config.access_cost_vector(0xFFFC).cost;
    }

    // Stage 1 stub — the single place IRQ/NMI signals would be observed at
    // the boundary of a timeslice. IRQ/NMI pipeline is a follow-up.
    auto sample_interrupts() -> void {}

    auto irq() -> void { /* TODO: interrupt pipeline */ }
    auto nmi() -> void { /* TODO: interrupt pipeline */ }

    auto run_until(Cycle horizon) -> Cycle
    {
        sample_interrupts();

        auto current = cycle;
        auto addr    = pending_addr;
        auto tst     = tstate;
        auto base    = base_addr;

        while (current < horizon) {
            switch (tst) {
                // --- Reset sequence -------------------------------------------
                case detail::ResetStep0: {
                    base = config.read_vector(addr);
                    addr = 0xFFFD;
                    TAWNY_STEP_TAIL(access_cost_vector(addr), detail::ResetStep1);
                }
                [[fallthrough]];
                case detail::ResetStep1: {
                    this->pc = static_cast<std::uint16_t>(
                        (base & 0x00FFu) |
                        (static_cast<std::uint16_t>(config.read_vector(addr)) << 8));
                    this->brk_flags = BrkFlags::None;
                    addr = this->pc;
                    TAWNY_STEP_TAIL(access_cost_opcode(addr), detail::ResetOpcodeFetch);
                }
                [[fallthrough]];
                case detail::ResetOpcodeFetch: {
                    this->pc = static_cast<std::uint16_t>(this->pc + 1);
                    tst  = static_cast<std::uint16_t>(
                        static_cast<std::uint16_t>(config.read_opcode(addr)) << 3);
                    addr = this->pc;
                    TAWNY_STEP_TAIL(access_cost(addr), tst);
                    break;
                }

                // --- Implied ops ---------------------------------------------
                TAWNY_IMPLIED(0xEA, detail::Nop)     // NOP
                TAWNY_IMPLIED(0xE8, detail::Inx)     // INX

                // --- Immediate reads -----------------------------------------
                TAWNY_IMM_READ(0xA9, detail::Lda)    // LDA #
                TAWNY_IMM_READ(0xC9, detail::Cmp)    // CMP #
                TAWNY_IMM_READ(0x69, detail::AdcBin) // ADC # (binary only)

                // --- Zero-page reads -----------------------------------------
                TAWNY_ZP_READ(0xA5, detail::Lda)     // LDA zp

                // --- Absolute reads ------------------------------------------
                TAWNY_ABS_READ(0xAD, detail::Lda)    // LDA abs

                // --- Zero-page writes ----------------------------------------
                TAWNY_ZP_WRITE(0x85, detail::Sta)    // STA zp

                // --- Absolute jump -------------------------------------------
                TAWNY_ABS_JUMP(0x4C)                 // JMP abs

                // --- Relative branches ---------------------------------------
                TAWNY_REL_BRANCH(0xD0, detail::BneCond)  // BNE rel

                default:
                    // Unimplemented opcode / step — stop emulation.
                    goto exit;
            }
        }

    exit:
        cycle        = current;
        pending_addr = addr;
        tstate       = tst;
        base_addr    = base;
        return current;
    }
};

}  // namespace tawny

// Keep dispatch macros from leaking out of this header.
#undef TAWNY_STEP_TAIL
#undef TAWNY_FETCH_OPCODE_CASE
#undef TAWNY_IMPLIED
#undef TAWNY_IMM_READ
#undef TAWNY_ZP_READ
#undef TAWNY_ZP_WRITE
#undef TAWNY_ABS_READ
#undef TAWNY_ABS_JUMP
#undef TAWNY_REL_BRANCH

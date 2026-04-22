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

// CPU register file. Lives as a local in run_until — constructed from the
// M6502 struct's fields at entry and written back at exit. Ops take it by
// mutable reference so they can update multiple fields at once without the
// pack/unpack dance that by-value returns would need.
struct Registers {
    std::uint8_t a;
    std::uint8_t x;
    std::uint8_t y;
    std::uint8_t s;
    std::uint8_t p;
};

[[gnu::always_inline]] inline auto set_nz(Registers &r, std::uint8_t val) -> void
{
    r.p = static_cast<std::uint8_t>((r.p & ~(flag::N | flag::Z))
                                     | (val & flag::N)
                                     | (val == 0 ? flag::Z : 0));
}

[[gnu::always_inline]] inline auto set_flag(Registers &r, std::uint8_t bit, bool on) -> void
{
    r.p = static_cast<std::uint8_t>(on ? (r.p | bit) : (r.p & ~bit));
}

// Op classes — each takes Registers& and returns void (or a byte for stores/
// RMW, or a bool for branch conds).

// ---- Read ops ----
struct Lda { static void apply(Registers &r, std::uint8_t v) { r.a = v; set_nz(r, v); } };
struct Ldx { static void apply(Registers &r, std::uint8_t v) { r.x = v; set_nz(r, v); } };
struct Ldy { static void apply(Registers &r, std::uint8_t v) { r.y = v; set_nz(r, v); } };

struct And { static void apply(Registers &r, std::uint8_t v) { r.a = static_cast<std::uint8_t>(r.a & v); set_nz(r, r.a); } };
struct Ora { static void apply(Registers &r, std::uint8_t v) { r.a = static_cast<std::uint8_t>(r.a | v); set_nz(r, r.a); } };
struct Eor { static void apply(Registers &r, std::uint8_t v) { r.a = static_cast<std::uint8_t>(r.a ^ v); set_nz(r, r.a); } };

struct Bit {
    static void apply(Registers &r, std::uint8_t v)
    {
        set_flag(r, flag::N, (v & 0x80) != 0);
        set_flag(r, flag::V, (v & 0x40) != 0);
        set_flag(r, flag::Z, (r.a & v) == 0);
    }
};

// Compares: reg - v, set N/Z from result; C = (reg >= v).
struct Cmp { static void apply(Registers &r, std::uint8_t v) { auto d = static_cast<std::uint8_t>(r.a - v); set_nz(r, d); set_flag(r, flag::C, r.a >= v); } };
struct Cpx { static void apply(Registers &r, std::uint8_t v) { auto d = static_cast<std::uint8_t>(r.x - v); set_nz(r, d); set_flag(r, flag::C, r.x >= v); } };
struct Cpy { static void apply(Registers &r, std::uint8_t v) { auto d = static_cast<std::uint8_t>(r.y - v); set_nz(r, d); set_flag(r, flag::C, r.y >= v); } };

// ADC / SBC — binary path always exists; decimal path taken when D is set.
struct Adc {
    static void apply_binary(Registers &r, std::uint8_t v)
    {
        unsigned a = r.a, m = v, cin = (r.p & flag::C) ? 1u : 0u;
        unsigned sum = a + m + cin;
        bool vflg = ((~(a ^ m) & (a ^ sum)) & 0x80u) != 0;
        r.a = static_cast<std::uint8_t>(sum);
        set_nz(r, r.a);
        set_flag(r, flag::C, sum > 0xFFu);
        set_flag(r, flag::V, vflg);
    }
    static void apply_decimal(Registers &r, std::uint8_t v)
    {
        // NMOS 6502 decimal ADC: N/V flags reflect the binary calculation;
        // A and C reflect the BCD-adjusted sum.
        unsigned a = r.a, m = v, cin = (r.p & flag::C) ? 1u : 0u;
        unsigned bin_sum = a + m + cin;
        set_flag(r, flag::Z, (bin_sum & 0xFFu) == 0);
        unsigned lo = (a & 0x0Fu) + (m & 0x0Fu) + cin;
        if (lo > 0x09u) lo += 0x06u;
        unsigned sum = (a & 0xF0u) + (m & 0xF0u) + (lo > 0x0Fu ? 0x10u : 0u) + (lo & 0x0Fu);
        set_flag(r, flag::N, (sum & 0x80u) != 0);
        set_flag(r, flag::V, ((~(a ^ m) & (a ^ sum)) & 0x80u) != 0);
        if (sum > 0x9Fu) sum += 0x60u;
        set_flag(r, flag::C, sum > 0xFFu);
        r.a = static_cast<std::uint8_t>(sum);
    }
    static void apply(Registers &r, std::uint8_t v)
    {
        if (r.p & flag::D) apply_decimal(r, v);
        else               apply_binary (r, v);
    }
};
struct Sbc {
    static void apply_binary(Registers &r, std::uint8_t v)
    {
        Adc::apply_binary(r, static_cast<std::uint8_t>(~v));
    }
    static void apply_decimal(Registers &r, std::uint8_t v)
    {
        unsigned a = r.a, m = v, cin = (r.p & flag::C) ? 1u : 0u;
        unsigned bin = (a - m - (1u - cin)) & 0xFFFFu;
        set_flag(r, flag::Z, (bin & 0xFFu) == 0);
        set_flag(r, flag::N, (bin & 0x80u) != 0);
        set_flag(r, flag::V, (((a ^ m) & (a ^ bin)) & 0x80u) != 0);
        set_flag(r, flag::C, bin < 0x100u);
        unsigned lo = (a & 0x0Fu) - (m & 0x0Fu) - (1u - cin);
        unsigned result;
        if (lo & 0x10u) {
            result = ((lo - 0x06u) & 0x0Fu) | (((a & 0xF0u) - (m & 0xF0u) - 0x10u) & 0xFFF0u);
        } else {
            result = (lo & 0x0Fu) | (((a & 0xF0u) - (m & 0xF0u)) & 0xFFF0u);
        }
        if (result & 0x100u) result -= 0x60u;
        r.a = static_cast<std::uint8_t>(result);
    }
    static void apply(Registers &r, std::uint8_t v)
    {
        if (r.p & flag::D) apply_decimal(r, v);
        else               apply_binary (r, v);
    }
};

// ---- Store ops (return the byte to write) ----
struct Sta { static auto value(const Registers &r) -> std::uint8_t { return r.a; } };
struct Stx { static auto value(const Registers &r) -> std::uint8_t { return r.x; } };
struct Sty { static auto value(const Registers &r) -> std::uint8_t { return r.y; } };
// SAX (illegal stable): store A & X.
struct Sax { static auto value(const Registers &r) -> std::uint8_t { return static_cast<std::uint8_t>(r.a & r.x); } };

// ---- Push / Pull ----
struct Pha { static auto value(const Registers &r) -> std::uint8_t { return r.a; } };
// PHP pushes P with B and U set (stack-only bits).
struct Php { static auto value(const Registers &r) -> std::uint8_t { return static_cast<std::uint8_t>(r.p | flag::B | flag::U); } };
struct Pla { static void apply(Registers &r, std::uint8_t v) { r.a = v; set_nz(r, v); } };
// PLP: strip B, ensure U.
struct Plp { static void apply(Registers &r, std::uint8_t v) { r.p = static_cast<std::uint8_t>((v & ~flag::B) | flag::U); } };

// ---- Implied ops ----
struct Nop {
    static void apply(Registers &) {}
    // Overload for read-addressing illegal NOPs.
    static void apply(Registers &, std::uint8_t) {}
};
struct Inx { static void apply(Registers &r) { r.x = static_cast<std::uint8_t>(r.x + 1); set_nz(r, r.x); } };
struct Iny { static void apply(Registers &r) { r.y = static_cast<std::uint8_t>(r.y + 1); set_nz(r, r.y); } };
struct Dex { static void apply(Registers &r) { r.x = static_cast<std::uint8_t>(r.x - 1); set_nz(r, r.x); } };
struct Dey { static void apply(Registers &r) { r.y = static_cast<std::uint8_t>(r.y - 1); set_nz(r, r.y); } };
struct Tax { static void apply(Registers &r) { r.x = r.a; set_nz(r, r.x); } };
struct Tay { static void apply(Registers &r) { r.y = r.a; set_nz(r, r.y); } };
struct Tsx { static void apply(Registers &r) { r.x = r.s; set_nz(r, r.x); } };
struct Txa { static void apply(Registers &r) { r.a = r.x; set_nz(r, r.a); } };
// TXS does NOT set flags.
struct Txs { static void apply(Registers &r) { r.s = r.x; } };
struct Tya { static void apply(Registers &r) { r.a = r.y; set_nz(r, r.a); } };
struct Clc { static void apply(Registers &r) { set_flag(r, flag::C, false); } };
struct Sec { static void apply(Registers &r) { set_flag(r, flag::C, true); } };
struct Cli { static void apply(Registers &r) { set_flag(r, flag::I, false); } };
struct Sei { static void apply(Registers &r) { set_flag(r, flag::I, true); } };
struct Clv { static void apply(Registers &r) { set_flag(r, flag::V, false); } };
struct Cld { static void apply(Registers &r) { set_flag(r, flag::D, false); } };
struct Sed { static void apply(Registers &r) { set_flag(r, flag::D, true); } };

// ---- RMW ops — take v, return new memory byte, mutate r.p. Illegal
// variants (SLO/RLA/...) also mutate r.a via the secondary ALU op.
struct Asl { static auto apply(Registers &r, std::uint8_t v) -> std::uint8_t { set_flag(r, flag::C, (v & 0x80) != 0); auto res = static_cast<std::uint8_t>(v << 1); set_nz(r, res); return res; } };
struct Lsr { static auto apply(Registers &r, std::uint8_t v) -> std::uint8_t { set_flag(r, flag::C, (v & 0x01) != 0); auto res = static_cast<std::uint8_t>(v >> 1); set_nz(r, res); return res; } };
struct Rol { static auto apply(Registers &r, std::uint8_t v) -> std::uint8_t { auto cin = (r.p & flag::C) ? 1u : 0u; set_flag(r, flag::C, (v & 0x80) != 0); auto res = static_cast<std::uint8_t>((v << 1) | cin); set_nz(r, res); return res; } };
struct Ror { static auto apply(Registers &r, std::uint8_t v) -> std::uint8_t { auto cin = (r.p & flag::C) ? 0x80u : 0u; set_flag(r, flag::C, (v & 0x01) != 0); auto res = static_cast<std::uint8_t>((v >> 1) | cin); set_nz(r, res); return res; } };
struct Inc { static auto apply(Registers &r, std::uint8_t v) -> std::uint8_t { auto res = static_cast<std::uint8_t>(v + 1); set_nz(r, res); return res; } };
struct Dec { static auto apply(Registers &r, std::uint8_t v) -> std::uint8_t { auto res = static_cast<std::uint8_t>(v - 1); set_nz(r, res); return res; } };

// ---- Branch conditions ----
struct BplCond { static auto taken(const Registers &r) -> bool { return (r.p & flag::N) == 0; } };
struct BmiCond { static auto taken(const Registers &r) -> bool { return (r.p & flag::N) != 0; } };
struct BvcCond { static auto taken(const Registers &r) -> bool { return (r.p & flag::V) == 0; } };
struct BvsCond { static auto taken(const Registers &r) -> bool { return (r.p & flag::V) != 0; } };
struct BccCond { static auto taken(const Registers &r) -> bool { return (r.p & flag::C) == 0; } };
struct BcsCond { static auto taken(const Registers &r) -> bool { return (r.p & flag::C) != 0; } };
struct BneCond { static auto taken(const Registers &r) -> bool { return (r.p & flag::Z) == 0; } };
struct BeqCond { static auto taken(const Registers &r) -> bool { return (r.p & flag::Z) != 0; } };

// ---- Stable illegal ops combining legal ones ----
// LAX = LDA + LDX.
struct Lax { static void apply(Registers &r, std::uint8_t v) { r.a = v; r.x = v; set_nz(r, v); } };
// SLO/RLA/SRE/RRA/DCP/ISC: RMW primary + second ALU op on A.
struct Slo { static auto apply(Registers &r, std::uint8_t v) -> std::uint8_t { auto res = Asl::apply(r, v); Ora::apply(r, res); return res; } };
struct Rla { static auto apply(Registers &r, std::uint8_t v) -> std::uint8_t { auto res = Rol::apply(r, v); And::apply(r, res); return res; } };
struct Sre { static auto apply(Registers &r, std::uint8_t v) -> std::uint8_t { auto res = Lsr::apply(r, v); Eor::apply(r, res); return res; } };
struct Rra { static auto apply(Registers &r, std::uint8_t v) -> std::uint8_t { auto res = Ror::apply(r, v); Adc::apply(r, res); return res; } };
struct Dcp { static auto apply(Registers &r, std::uint8_t v) -> std::uint8_t { auto res = Dec::apply(r, v); Cmp::apply(r, res); return res; } };
struct Isc { static auto apply(Registers &r, std::uint8_t v) -> std::uint8_t { auto res = Inc::apply(r, v); Sbc::apply(r, res); return res; } };

// Immediate-only illegals.
struct Anc { static void apply(Registers &r, std::uint8_t v) { And::apply(r, v); set_flag(r, flag::C, (r.a & 0x80) != 0); } };
struct Alr { static void apply(Registers &r, std::uint8_t v) { And::apply(r, v); r.a = Lsr::apply(r, r.a); } };
// ARR: A = ((A & v) >> 1) | (C << 7), with the ARR-specific V/C rule.
struct Arr {
    static void apply(Registers &r, std::uint8_t v)
    {
        And::apply(r, v);
        auto cin = (r.p & flag::C) ? 0x80u : 0u;
        r.a = static_cast<std::uint8_t>((r.a >> 1) | cin);
        set_nz(r, r.a);
        set_flag(r, flag::C, (r.a & 0x40) != 0);
        set_flag(r, flag::V, ((r.a >> 5) ^ (r.a >> 6)) & 0x01);
    }
};
// AXS: X = (A & X) - imm. Flags as CMP.
struct Axs {
    static void apply(Registers &r, std::uint8_t v)
    {
        auto ax  = static_cast<std::uint8_t>(r.a & r.x);
        auto res = static_cast<std::uint8_t>(ax - v);
        set_nz(r, res);
        set_flag(r, flag::C, ax >= v);
        r.x = res;
    }
};
// USBC: bit-equivalent to SBC.
struct Usbc { static void apply(Registers &r, std::uint8_t v) { Sbc::apply(r, v); } };

// ---- Unstable illegal ops (analog behaviour on real hardware — emulated
// here with the magic-constant approximation common to NMOS emulators).
// Virtually never used by legitimate software; accuracy beyond "doesn't
// crash" isn't a goal for a BBC Micro.

// ANE (0x8B, aka XAA): A = (A | magic) & X & imm. Magic is CPU-specific
// (commonly 0xEE on 6502, 0xEF on 6510).
struct Ane { static void apply(Registers &r, std::uint8_t v) {
    auto result = static_cast<std::uint8_t>((r.a | 0xEEu) & r.x & v);
    r.a = result;
    set_nz(r, result);
} };

// LXA (0xAB): A = X = (A | magic) & imm.
struct Lxa { static void apply(Registers &r, std::uint8_t v) {
    auto result = static_cast<std::uint8_t>((r.a | 0xEEu) & v);
    r.a = result;
    r.x = result;
    set_nz(r, result);
} };

// LAS (0xBB, aka LAR): A = X = S = mem & S.
struct Las { static void apply(Registers &r, std::uint8_t v) {
    auto result = static_cast<std::uint8_t>(v & r.s);
    r.a = result;
    r.x = result;
    r.s = result;
    set_nz(r, result);
} };

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

// Shorthand: set the next phi2 to a stack access at the current S, then run
// the standard step tail. Used all over BRK/JSR/RTS/RTI/PHA/PHP/PLA/PLP.
#define TAWNY_NEXT_STACK(NEXT_TST)                    \
    addr = static_cast<std::uint16_t>(0x0100u | r.s); \
    TAWNY_STEP_TAIL(access_cost_stack(r.s), (NEXT_TST))

// Shorthand: set the next phi2 to an opcode fetch at PC, then run the tail.
// (Used at the end of every penultimate step.)
#define TAWNY_NEXT_OPCODE_FETCH(NEXT_TST) \
    addr = pc;                            \
    TAWNY_STEP_TAIL(access_cost_opcode(addr), (NEXT_TST))

// FETCH_OPCODE_CASE — the last step of every instruction. Reads the next
// opcode byte (via read_opcode for the sync-read semantics), shifts it into a
// step-0 tstate for the new instruction, sets up the operand-fetch address,
// and breaks out so the while loop re-enters the switch at the new tstate.

#define TAWNY_FETCH_OPCODE_CASE(OPCODE, STEP)                           \
    case ((OPCODE) << 3) | (STEP): {                                    \
        pc = static_cast<std::uint16_t>(pc + 1);                        \
        tst  = static_cast<std::uint16_t>(                              \
                   static_cast<std::uint16_t>(config.read_opcode(addr)) \
                   << 3);                                               \
        addr = pc;                                                      \
        TAWNY_STEP_TAIL(access_cost(addr), tst);                        \
        break;                                                          \
    }

// IMPLIED(OPCODE, OP_CLASS) — 2 cycles. Step 0 is a discarded operand-fetch
// read; pc is NOT incremented (implied ops consume no bytes after the opcode).

#define TAWNY_IMPLIED(OPCODE, OP_CLASS)               \
    case ((OPCODE) << 3) | 0: {                       \
        (void)config.read(addr);                      \
        OP_CLASS::apply(r);                           \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 1); \
    }                                                 \
    [[fallthrough]];                                  \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 1)

// IMM_READ(OPCODE, OP_CLASS) — 2 cycles. Step 0 reads the immediate byte and
// applies the op; pc advances past it.

#define TAWNY_IMM_READ(OPCODE, OP_CLASS)              \
    case ((OPCODE) << 3) | 0: {                       \
        pc = static_cast<std::uint16_t>(pc + 1);      \
        OP_CLASS::apply(r, config.read(addr));        \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 1); \
    }                                                 \
    [[fallthrough]];                                  \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 1)

// ZP_READ(OPCODE, OP_CLASS) — 3 cycles.
//   step 0: read ZP-addr byte, stash in addr (high bits 0).
//   step 1: read value from ZP, apply op.
//   step 2: opcode fetch for next instruction.

#define TAWNY_ZP_READ(OPCODE, OP_CLASS)                                  \
    case ((OPCODE) << 3) | 0: {                                          \
        pc = static_cast<std::uint16_t>(pc + 1);                         \
        addr = config.read(addr);                                        \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 1);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 1: {                                          \
        OP_CLASS::apply(r,                                               \
            config.read_zp(static_cast<std::uint8_t>(addr)));            \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 2);                    \
    }                                                                    \
    [[fallthrough]];                                                     \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 2)

// ZP_WRITE(OPCODE, OP_CLASS) — 3 cycles.
//   step 0: read ZP-addr byte.
//   step 1: write op_class::value(cpu) to ZP.
//   step 2: opcode fetch for next instruction.

#define TAWNY_ZP_WRITE(OPCODE, OP_CLASS)                                 \
    case ((OPCODE) << 3) | 0: {                                          \
        pc = static_cast<std::uint16_t>(pc + 1);                         \
        addr = config.read(addr);                                        \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 1);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 1: {                                          \
        config.write_zp(static_cast<std::uint8_t>(addr),                 \
                        OP_CLASS::value(r));                             \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 2);                    \
    }                                                                    \
    [[fallthrough]];                                                     \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 2)

// ABS_READ(OPCODE, OP_CLASS) — 4 cycles.
//   step 0: read addr lo (into base).
//   step 1: read addr hi, combine into addr = base | (hi << 8).
//   step 2: read target value, apply op.
//   step 3: opcode fetch for next instruction.

#define TAWNY_ABS_READ(OPCODE, OP_CLASS)                           \
    case ((OPCODE) << 3) | 0: {                                    \
        pc = static_cast<std::uint16_t>(pc + 1);                   \
        base = config.read(addr);                                  \
        addr = pc;                                                 \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);   \
    }                                                              \
    [[fallthrough]];                                               \
    case ((OPCODE) << 3) | 1: {                                    \
        pc = static_cast<std::uint16_t>(pc + 1);                   \
        addr = static_cast<std::uint16_t>(                         \
            (base & 0x00FFu) |                                     \
            (static_cast<std::uint16_t>(config.read(addr)) << 8)); \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 2);   \
    }                                                              \
    [[fallthrough]];                                               \
    case ((OPCODE) << 3) | 2: {                                    \
        OP_CLASS::apply(r, config.read(addr));                     \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 3);              \
    }                                                              \
    [[fallthrough]];                                               \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 3)

// ABS_JUMP(OPCODE) — JMP abs, 3 cycles. No op class; updates pc in place.
//   step 0: read addr lo (into base).
//   step 1: read addr hi, set pc = (hi << 8) | (base & 0xFF).
//   step 2: opcode fetch at new pc.

#define TAWNY_ABS_JUMP(OPCODE)                                     \
    case ((OPCODE) << 3) | 0: {                                    \
        pc = static_cast<std::uint16_t>(pc + 1);                   \
        base = config.read(addr);                                  \
        addr = pc;                                                 \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);   \
    }                                                              \
    [[fallthrough]];                                               \
    case ((OPCODE) << 3) | 1: {                                    \
        pc = static_cast<std::uint16_t>(                           \
            (base & 0x00FFu) |                                     \
            (static_cast<std::uint16_t>(config.read(addr)) << 8)); \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 2);              \
    }                                                              \
    [[fallthrough]];                                               \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 2)

// ABS_WRITE(OPCODE, OP_CLASS) — 4 cycles. Like ABS_READ but the final step
// writes OP_CLASS::value(cpu) instead of reading.
#define TAWNY_ABS_WRITE(OPCODE, OP_CLASS)                          \
    case ((OPCODE) << 3) | 0: {                                    \
        pc = static_cast<std::uint16_t>(pc + 1);                   \
        base = config.read(addr);                                  \
        addr = pc;                                                 \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);   \
    }                                                              \
    [[fallthrough]];                                               \
    case ((OPCODE) << 3) | 1: {                                    \
        pc = static_cast<std::uint16_t>(pc + 1);                   \
        addr = static_cast<std::uint16_t>(                         \
            (base & 0x00FFu) |                                     \
            (static_cast<std::uint16_t>(config.read(addr)) << 8)); \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 2);   \
    }                                                              \
    [[fallthrough]];                                               \
    case ((OPCODE) << 3) | 2: {                                    \
        config.write(addr, OP_CLASS::value(r));                    \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 3);              \
    }                                                              \
    [[fallthrough]];                                               \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 3)

// ZP_INDEXED_READ / ZP_INDEXED_WRITE — 4 cycles. Parameterised by index reg
// (X or Y). The indexed-zp address wraps within ZP ((base + idx) & 0xFF).
#define TAWNY_ZP_INDEXED_READ(OPCODE, OP_CLASS, IDX)                     \
    case ((OPCODE) << 3) | 0: {                                          \
        pc = static_cast<std::uint16_t>(pc + 1);                         \
        addr = config.read(addr);                                        \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 1);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 1: {                                          \
        (void)config.read_zp(static_cast<std::uint8_t>(addr));           \
        addr = static_cast<std::uint16_t>(                               \
            static_cast<std::uint8_t>(addr + (IDX)));                    \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 2);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 2: {                                          \
        OP_CLASS::apply(r,                                               \
            config.read_zp(static_cast<std::uint8_t>(addr)));            \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 3);                    \
    }                                                                    \
    [[fallthrough]];                                                     \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 3)

#define TAWNY_ZP_INDEXED_WRITE(OPCODE, OP_CLASS, IDX)                    \
    case ((OPCODE) << 3) | 0: {                                          \
        pc = static_cast<std::uint16_t>(pc + 1);                         \
        addr = config.read(addr);                                        \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 1);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 1: {                                          \
        (void)config.read_zp(static_cast<std::uint8_t>(addr));           \
        addr = static_cast<std::uint16_t>(                               \
            static_cast<std::uint8_t>(addr + (IDX)));                    \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 2);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 2: {                                          \
        config.write_zp(static_cast<std::uint8_t>(addr),                 \
                        OP_CLASS::value(r));                             \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 3);                    \
    }                                                                    \
    [[fallthrough]];                                                     \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 3)

#define TAWNY_ZPX_READ(OPCODE, OP_CLASS)  TAWNY_ZP_INDEXED_READ(OPCODE, OP_CLASS, r.x)
#define TAWNY_ZPY_READ(OPCODE, OP_CLASS)  TAWNY_ZP_INDEXED_READ(OPCODE, OP_CLASS, r.y)
#define TAWNY_ZPX_WRITE(OPCODE, OP_CLASS) TAWNY_ZP_INDEXED_WRITE(OPCODE, OP_CLASS, r.x)
#define TAWNY_ZPY_WRITE(OPCODE, OP_CLASS) TAWNY_ZP_INDEXED_WRITE(OPCODE, OP_CLASS, r.y)

// ABS_INDEXED_READ — 4 or 5 cycles. Optimistic path: if base_lo + IDX didn't
// carry, the "wrong" address with the un-carried high byte is actually
// correct, and we skip the fix-up cycle. If it did carry, step 2 does a
// dummy read at the wrong page, fixes the high byte, and step 3 does the
// real read.
#define TAWNY_AB_INDEXED_READ(OPCODE, OP_CLASS, IDX)                        \
    case ((OPCODE) << 3) | 0: {                                             \
        pc = static_cast<std::uint16_t>(pc + 1);                            \
        base = config.read(addr);                                           \
        addr = pc;                                                          \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);            \
    }                                                                       \
    [[fallthrough]];                                                        \
    case ((OPCODE) << 3) | 1: {                                             \
        pc = static_cast<std::uint16_t>(pc + 1);                            \
        /* Compute target addr + page-cross flag in a tight sub-scope so    \
           _hi/_lo_sum aren't live at the step-2 case label below (case     \
           labels can't bypass initializations in enclosing scope). */      \
        bool _cross;                                                        \
        {                                                                   \
            auto _hi     = config.read(addr);                               \
            auto _lo_sum = static_cast<unsigned>((base & 0x00FFu) + (IDX)); \
            addr = static_cast<std::uint16_t>(                              \
                (_hi << 8) | (_lo_sum & 0x00FFu));                          \
            _cross = _lo_sum > 0xFFu;                                       \
        }                                                                   \
        /* Common path: no page cross — fall straight through to step 3,    \
           no break, no tst write, no re-dispatch. On page cross we enter   \
           the if body and the nested step-2 case label, then also fall     \
           through to step 3. */                                            \
        if (_cross) {                                                       \
            base = static_cast<std::uint16_t>(addr + 0x0100u);              \
            TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 2);        \
    case ((OPCODE) << 3) | 2:                                               \
            (void)config.read(addr);                                        \
            addr = base;                                                    \
            TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 3);        \
        } else {                                                            \
            TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 3);        \
        }                                                                   \
    }                                                                       \
    [[fallthrough]];                                                        \
    case ((OPCODE) << 3) | 3: {                                             \
        OP_CLASS::apply(r, config.read(addr));                              \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 4);                       \
    }                                                                       \
    [[fallthrough]];                                                        \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 4)

// ABS_INDEXED_WRITE — always 5 cycles (no skip — penalty always paid).
#define TAWNY_AB_INDEXED_WRITE(OPCODE, OP_CLASS, IDX)                   \
    case ((OPCODE) << 3) | 0: {                                         \
        pc = static_cast<std::uint16_t>(pc + 1);                        \
        base = config.read(addr);                                       \
        addr = pc;                                                      \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);        \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 1: {                                         \
        pc = static_cast<std::uint16_t>(pc + 1);                        \
        auto _hi     = config.read(addr);                               \
        auto _lo_sum = static_cast<unsigned>((base & 0x00FFu) + (IDX)); \
        addr = static_cast<std::uint16_t>(                              \
            (_hi << 8) | (_lo_sum & 0x00FFu));                          \
        /* stash correct addr for step 3 */                             \
        base = static_cast<std::uint16_t>(                              \
            ((_hi << 8) + (_lo_sum & 0xFF00u)) |                        \
            (_lo_sum & 0x00FFu));                                       \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 2);        \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 2: {                                         \
        (void)config.read(addr);                                        \
        addr = base;                                                    \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 3);        \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 3: {                                         \
        config.write(addr, OP_CLASS::value(r));                         \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 4);                   \
    }                                                                   \
    [[fallthrough]];                                                    \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 4)

#define TAWNY_ABX_READ(OPCODE, OP_CLASS)  TAWNY_AB_INDEXED_READ(OPCODE, OP_CLASS, r.x)
#define TAWNY_ABY_READ(OPCODE, OP_CLASS)  TAWNY_AB_INDEXED_READ(OPCODE, OP_CLASS, r.y)
#define TAWNY_ABX_WRITE(OPCODE, OP_CLASS) TAWNY_AB_INDEXED_WRITE(OPCODE, OP_CLASS, r.x)
#define TAWNY_ABY_WRITE(OPCODE, OP_CLASS) TAWNY_AB_INDEXED_WRITE(OPCODE, OP_CLASS, r.y)

// IZX (indirect,X) read/write — 6 cycles. ZP index with X, then fetch 2-byte
// pointer from ZP (wrapping in ZP), then read/write from target.
#define TAWNY_IZX_READ(OPCODE, OP_CLASS)                                 \
    case ((OPCODE) << 3) | 0: {                                          \
        pc = static_cast<std::uint16_t>(pc + 1);                         \
        auto _zp = config.read(addr);                                    \
        base = static_cast<std::uint16_t>(                               \
            static_cast<std::uint8_t>(_zp + r.x));                       \
        addr = static_cast<std::uint16_t>(_zp);                          \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 1);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 1: {                                          \
        (void)config.read_zp(static_cast<std::uint8_t>(addr));           \
        addr = base;                                                     \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 2);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 2: {                                          \
        base = config.read_zp(static_cast<std::uint8_t>(addr));          \
        addr = static_cast<std::uint16_t>(                               \
            static_cast<std::uint8_t>(addr + 1));                        \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 3);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 3: {                                          \
        addr = static_cast<std::uint16_t>(                               \
            (base & 0x00FFu) |                                           \
            (static_cast<std::uint16_t>(                                 \
                config.read_zp(static_cast<std::uint8_t>(addr))) << 8)); \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);         \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 4: {                                          \
        OP_CLASS::apply(r, config.read(addr));                           \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                    \
    }                                                                    \
    [[fallthrough]];                                                     \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

#define TAWNY_IZX_WRITE(OPCODE, OP_CLASS)                                \
    case ((OPCODE) << 3) | 0: {                                          \
        pc = static_cast<std::uint16_t>(pc + 1);                         \
        auto _zp = config.read(addr);                                    \
        base = static_cast<std::uint16_t>(                               \
            static_cast<std::uint8_t>(_zp + r.x));                       \
        addr = static_cast<std::uint16_t>(_zp);                          \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 1);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 1: {                                          \
        (void)config.read_zp(static_cast<std::uint8_t>(addr));           \
        addr = base;                                                     \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 2);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 2: {                                          \
        base = config.read_zp(static_cast<std::uint8_t>(addr));          \
        addr = static_cast<std::uint16_t>(                               \
            static_cast<std::uint8_t>(addr + 1));                        \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 3);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 3: {                                          \
        addr = static_cast<std::uint16_t>(                               \
            (base & 0x00FFu) |                                           \
            (static_cast<std::uint16_t>(                                 \
                config.read_zp(static_cast<std::uint8_t>(addr))) << 8)); \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);         \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 4: {                                          \
        config.write(addr, OP_CLASS::value(r));                          \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                    \
    }                                                                    \
    [[fallthrough]];                                                     \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

// IZY (indirect),Y read/write — 5 or 6 cycles for reads (page-cross
// penalty); always 6 for writes.
#define TAWNY_IZY_READ(OPCODE, OP_CLASS)                                    \
    case ((OPCODE) << 3) | 0: {                                             \
        pc = static_cast<std::uint16_t>(pc + 1);                            \
        addr = static_cast<std::uint16_t>(config.read(addr));               \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),    \
                        ((OPCODE) << 3) | 1);                               \
    }                                                                       \
    [[fallthrough]];                                                        \
    case ((OPCODE) << 3) | 1: {                                             \
        base = config.read_zp(static_cast<std::uint8_t>(addr));             \
        addr = static_cast<std::uint16_t>(                                  \
            static_cast<std::uint8_t>(addr + 1));                           \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),    \
                        ((OPCODE) << 3) | 2);                               \
    }                                                                       \
    [[fallthrough]];                                                        \
    case ((OPCODE) << 3) | 2: {                                             \
        /* Same trick as AB_INDEXED_READ: nested step-3 case label inside   \
           the page-cross branch, fall straight through to step 4 on the    \
           common (no-cross) path. _hi/_lo_sum scoped tightly so the case   \
           label doesn't bypass their initializations. */                   \
        bool _cross;                                                        \
        {                                                                   \
            auto _hi     = config.read_zp(static_cast<std::uint8_t>(addr)); \
            auto _lo_sum = static_cast<unsigned>((base & 0x00FFu) + r.y);   \
            addr = static_cast<std::uint16_t>(                              \
                (_hi << 8) | (_lo_sum & 0x00FFu));                          \
            _cross = _lo_sum > 0xFFu;                                       \
        }                                                                   \
        if (_cross) {                                                       \
            base = static_cast<std::uint16_t>(addr + 0x0100u);              \
            TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 3);        \
    case ((OPCODE) << 3) | 3:                                               \
            (void)config.read(addr);                                        \
            addr = base;                                                    \
            TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);        \
        } else {                                                            \
            TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);        \
        }                                                                   \
    }                                                                       \
    [[fallthrough]];                                                        \
    case ((OPCODE) << 3) | 4: {                                             \
        OP_CLASS::apply(r, config.read(addr));                              \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                       \
    }                                                                       \
    [[fallthrough]];                                                        \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

#define TAWNY_IZY_WRITE(OPCODE, OP_CLASS)                                \
    case ((OPCODE) << 3) | 0: {                                          \
        pc = static_cast<std::uint16_t>(pc + 1);                         \
        addr = static_cast<std::uint16_t>(config.read(addr));            \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 1);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 1: {                                          \
        base = config.read_zp(static_cast<std::uint8_t>(addr));          \
        addr = static_cast<std::uint16_t>(                               \
            static_cast<std::uint8_t>(addr + 1));                        \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 2);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 2: {                                          \
        auto _hi     = config.read_zp(static_cast<std::uint8_t>(addr));  \
        auto _lo_sum = static_cast<unsigned>((base & 0x00FFu) + r.y);    \
        addr = static_cast<std::uint16_t>(                               \
            (_hi << 8) | (_lo_sum & 0x00FFu));                           \
        base = static_cast<std::uint16_t>(                               \
            ((_hi << 8) + (_lo_sum & 0xFF00u)) |                         \
            (_lo_sum & 0x00FFu));                                        \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 3);         \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 3: {                                          \
        (void)config.read(addr);                                         \
        addr = base;                                                     \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);         \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 4: {                                          \
        config.write(addr, OP_CLASS::value(r));                          \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                    \
    }                                                                    \
    [[fallthrough]];                                                     \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

// ACC_RMW(OPCODE, OP_CLASS) — ASL A / LSR A / ROL A / ROR A. 2 cycles:
// dummy operand read, apply op to A, fetch_opcode.
#define TAWNY_ACC_RMW(OPCODE, OP_CLASS)               \
    case ((OPCODE) << 3) | 0: {                       \
        (void)config.read(addr);                      \
        r.a = OP_CLASS::apply(r, r.a);                \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 1); \
    }                                                 \
    [[fallthrough]];                                  \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 1)

// RMW addressing modes. Each does a read-modify-write: read original value,
// dummy-write it back, then write the transformed value. 6502 quirk.
//
// ZP_RMW: 5 cycles. ZPX_RMW: 6. ABS_RMW: 6. ABX_RMW: 7 (always).
#define TAWNY_ZP_RMW(OPCODE, OP_CLASS)                                   \
    case ((OPCODE) << 3) | 0: {                                          \
        pc = static_cast<std::uint16_t>(pc + 1);                         \
        addr = config.read(addr);                                        \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 1);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 1: {                                          \
        base = config.read_zp(static_cast<std::uint8_t>(addr));          \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 2);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 2: {                                          \
        config.write_zp(static_cast<std::uint8_t>(addr),                 \
                        static_cast<std::uint8_t>(base));                \
        base = OP_CLASS::apply(r, static_cast<std::uint8_t>(base));      \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 3);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 3: {                                          \
        config.write_zp(static_cast<std::uint8_t>(addr),                 \
                        static_cast<std::uint8_t>(base));                \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 4);                    \
    }                                                                    \
    [[fallthrough]];                                                     \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 4)

#define TAWNY_ZPX_RMW(OPCODE, OP_CLASS)                                  \
    case ((OPCODE) << 3) | 0: {                                          \
        pc = static_cast<std::uint16_t>(pc + 1);                         \
        addr = config.read(addr);                                        \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 1);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 1: {                                          \
        (void)config.read_zp(static_cast<std::uint8_t>(addr));           \
        addr = static_cast<std::uint16_t>(                               \
            static_cast<std::uint8_t>(addr + r.x));                      \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 2);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 2: {                                          \
        base = config.read_zp(static_cast<std::uint8_t>(addr));          \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 3);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 3: {                                          \
        config.write_zp(static_cast<std::uint8_t>(addr),                 \
                        static_cast<std::uint8_t>(base));                \
        base = OP_CLASS::apply(r, static_cast<std::uint8_t>(base));      \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)), \
                        ((OPCODE) << 3) | 4);                            \
    }                                                                    \
    [[fallthrough]];                                                     \
    case ((OPCODE) << 3) | 4: {                                          \
        config.write_zp(static_cast<std::uint8_t>(addr),                 \
                        static_cast<std::uint8_t>(base));                \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                    \
    }                                                                    \
    [[fallthrough]];                                                     \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

#define TAWNY_ABS_RMW(OPCODE, OP_CLASS)                             \
    case ((OPCODE) << 3) | 0: {                                     \
        pc = static_cast<std::uint16_t>(pc + 1);                    \
        base = config.read(addr);                                   \
        addr = pc;                                                  \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);    \
    }                                                               \
    [[fallthrough]];                                                \
    case ((OPCODE) << 3) | 1: {                                     \
        pc = static_cast<std::uint16_t>(pc + 1);                    \
        addr = static_cast<std::uint16_t>(                          \
            (base & 0x00FFu) |                                      \
            (static_cast<std::uint16_t>(config.read(addr)) << 8));  \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 2);    \
    }                                                               \
    [[fallthrough]];                                                \
    case ((OPCODE) << 3) | 2: {                                     \
        base = config.read(addr);                                   \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 3);    \
    }                                                               \
    [[fallthrough]];                                                \
    case ((OPCODE) << 3) | 3: {                                     \
        config.write(addr, static_cast<std::uint8_t>(base));        \
        base = OP_CLASS::apply(r, static_cast<std::uint8_t>(base)); \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);    \
    }                                                               \
    [[fallthrough]];                                                \
    case ((OPCODE) << 3) | 4: {                                     \
        config.write(addr, static_cast<std::uint8_t>(base));        \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);               \
    }                                                               \
    [[fallthrough]];                                                \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

#define TAWNY_AB_INDEXED_RMW(OPCODE, OP_CLASS, IDX)                     \
    case ((OPCODE) << 3) | 0: {                                         \
        pc = static_cast<std::uint16_t>(pc + 1);                        \
        base = config.read(addr);                                       \
        addr = pc;                                                      \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);        \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 1: {                                         \
        pc = static_cast<std::uint16_t>(pc + 1);                        \
        auto _hi     = config.read(addr);                               \
        auto _lo_sum = static_cast<unsigned>((base & 0x00FFu) + (IDX)); \
        addr = static_cast<std::uint16_t>(                              \
            (_hi << 8) | (_lo_sum & 0x00FFu));                          \
        base = static_cast<std::uint16_t>(                              \
            ((_hi << 8) + (_lo_sum & 0xFF00u)) |                        \
            (_lo_sum & 0x00FFu));                                       \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 2);        \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 2: {                                         \
        (void)config.read(addr);                                        \
        addr = base;                                                    \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 3);        \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 3: {                                         \
        base = config.read(addr);                                       \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);        \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 4: {                                         \
        config.write(addr, static_cast<std::uint8_t>(base));            \
        base = OP_CLASS::apply(r, static_cast<std::uint8_t>(base));     \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 5);        \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 5: {                                         \
        config.write(addr, static_cast<std::uint8_t>(base));            \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 6);                   \
    }                                                                   \
    [[fallthrough]];                                                    \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 6)

#define TAWNY_ABX_RMW(OPCODE, OP_CLASS) TAWNY_AB_INDEXED_RMW(OPCODE, OP_CLASS, r.x)
#define TAWNY_ABY_RMW(OPCODE, OP_CLASS) TAWNY_AB_INDEXED_RMW(OPCODE, OP_CLASS, r.y)

// IZX_RMW — 8 cycles. Indexed-indirect (zp,X) RMW.
#define TAWNY_IZX_RMW(OPCODE, OP_CLASS)                                 \
    case ((OPCODE) << 3) | 0: {                                         \
        pc = static_cast<std::uint16_t>(pc + 1);                        \
        auto _zp = config.read(addr);                                   \
        base = static_cast<std::uint16_t>(                              \
            static_cast<std::uint8_t>(_zp + r.x));                      \
        addr = static_cast<std::uint16_t>(_zp);                         \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),\
                        ((OPCODE) << 3) | 1);                           \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 1: {                                         \
        (void)config.read_zp(static_cast<std::uint8_t>(addr));          \
        addr = base;                                                    \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),\
                        ((OPCODE) << 3) | 2);                           \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 2: {                                         \
        base = config.read_zp(static_cast<std::uint8_t>(addr));         \
        addr = static_cast<std::uint16_t>(                              \
            static_cast<std::uint8_t>(addr + 1));                       \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),\
                        ((OPCODE) << 3) | 3);                           \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 3: {                                         \
        addr = static_cast<std::uint16_t>(                              \
            (base & 0x00FFu) |                                          \
            (static_cast<std::uint16_t>(                                \
                config.read_zp(static_cast<std::uint8_t>(addr))) << 8));\
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);        \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 4: {                                         \
        base = config.read(addr);                                       \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 5);        \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 5: {                                         \
        config.write(addr, static_cast<std::uint8_t>(base));            \
        base = OP_CLASS::apply(r, static_cast<std::uint8_t>(base));     \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 6);        \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 6: {                                         \
        config.write(addr, static_cast<std::uint8_t>(base));            \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 7);                   \
    }                                                                   \
    [[fallthrough]];                                                    \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 7)

// IZY_RMW — 8 cycles. Always pays the page-cross fix-up (like IZY_WRITE).
#define TAWNY_IZY_RMW(OPCODE, OP_CLASS)                                 \
    case ((OPCODE) << 3) | 0: {                                         \
        pc = static_cast<std::uint16_t>(pc + 1);                        \
        addr = static_cast<std::uint16_t>(config.read(addr));           \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),\
                        ((OPCODE) << 3) | 1);                           \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 1: {                                         \
        base = config.read_zp(static_cast<std::uint8_t>(addr));         \
        addr = static_cast<std::uint16_t>(                              \
            static_cast<std::uint8_t>(addr + 1));                       \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),\
                        ((OPCODE) << 3) | 2);                           \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 2: {                                         \
        auto _hi     = config.read_zp(static_cast<std::uint8_t>(addr)); \
        auto _lo_sum = static_cast<unsigned>((base & 0x00FFu) + r.y);   \
        addr = static_cast<std::uint16_t>(                              \
            (_hi << 8) | (_lo_sum & 0x00FFu));                          \
        base = static_cast<std::uint16_t>(                              \
            ((_hi << 8) + (_lo_sum & 0xFF00u)) |                        \
            (_lo_sum & 0x00FFu));                                       \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 3);        \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 3: {                                         \
        (void)config.read(addr);                                        \
        addr = base;                                                    \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);        \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 4: {                                         \
        base = config.read(addr);                                       \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 5);        \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 5: {                                         \
        config.write(addr, static_cast<std::uint8_t>(base));            \
        base = OP_CLASS::apply(r, static_cast<std::uint8_t>(base));     \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 6);        \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 6: {                                         \
        config.write(addr, static_cast<std::uint8_t>(base));            \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 7);                   \
    }                                                                   \
    [[fallthrough]];                                                    \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 7)

// JMP (indirect) — 5 cycles. The NMOS 6502 has a well-known page-wrap bug:
// when the pointer low byte is at $xxFF, the high byte is fetched from $xx00
// (same page) rather than $(xx+1)00. We replicate.
#define TAWNY_JMP_IND(OPCODE)                                      \
    case ((OPCODE) << 3) | 0: {                                    \
        pc = static_cast<std::uint16_t>(pc + 1);                   \
        base = config.read(addr);                                  \
        addr = pc;                                                 \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);   \
    }                                                              \
    [[fallthrough]];                                               \
    case ((OPCODE) << 3) | 1: {                                    \
        pc = static_cast<std::uint16_t>(pc + 1);                   \
        addr = static_cast<std::uint16_t>(                         \
            (base & 0x00FFu) |                                     \
            (static_cast<std::uint16_t>(config.read(addr)) << 8)); \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 2);   \
    }                                                              \
    [[fallthrough]];                                               \
    case ((OPCODE) << 3) | 2: {                                    \
        base = config.read(addr);                                  \
        /* NMOS bug: +1 wraps within the same page. */             \
        addr = static_cast<std::uint16_t>(                         \
            (addr & 0xFF00u) |                                     \
            static_cast<std::uint8_t>((addr & 0xFFu) + 1));        \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 3);   \
    }                                                              \
    [[fallthrough]];                                               \
    case ((OPCODE) << 3) | 3: {                                    \
        pc = static_cast<std::uint16_t>(                           \
            (base & 0x00FFu) |                                     \
            (static_cast<std::uint16_t>(config.read(addr)) << 8)); \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 4);              \
    }                                                              \
    [[fallthrough]];                                               \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 4)

// JSR — 6 cycles. Reads addr lo, dummies a stack read, pushes PCH, pushes
// PCL, reads addr hi (now forming the jump target), then fetch_opcode.
#define TAWNY_JSR(OPCODE)                                            \
    case ((OPCODE) << 3) | 0: {                                      \
        pc = static_cast<std::uint16_t>(pc + 1);                     \
        base = config.read(addr);                                    \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 1);                       \
    }                                                                \
    [[fallthrough]];                                                 \
    case ((OPCODE) << 3) | 1: {                                      \
        (void)config.read_stack(r.s);                                \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 2);                       \
    }                                                                \
    [[fallthrough]];                                                 \
    case ((OPCODE) << 3) | 2: {                                      \
        config.write_stack(r.s, static_cast<std::uint8_t>(pc >> 8)); \
        r.s = static_cast<std::uint8_t>(r.s - 1);                    \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 3);                       \
    }                                                                \
    [[fallthrough]];                                                 \
    case ((OPCODE) << 3) | 3: {                                      \
        config.write_stack(r.s, static_cast<std::uint8_t>(pc));      \
        r.s = static_cast<std::uint8_t>(r.s - 1);                    \
        addr = pc;                                                   \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);     \
    }                                                                \
    [[fallthrough]];                                                 \
    case ((OPCODE) << 3) | 4: {                                      \
        pc = static_cast<std::uint16_t>(                             \
            (base & 0x00FFu) |                                       \
            (static_cast<std::uint16_t>(config.read(addr)) << 8));   \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                \
    }                                                                \
    [[fallthrough]];                                                 \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

// RTS — 6 cycles. Dummy operand read, dummy stack read, pull PCL, pull PCH
// (forming PC), dummy read at PC (pc++ afterwards), fetch_opcode.
#define TAWNY_RTS(OPCODE)                                               \
    case ((OPCODE) << 3) | 0: {                                         \
        (void)config.read(addr);                                        \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 1);                          \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 1: {                                         \
        (void)config.read_stack(r.s);                                   \
        r.s = static_cast<std::uint8_t>(r.s + 1);                       \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 2);                          \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 2: {                                         \
        base = config.read_stack(r.s);                                  \
        r.s = static_cast<std::uint8_t>(r.s + 1);                       \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 3);                          \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 3: {                                         \
        pc = static_cast<std::uint16_t>(                                \
            (base & 0x00FFu) |                                          \
            (static_cast<std::uint16_t>(config.read_stack(r.s)) << 8)); \
        addr = pc;                                                      \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);        \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 4: {                                         \
        (void)config.read(addr);                                        \
        pc = static_cast<std::uint16_t>(pc + 1);                        \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                   \
    }                                                                   \
    [[fallthrough]];                                                    \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

// RTI — 6 cycles. Dummy operand read, dummy stack read, pull P, pull PCL,
// pull PCH (forming PC), fetch_opcode.
#define TAWNY_RTI(OPCODE)                                               \
    case ((OPCODE) << 3) | 0: {                                         \
        (void)config.read(addr);                                        \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 1);                          \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 1: {                                         \
        (void)config.read_stack(r.s);                                   \
        r.s = static_cast<std::uint8_t>(r.s + 1);                       \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 2);                          \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 2: {                                         \
        r.p = static_cast<std::uint8_t>(                                \
            (config.read_stack(r.s) & ~flag::B) | flag::U);             \
        r.s = static_cast<std::uint8_t>(r.s + 1);                       \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 3);                          \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 3: {                                         \
        base = config.read_stack(r.s);                                  \
        r.s = static_cast<std::uint8_t>(r.s + 1);                       \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 4);                          \
    }                                                                   \
    [[fallthrough]];                                                    \
    case ((OPCODE) << 3) | 4: {                                         \
        pc = static_cast<std::uint16_t>(                                \
            (base & 0x00FFu) |                                          \
            (static_cast<std::uint16_t>(config.read_stack(r.s)) << 8)); \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                   \
    }                                                                   \
    [[fallthrough]];                                                    \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

// PHA / PHP — 3 cycles. Dummy operand read, push OP_CLASS::value(cpu),
// fetch_opcode.
#define TAWNY_PUSH(OPCODE, OP_CLASS)                  \
    case ((OPCODE) << 3) | 0: {                       \
        (void)config.read(addr);                      \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 1);        \
    }                                                 \
    [[fallthrough]];                                  \
    case ((OPCODE) << 3) | 1: {                       \
        config.write_stack(r.s, OP_CLASS::value(r));  \
        r.s = static_cast<std::uint8_t>(r.s - 1);     \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 2); \
    }                                                 \
    [[fallthrough]];                                  \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 2)

// PLA / PLP — 4 cycles. Dummy operand read, dummy stack read (pre-increment),
// pull byte and apply to CPU via OP_CLASS::apply(cpu, pulled), fetch_opcode.
#define TAWNY_PULL(OPCODE, OP_CLASS)                  \
    case ((OPCODE) << 3) | 0: {                       \
        (void)config.read(addr);                      \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 1);        \
    }                                                 \
    [[fallthrough]];                                  \
    case ((OPCODE) << 3) | 1: {                       \
        (void)config.read_stack(r.s);                 \
        r.s = static_cast<std::uint8_t>(r.s + 1);     \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 2);        \
    }                                                 \
    [[fallthrough]];                                  \
    case ((OPCODE) << 3) | 2: {                       \
        OP_CLASS::apply(r, config.read_stack(r.s));   \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 3); \
    }                                                 \
    [[fallthrough]];                                  \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 3)

// JAM — halts the CPU on an illegal opcode by redirecting the next phi2 back
// to the same opcode, which the Dormann trap detection catches. Simple stub.
#define TAWNY_JAM(OPCODE)                                                     \
    case ((OPCODE) << 3) | 0: {                                               \
        /* Leave addr pointing at the JAM opcode — next opcode fetch will     \
           hit the same address, triggering the trap or at least spinning. */ \
        pc = static_cast<std::uint16_t>(pc - 1);                              \
        addr = pc;                                                            \
        TAWNY_STEP_TAIL(access_cost_opcode(addr), ((OPCODE) << 3) | 0);       \
        break;                                                                \
    }

// BRK(OPCODE) — 7 cycles. Handles software BRK, reset, IRQ, and NMI through a
// single microcode with runtime branching on brk_flags. This is the one opcode
// where phi2's bus-op kind (write_stack vs read_stack) isn't statically fixed
// per case: reset does dummy reads, the other three push.
//
// Step layout:
//   0: discard pending read (signature byte on SW BRK, junk on reset/IRQ/NMI);
//      SW BRK also advances pc past the signature byte.
//   1: push PCH (or dummy read on reset), decrement S.
//   2: push PCL (or dummy read), decrement S.
//   3: push P (B set only for SW BRK; or dummy read on reset), decrement S,
//      set I, choose vector ($FFFA NMI / $FFFC Reset / $FFFE IRQ or BRK).
//   4: read vector low.
//   5: read vector high, set PC, clear brk_flags.
//   6: opcode fetch of the handler's first instruction.
#define TAWNY_BRK(OPCODE)                                                 \
    case ((OPCODE) << 3) | 0: {                                           \
        (void)config.read(addr);                                          \
        if (brk_flags == BrkFlags::None) {                                \
            pc = static_cast<std::uint16_t>(pc + 1);                      \
        }                                                                 \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 1);                            \
    }                                                                     \
    [[fallthrough]];                                                      \
    case ((OPCODE) << 3) | 1: {                                           \
        if (brk_flags == BrkFlags::Reset) {                               \
            (void)config.read_stack(r.s);                                 \
        } else {                                                          \
            config.write_stack(r.s, static_cast<std::uint8_t>(pc >> 8));  \
        }                                                                 \
        r.s = static_cast<std::uint8_t>(r.s - 1);                         \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 2);                            \
    }                                                                     \
    [[fallthrough]];                                                      \
    case ((OPCODE) << 3) | 2: {                                           \
        if (brk_flags == BrkFlags::Reset) {                               \
            (void)config.read_stack(r.s);                                 \
        } else {                                                          \
            config.write_stack(r.s, static_cast<std::uint8_t>(pc));       \
        }                                                                 \
        r.s = static_cast<std::uint8_t>(r.s - 1);                         \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 3);                            \
    }                                                                     \
    [[fallthrough]];                                                      \
    case ((OPCODE) << 3) | 3: {                                           \
        if (brk_flags == BrkFlags::Reset) {                               \
            (void)config.read_stack(r.s);                                 \
        } else {                                                          \
            std::uint8_t _pushed_p = (brk_flags == BrkFlags::None)        \
                ? static_cast<std::uint8_t>(r.p | flag::B | flag::U)      \
                : static_cast<std::uint8_t>(r.p | flag::U);               \
            config.write_stack(r.s, _pushed_p);                           \
        }                                                                 \
        r.s = static_cast<std::uint8_t>(r.s - 1);                         \
        r.p = static_cast<std::uint8_t>(r.p | flag::I);                   \
        addr = (brk_flags == BrkFlags::Nmi)   ? 0xFFFAu                   \
             : (brk_flags == BrkFlags::Reset) ? 0xFFFCu                   \
             :                                  0xFFFEu;                  \
        TAWNY_STEP_TAIL(access_cost_vector(addr), ((OPCODE) << 3) | 4);   \
    }                                                                     \
    [[fallthrough]];                                                      \
    case ((OPCODE) << 3) | 4: {                                           \
        base = config.read_vector(addr);                                  \
        addr = static_cast<std::uint16_t>(addr + 1);                      \
        TAWNY_STEP_TAIL(access_cost_vector(addr), ((OPCODE) << 3) | 5);   \
    }                                                                     \
    [[fallthrough]];                                                      \
    case ((OPCODE) << 3) | 5: {                                           \
        pc = static_cast<std::uint16_t>(                                  \
            (base & 0x00FFu) |                                            \
            (static_cast<std::uint16_t>(config.read_vector(addr)) << 8)); \
        brk_flags = BrkFlags::None;                                       \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 6);                     \
    }                                                                     \
    [[fallthrough]];                                                      \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 6)

// REL_BRANCH(OPCODE, COND) — 2/3/4 cycles. Three outcomes:
//   not-taken — fall straight through to step 3 (opcode fetch).
//   taken, no page cross — one dummy read at target (step 1), then step 3.
//   taken, page cross — dummy read at wrong page, fix pc (step 2), step 3.
// Step-1 and step-2 case labels live inside the relevant branches of the if
// chain in step 0, so the common paths fall through with no tst write and
// no re-dispatch via the switch.

#define TAWNY_REL_BRANCH(OPCODE, COND)                                    \
    case ((OPCODE) << 3) | 0: {                                           \
        pc = static_cast<std::uint16_t>(pc + 1);                          \
        /* _taken / _cross declared without initializers (scalars — legal \
           to jump past), so the step-1 / step-2 case labels below can be \
           reached via the switch without hitting a bypassed init. Both   \
           are always assigned before they're read on any path. */        \
        bool _taken;                                                      \
        bool _cross;                                                      \
        {                                                                 \
            auto _off = static_cast<std::int8_t>(config.read(addr));      \
            _taken = COND::taken(r);                                      \
            if (_taken) {                                                 \
                auto _tgt   = static_cast<std::uint16_t>(                 \
                    pc + static_cast<std::int16_t>(_off));                \
                auto _wrong = static_cast<std::uint16_t>(                 \
                    (pc & 0xFF00u) | (_tgt & 0x00FFu));                   \
                _cross = _wrong != _tgt;                                  \
                pc = _cross ? _wrong : _tgt;                              \
                if (_cross) base = _tgt;                                  \
            }                                                             \
        }                                                                 \
        if (!_taken) {                                                    \
            TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 3);                 \
        } else if (!_cross) {                                             \
            addr = pc;                                                    \
            TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);      \
    case ((OPCODE) << 3) | 1:                                             \
            (void)config.read(addr);                                      \
            TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 3);                 \
        } else {                                                          \
            addr = pc;                                                    \
            TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 2);      \
    case ((OPCODE) << 3) | 2:                                             \
            (void)config.read(addr);                                      \
            pc = base;                                                    \
            TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 3);                 \
        }                                                                 \
    }                                                                     \
    [[fallthrough]];                                                      \
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
    // Layout is deliberate: the 8-byte cycle first (so it's 8-aligned without
    // internal padding), then pc + the u8 registers with brk_flags packed next
    // to p (no hole after p), then the u16 emulation-state fields. Fits the
    // whole working set into the first 24 bytes, and cycle lands at offset 0
    // so the hot load inside run_until is a `mov (%rdi), ...` (no displacement).

    // Start time of the next cycle to run. reset() pre-pays the cost of the
    // first access, so the run loop can enter phi2 on iteration 1 without a
    // bootstrap branch.
    Cycle         cycle{};                         // offset 0

    // Registers.
    std::uint16_t pc{};                            // 8
    std::uint8_t  a{};                             // 10
    std::uint8_t  x{};                             // 11
    std::uint8_t  y{};                             // 12
    std::uint8_t  s{0xFD};                         // 13
    std::uint8_t  p{flag::I | flag::U};            // 14
    BrkFlags      brk_flags{BrkFlags::Reset};      // 15  (packs next to p)

    // Emulation state.
    std::uint16_t tstate{};                        // 16  (ir << 3) | step
    std::uint16_t base_addr{};                     // 18  scratch across multi-step ops

    // Pending bus op — the phi2 the *next* run_until iteration will perform.
    std::uint16_t pending_addr{};                  // 20

    // 22..23: padding to 8-byte align config's first field (unique_ptr).
    Config config;

    explicit M6502(Config cfg) noexcept(std::is_nothrow_move_constructible_v<Config>)
        : config(std::move(cfg))
    {
        reset();
    }

    // Reset kicks off the CPU by entering BRK's microcode with
    // brk_flags = Reset. The BRK step sequence does 3 dummy stack reads (real
    // hardware decrements S; we match that — post-reset S = initial - 3) then
    // fetches the reset vector at $FFFC/$FFFD, jumps there, and fetches the
    // first handler opcode. 7 cycles total.
    //
    // Registers other than `tstate`, `brk_flags`, and `pending_addr` are left
    // indeterminate-ish (zeroed here for deterministic tests; real programs
    // set up A/X/Y/S/P themselves). S starts at 0 and becomes 0xFD via the
    // three dummy-push decrements in BRK steps 1-3.
    auto reset() -> void
    {
        pc           = 0;
        a            = 0;
        x            = 0;
        y            = 0;
        s            = 0;
        p            = flag::U;
        tstate       = 0;               // BRK step 0
        base_addr    = 0;
        brk_flags    = BrkFlags::Reset;
        pending_addr = 0;               // dummy first phi2 read
        cycle        = config.access_cost(pending_addr).cost;
    }

    // Start execution at `target` without going through the reset sequence.
    // Registers (A/X/Y/S/P) are left untouched — the caller is responsible
    // for setting them up if the target code relies on specific values.
    //
    // Mechanism: land in the synthetic opcode-fetch tstate `0x7FF` — added
    // at the end of the switch — so the first dispatch reads the opcode at
    // `target` and jumps into the normal instruction pipeline. `cycle` is
    // pre-paid with the cost of that first fetch (matching reset's pattern).
    auto set_pc(std::uint16_t target) -> void
    {
        pc           = target;
        pending_addr = target;
        tstate       = 0x7FF;
        brk_flags    = BrkFlags::None;
        cycle        = config.access_cost_opcode(target).cost;
    }

    // Stage 1 stub — the single place IRQ/NMI signals would be observed at
    // the boundary of a timeslice. IRQ/NMI pipeline is a follow-up.
    auto sample_interrupts() -> void {}

    auto irq() -> void { /* TODO: interrupt pipeline */ }
    auto nmi() -> void { /* TODO: interrupt pipeline */ }

    auto run_until(Cycle horizon) -> Cycle
    {
        sample_interrupts();

        // Hot state is cached in stack locals for the duration of the loop
        // and written back at `exit:`. Empirically worth ~30-35% over
        // struct-field references — the compiler can't hoist the fields
        // into registers across `config.*` calls, because `config` is a
        // member of `*this` and alias analysis conservatively assumes a
        // call through one member could touch others.
        auto current = cycle;
        auto addr    = pending_addr;
        auto tst     = tstate;
        auto base    = base_addr;
        auto pc      = this->pc;   // shadow member so macros can write bare `pc`
        // Single register-file local. Ops take it by reference and mutate
        // fields in place — one aggregate, no pack/unpack at call sites.
        auto r       = detail::Registers{this->a, this->x, this->y, this->s, this->p};

        while (current < horizon) {
            switch (tst) {
                // Full NMOS 6502 opcode table, laid out in opcode order. * marks
                // illegal-but-stable ops; ! marks illegal-unstable (stubbed as
                // JAM here — implementation would need CPU-specific magic
                // constants). The stable-illegal RMW ops (SLO/RLA/SRE/RRA/DCP/ISC)
                // are implemented only in their zp/zpx/abs/abx forms for now;
                // the aby/izx/izy variants stub as JAM pending ABY_RMW/IZX_RMW/
                // IZY_RMW macros.
                // Legal opcodes, sorted by frequency (Table 3 in
                // Hansotten's 6502 opcode analysis of a PET BASIC
                // sample). Opcodes with no frequency data follow in
                // numeric order.
                TAWNY_JSR       (0x20)                       // JSR abs (freq 684)
                TAWNY_ZP_WRITE  (0x85, detail::Sta)          // STA zp (freq 511)
                TAWNY_ZP_READ   (0xA5, detail::Lda)          // LDA zp (freq 437)
                TAWNY_REL_BRANCH(0xD0, detail::BneCond)      // BNE (freq 388)
                TAWNY_IMM_READ  (0xA9, detail::Lda)          // LDA # (freq 320)
                TAWNY_REL_BRANCH(0xF0, detail::BeqCond)      // BEQ (freq 267)
                TAWNY_ABS_JUMP  (0x4C)                       // JMP abs (freq 221)
                TAWNY_IMM_READ  (0xA0, detail::Ldy)          // LDY # (freq 197)
                TAWNY_IMM_READ  (0xC9, detail::Cmp)          // CMP # (freq 154)
                TAWNY_PULL      (0x68, detail::Pla)          // PLA (freq 144)
                TAWNY_RTS       (0x60)                       // RTS (freq 136)
                TAWNY_ZP_WRITE  (0x84, detail::Sty)          // STY zp (freq 135)
                TAWNY_PUSH      (0x48, detail::Pha)          // PHA (freq 127)
                TAWNY_REL_BRANCH(0x90, detail::BccCond)      // BCC (freq 127)
                TAWNY_IMPLIED   (0xC8, detail::Iny)          // INY (freq 121)
                TAWNY_ZP_WRITE  (0x86, detail::Stx)          // STX zp (freq 116)
                TAWNY_IMM_READ  (0xA2, detail::Ldx)          // LDX # (freq 110)
                TAWNY_IZY_READ  (0xB1, detail::Lda)          // LDA (zp),Y (freq 109)
                TAWNY_ZP_READ   (0xA4, detail::Ldy)          // LDY zp (freq 96)
                TAWNY_ZP_READ   (0xA6, detail::Ldx)          // LDX zp (freq 92)
                TAWNY_REL_BRANCH(0x10, detail::BplCond)      // BPL (freq 75)
                TAWNY_ABS_WRITE (0x8D, detail::Sta)          // STA abs (freq 73)
                TAWNY_IZY_WRITE (0x91, detail::Sta)          // STA (zp),Y (freq 72)
                TAWNY_ZPX_WRITE (0x94, detail::Sty)          // STY zp,X (freq 72)
                TAWNY_ZP_RMW    (0xE6, detail::Inc)          // INC zp (freq 72)
                TAWNY_REL_BRANCH(0xB0, detail::BcsCond)      // BCS (freq 67)
                TAWNY_IMPLIED   (0xAA, detail::Tax)          // TAX (freq 60)
                TAWNY_IMPLIED   (0x18, detail::Clc)          // CLC (freq 56)
                TAWNY_ZP_READ   (0x65, detail::Adc)          // ADC zp (freq 55)
                TAWNY_IMM_READ  (0x69, detail::Adc)          // ADC # (freq 55)
                TAWNY_REL_BRANCH(0x30, detail::BmiCond)      // BMI (freq 54)
                TAWNY_IMPLIED   (0xE8, detail::Inx)          // INX (freq 49)
                TAWNY_IMPLIED   (0x88, detail::Dey)          // DEY (freq 48)
                TAWNY_IMPLIED   (0x8A, detail::Txa)          // TXA (freq 48)
                TAWNY_ZP_RMW    (0xC6, detail::Dec)          // DEC zp (freq 48)
                TAWNY_IMPLIED   (0x98, detail::Tya)          // TYA (freq 46)
                TAWNY_IMPLIED   (0xA8, detail::Tay)          // TAY (freq 45)
                TAWNY_ZP_READ   (0xC5, detail::Cmp)          // CMP zp (freq 43)
                TAWNY_IMPLIED   (0xCA, detail::Dex)          // DEX (freq 43)
                TAWNY_IMM_READ  (0x29, detail::And)          // AND # (freq 40)
                TAWNY_IMPLIED   (0x38, detail::Sec)          // SEC (freq 40)
                TAWNY_ABS_READ  (0xAD, detail::Lda)          // LDA abs (freq 38)
                TAWNY_IMM_READ  (0x49, detail::Eor)          // EOR # (freq 37)
                TAWNY_ABX_READ  (0xBD, detail::Lda)          // LDA abs,X (freq 35)
                TAWNY_IMM_READ  (0x09, detail::Ora)          // ORA # (freq 31)
                TAWNY_ZP_READ   (0xE5, detail::Sbc)          // SBC zp (freq 30)
                TAWNY_ZP_READ   (0x45, detail::Eor)          // EOR zp (freq 19)
                TAWNY_ACC_RMW   (0x4A, detail::Lsr)          // LSR A (freq 19)
                TAWNY_IMM_READ  (0xC0, detail::Cpy)          // CPY # (freq 17)
                TAWNY_IZY_READ  (0xD1, detail::Cmp)          // CMP (zp),Y (freq 17)
                TAWNY_ZP_READ   (0xE4, detail::Cpx)          // CPX zp (freq 17)
                TAWNY_ZP_RMW    (0x46, detail::Lsr)          // LSR zp (freq 14)
                TAWNY_IMPLIED   (0xEA, detail::Nop)          // NOP (freq 14)
                TAWNY_IMPLIED   (0x58, detail::Cli)          // CLI (freq 13)
                TAWNY_ACC_RMW   (0x2A, detail::Rol)          // ROL A (freq 12)
                TAWNY_IMPLIED   (0x78, detail::Sei)          // SEI (freq 12)
                TAWNY_ZP_READ   (0x05, detail::Ora)          // ORA zp (freq 11)
                TAWNY_ZP_RMW    (0x26, detail::Rol)          // ROL zp (freq 11)
                TAWNY_PULL      (0x28, detail::Plp)          // PLP (freq 10)
                TAWNY_IMPLIED   (0x9A, detail::Txs)          // TXS (freq 9)
                TAWNY_ZP_RMW    (0x06, detail::Asl)          // ASL zp (freq 8)
                TAWNY_ZP_READ   (0x25, detail::And)          // AND zp (freq 8)
                TAWNY_REL_BRANCH(0x50, detail::BvcCond)      // BVC (freq 8)
                TAWNY_ZPX_READ  (0xB4, detail::Ldy)          // LDY zp,X (freq 8)
                TAWNY_ABS_WRITE (0x8E, detail::Stx)          // STX abs (freq 7)
                TAWNY_IMPLIED   (0xBA, detail::Tsx)          // TSX (freq 7)
                TAWNY_ABX_READ  (0xDD, detail::Cmp)          // CMP abs,X (freq 7)
                TAWNY_JMP_IND   (0x6C)                       // JMP (ind) (freq 6)
                TAWNY_REL_BRANCH(0x70, detail::BvsCond)      // BVS (freq 6)
                TAWNY_ZPX_RMW   (0x76, detail::Ror)          // ROR zp,X (freq 5)
                TAWNY_ABY_READ  (0x79, detail::Adc)          // ADC abs,Y (freq 4)
                TAWNY_ABS_READ  (0xAE, detail::Ldx)          // LDX abs (freq 4)
                TAWNY_ZPX_READ  (0xF5, detail::Sbc)          // SBC zp,X (freq 4)
                TAWNY_ABS_WRITE (0x8C, detail::Sty)          // STY abs (freq 3)
                TAWNY_ABS_READ  (0xCD, detail::Cmp)          // CMP abs (freq 3)
                TAWNY_IZY_READ  (0xF1, detail::Sbc)          // SBC (zp),Y (freq 3)
                TAWNY_ZPX_RMW   (0x16, detail::Asl)          // ASL zp,X (freq 2)
                TAWNY_RTI       (0x40)                       // RTI (freq 2)
                TAWNY_ZPX_RMW   (0x56, detail::Lsr)          // LSR zp,X (freq 2)
                TAWNY_IZY_READ  (0x71, detail::Adc)          // ADC (zp),Y (freq 2)
                TAWNY_ABS_READ  (0xAC, detail::Ldy)          // LDY abs (freq 2)
                TAWNY_IMPLIED   (0xD8, detail::Cld)          // CLD (freq 2)
                TAWNY_ABY_READ  (0xD9, detail::Cmp)          // CMP abs,Y (freq 2)
                TAWNY_ABS_RMW   (0xEE, detail::Inc)          // INC abs (freq 2)
                TAWNY_ZPX_RMW   (0xF6, detail::Inc)          // INC zp,X (freq 2)
                TAWNY_ABY_READ  (0xF9, detail::Sbc)          // SBC abs,Y (freq 1)
                TAWNY_ABX_READ  (0xFD, detail::Sbc)          // SBC abs,X (freq 1)
                TAWNY_BRK       (0x00)                       // BRK
                TAWNY_IZX_READ  (0x01, detail::Ora)          // ORA (zp,X)
                TAWNY_PUSH      (0x08, detail::Php)          // PHP
                TAWNY_ACC_RMW   (0x0A, detail::Asl)          // ASL A
                TAWNY_ABS_READ  (0x0D, detail::Ora)          // ORA abs
                TAWNY_ABS_RMW   (0x0E, detail::Asl)          // ASL abs
                TAWNY_IZY_READ  (0x11, detail::Ora)          // ORA (zp),Y
                TAWNY_ZPX_READ  (0x15, detail::Ora)          // ORA zp,X
                TAWNY_ABY_READ  (0x19, detail::Ora)          // ORA abs,Y
                TAWNY_ABX_READ  (0x1D, detail::Ora)          // ORA abs,X
                TAWNY_ABX_RMW   (0x1E, detail::Asl)          // ASL abs,X
                TAWNY_IZX_READ  (0x21, detail::And)          // AND (zp,X)
                TAWNY_ZP_READ   (0x24, detail::Bit)          // BIT zp
                TAWNY_ABS_READ  (0x2C, detail::Bit)          // BIT abs
                TAWNY_ABS_READ  (0x2D, detail::And)          // AND abs
                TAWNY_ABS_RMW   (0x2E, detail::Rol)          // ROL abs
                TAWNY_IZY_READ  (0x31, detail::And)          // AND (zp),Y
                TAWNY_ZPX_READ  (0x35, detail::And)          // AND zp,X
                TAWNY_ZPX_RMW   (0x36, detail::Rol)          // ROL zp,X
                TAWNY_ABY_READ  (0x39, detail::And)          // AND abs,Y
                TAWNY_ABX_READ  (0x3D, detail::And)          // AND abs,X
                TAWNY_ABX_RMW   (0x3E, detail::Rol)          // ROL abs,X
                TAWNY_IZX_READ  (0x41, detail::Eor)          // EOR (zp,X)
                TAWNY_ABS_READ  (0x4D, detail::Eor)          // EOR abs
                TAWNY_ABS_RMW   (0x4E, detail::Lsr)          // LSR abs
                TAWNY_IZY_READ  (0x51, detail::Eor)          // EOR (zp),Y
                TAWNY_ZPX_READ  (0x55, detail::Eor)          // EOR zp,X
                TAWNY_ABY_READ  (0x59, detail::Eor)          // EOR abs,Y
                TAWNY_ABX_READ  (0x5D, detail::Eor)          // EOR abs,X
                TAWNY_ABX_RMW   (0x5E, detail::Lsr)          // LSR abs,X
                TAWNY_IZX_READ  (0x61, detail::Adc)          // ADC (zp,X)
                TAWNY_ZP_RMW    (0x66, detail::Ror)          // ROR zp
                TAWNY_ACC_RMW   (0x6A, detail::Ror)          // ROR A
                TAWNY_ABS_READ  (0x6D, detail::Adc)          // ADC abs
                TAWNY_ABS_RMW   (0x6E, detail::Ror)          // ROR abs
                TAWNY_ZPX_READ  (0x75, detail::Adc)          // ADC zp,X
                TAWNY_ABX_READ  (0x7D, detail::Adc)          // ADC abs,X
                TAWNY_ABX_RMW   (0x7E, detail::Ror)          // ROR abs,X
                TAWNY_IZX_WRITE (0x81, detail::Sta)          // STA (zp,X)
                TAWNY_ZPX_WRITE (0x95, detail::Sta)          // STA zp,X
                TAWNY_ZPY_WRITE (0x96, detail::Stx)          // STX zp,Y
                TAWNY_ABY_WRITE (0x99, detail::Sta)          // STA abs,Y
                TAWNY_ABX_WRITE (0x9D, detail::Sta)          // STA abs,X
                TAWNY_IZX_READ  (0xA1, detail::Lda)          // LDA (zp,X)
                TAWNY_ZPX_READ  (0xB5, detail::Lda)          // LDA zp,X
                TAWNY_ZPY_READ  (0xB6, detail::Ldx)          // LDX zp,Y
                TAWNY_IMPLIED   (0xB8, detail::Clv)          // CLV
                TAWNY_ABY_READ  (0xB9, detail::Lda)          // LDA abs,Y
                TAWNY_ABX_READ  (0xBC, detail::Ldy)          // LDY abs,X
                TAWNY_ABY_READ  (0xBE, detail::Ldx)          // LDX abs,Y
                TAWNY_IZX_READ  (0xC1, detail::Cmp)          // CMP (zp,X)
                TAWNY_ZP_READ   (0xC4, detail::Cpy)          // CPY zp
                TAWNY_ABS_READ  (0xCC, detail::Cpy)          // CPY abs
                TAWNY_ABS_RMW   (0xCE, detail::Dec)          // DEC abs
                TAWNY_ZPX_READ  (0xD5, detail::Cmp)          // CMP zp,X
                TAWNY_ZPX_RMW   (0xD6, detail::Dec)          // DEC zp,X
                TAWNY_ABX_RMW   (0xDE, detail::Dec)          // DEC abs,X
                TAWNY_IMM_READ  (0xE0, detail::Cpx)          // CPX #
                TAWNY_IZX_READ  (0xE1, detail::Sbc)          // SBC (zp,X)
                TAWNY_IMM_READ  (0xE9, detail::Sbc)          // SBC #
                TAWNY_ABS_READ  (0xEC, detail::Cpx)          // CPX abs
                TAWNY_ABS_READ  (0xED, detail::Sbc)          // SBC abs
                TAWNY_IMPLIED   (0xF8, detail::Sed)          // SED
                TAWNY_ABX_RMW   (0xFE, detail::Inc)          // INC abs,X

                // Synthetic "bootstrap opcode fetch" tstate (0x7FF) used by
                // set_pc(). Step 7 isn't produced by normal dispatch (longest
                // instruction = 7 cycles, steps 0-6), so this slot is free.
                TAWNY_FETCH_OPCODE_CASE(0xFF, 7)

                // Illegal opcodes (JAM stubs + stable illegals) in numeric order.
                TAWNY_JAM       (0x02)                       // JAM*
                TAWNY_IZX_RMW   (0x03, detail::Slo)          // SLO (zp,X)*
                TAWNY_ZP_READ   (0x04, detail::Nop)          // NOP zp*
                TAWNY_ZP_RMW    (0x07, detail::Slo)          // SLO zp*
                TAWNY_IMM_READ  (0x0B, detail::Anc)          // ANC #*
                TAWNY_ABS_READ  (0x0C, detail::Nop)          // NOP abs*
                TAWNY_ABS_RMW   (0x0F, detail::Slo)          // SLO abs*
                TAWNY_JAM       (0x12)                       // JAM*
                TAWNY_IZY_RMW   (0x13, detail::Slo)          // SLO (zp),Y*
                TAWNY_ZPX_READ  (0x14, detail::Nop)          // NOP zp,X*
                TAWNY_ZPX_RMW   (0x17, detail::Slo)          // SLO zp,X*
                TAWNY_IMPLIED   (0x1A, detail::Nop)          // NOP*
                TAWNY_ABY_RMW   (0x1B, detail::Slo)          // SLO abs,Y*
                TAWNY_ABX_READ  (0x1C, detail::Nop)          // NOP abs,X*
                TAWNY_ABX_RMW   (0x1F, detail::Slo)          // SLO abs,X*
                TAWNY_JAM       (0x22)                       // JAM*
                TAWNY_IZX_RMW   (0x23, detail::Rla)          // RLA (zp,X)*
                TAWNY_ZP_RMW    (0x27, detail::Rla)          // RLA zp*
                TAWNY_IMM_READ  (0x2B, detail::Anc)          // ANC #*
                TAWNY_ABS_RMW   (0x2F, detail::Rla)          // RLA abs*
                TAWNY_JAM       (0x32)                       // JAM*
                TAWNY_IZY_RMW   (0x33, detail::Rla)          // RLA (zp),Y*
                TAWNY_ZPX_READ  (0x34, detail::Nop)          // NOP zp,X*
                TAWNY_ZPX_RMW   (0x37, detail::Rla)          // RLA zp,X*
                TAWNY_IMPLIED   (0x3A, detail::Nop)          // NOP*
                TAWNY_ABY_RMW   (0x3B, detail::Rla)          // RLA abs,Y*
                TAWNY_ABX_READ  (0x3C, detail::Nop)          // NOP abs,X*
                TAWNY_ABX_RMW   (0x3F, detail::Rla)          // RLA abs,X*
                TAWNY_JAM       (0x42)                       // JAM*
                TAWNY_IZX_RMW   (0x43, detail::Sre)          // SRE (zp,X)*
                TAWNY_ZP_READ   (0x44, detail::Nop)          // NOP zp*
                TAWNY_ZP_RMW    (0x47, detail::Sre)          // SRE zp*
                TAWNY_IMM_READ  (0x4B, detail::Alr)          // ALR #*
                TAWNY_ABS_RMW   (0x4F, detail::Sre)          // SRE abs*
                TAWNY_JAM       (0x52)                       // JAM*
                TAWNY_IZY_RMW   (0x53, detail::Sre)          // SRE (zp),Y*
                TAWNY_ZPX_READ  (0x54, detail::Nop)          // NOP zp,X*
                TAWNY_ZPX_RMW   (0x57, detail::Sre)          // SRE zp,X*
                TAWNY_IMPLIED   (0x5A, detail::Nop)          // NOP*
                TAWNY_ABY_RMW   (0x5B, detail::Sre)          // SRE abs,Y*
                TAWNY_ABX_READ  (0x5C, detail::Nop)          // NOP abs,X*
                TAWNY_ABX_RMW   (0x5F, detail::Sre)          // SRE abs,X*
                TAWNY_JAM       (0x62)                       // JAM*
                TAWNY_IZX_RMW   (0x63, detail::Rra)          // RRA (zp,X)*
                TAWNY_ZP_READ   (0x64, detail::Nop)          // NOP zp*
                TAWNY_ZP_RMW    (0x67, detail::Rra)          // RRA zp*
                TAWNY_IMM_READ  (0x6B, detail::Arr)          // ARR #*
                TAWNY_ABS_RMW   (0x6F, detail::Rra)          // RRA abs*
                TAWNY_JAM       (0x72)                       // JAM*
                TAWNY_IZY_RMW   (0x73, detail::Rra)          // RRA (zp),Y*
                TAWNY_ZPX_READ  (0x74, detail::Nop)          // NOP zp,X*
                TAWNY_ZPX_RMW   (0x77, detail::Rra)          // RRA zp,X*
                TAWNY_IMPLIED   (0x7A, detail::Nop)          // NOP*
                TAWNY_ABY_RMW   (0x7B, detail::Rra)          // RRA abs,Y*
                TAWNY_ABX_READ  (0x7C, detail::Nop)          // NOP abs,X*
                TAWNY_ABX_RMW   (0x7F, detail::Rra)          // RRA abs,X*
                TAWNY_IMM_READ  (0x80, detail::Nop)          // NOP #*
                TAWNY_IMM_READ  (0x82, detail::Nop)          // NOP #*
                TAWNY_IZX_WRITE (0x83, detail::Sax)          // SAX (zp,X)*
                TAWNY_ZP_WRITE  (0x87, detail::Sax)          // SAX zp*
                TAWNY_IMM_READ  (0x89, detail::Nop)          // NOP #*
                TAWNY_IMM_READ  (0x8B, detail::Ane)          // ANE #! (unstable)
                TAWNY_ABS_WRITE (0x8F, detail::Sax)          // SAX abs*
                TAWNY_JAM       (0x92)                       // JAM*
                TAWNY_JAM       (0x93)                       // SHA (zp),Y!
                TAWNY_ZPY_WRITE (0x97, detail::Sax)          // SAX zp,Y*
                TAWNY_JAM       (0x9B)                       // TAS abs,Y!
                TAWNY_JAM       (0x9C)                       // SHY abs,X!
                TAWNY_JAM       (0x9E)                       // SHX abs,Y!
                TAWNY_JAM       (0x9F)                       // SHA abs,Y!
                TAWNY_IZX_READ  (0xA3, detail::Lax)          // LAX (zp,X)*
                TAWNY_ZP_READ   (0xA7, detail::Lax)          // LAX zp*
                TAWNY_IMM_READ  (0xAB, detail::Lxa)          // LXA #! (unstable)
                TAWNY_ABS_READ  (0xAF, detail::Lax)          // LAX abs*
                TAWNY_JAM       (0xB2)                       // JAM*
                TAWNY_IZY_READ  (0xB3, detail::Lax)          // LAX (zp),Y*
                TAWNY_ZPY_READ  (0xB7, detail::Lax)          // LAX zp,Y*
                TAWNY_ABY_READ  (0xBB, detail::Las)          // LAS abs,Y! (unstable)
                TAWNY_ABY_READ  (0xBF, detail::Lax)          // LAX abs,Y*
                TAWNY_IMM_READ  (0xC2, detail::Nop)          // NOP #*
                TAWNY_IZX_RMW   (0xC3, detail::Dcp)          // DCP (zp,X)*
                TAWNY_ZP_RMW    (0xC7, detail::Dcp)          // DCP zp*
                TAWNY_IMM_READ  (0xCB, detail::Axs)          // AXS #*
                TAWNY_ABS_RMW   (0xCF, detail::Dcp)          // DCP abs*
                TAWNY_JAM       (0xD2)                       // JAM*
                TAWNY_IZY_RMW   (0xD3, detail::Dcp)          // DCP (zp),Y*
                TAWNY_ZPX_READ  (0xD4, detail::Nop)          // NOP zp,X*
                TAWNY_ZPX_RMW   (0xD7, detail::Dcp)          // DCP zp,X*
                TAWNY_IMPLIED   (0xDA, detail::Nop)          // NOP*
                TAWNY_ABY_RMW   (0xDB, detail::Dcp)          // DCP abs,Y*
                TAWNY_ABX_READ  (0xDC, detail::Nop)          // NOP abs,X*
                TAWNY_ABX_RMW   (0xDF, detail::Dcp)          // DCP abs,X*
                TAWNY_IMM_READ  (0xE2, detail::Nop)          // NOP #*
                TAWNY_IZX_RMW   (0xE3, detail::Isc)          // ISC (zp,X)*
                TAWNY_ZP_RMW    (0xE7, detail::Isc)          // ISC zp*
                TAWNY_IMM_READ  (0xEB, detail::Usbc)         // USBC #*
                TAWNY_ABS_RMW   (0xEF, detail::Isc)          // ISC abs*
                TAWNY_JAM       (0xF2)                       // JAM*
                TAWNY_IZY_RMW   (0xF3, detail::Isc)          // ISC (zp),Y*
                TAWNY_ZPX_READ  (0xF4, detail::Nop)          // NOP zp,X*
                TAWNY_ZPX_RMW   (0xF7, detail::Isc)          // ISC zp,X*
                TAWNY_IMPLIED   (0xFA, detail::Nop)          // NOP*
                TAWNY_ABY_RMW   (0xFB, detail::Isc)          // ISC abs,Y*
                TAWNY_ABX_READ  (0xFC, detail::Nop)          // NOP abs,X*
                TAWNY_ABX_RMW   (0xFF, detail::Isc)          // ISC abs,X*



                default:
                    // Unreachable — every tstate value is covered. If we get
                    // here it means a test has a bug; stop to prevent infinite
                    // looping.
                    goto exit;
            }
        }

    exit:
        cycle        = current;
        pending_addr = addr;
        tstate       = tst;
        base_addr    = base;
        this->pc     = pc;
        this->a      = r.a;
        this->x      = r.x;
        this->y      = r.y;
        this->s      = r.s;
        this->p      = r.p;
        return current;
    }
};

}  // namespace tawny

// Keep dispatch macros from leaking out of this header.
#undef TAWNY_STEP_TAIL
#undef TAWNY_NEXT_STACK
#undef TAWNY_NEXT_OPCODE_FETCH
#undef TAWNY_FETCH_OPCODE_CASE
#undef TAWNY_BRK
#undef TAWNY_IMPLIED
#undef TAWNY_IMM_READ
#undef TAWNY_ZP_READ
#undef TAWNY_ZP_WRITE
#undef TAWNY_ABS_READ
#undef TAWNY_ABS_WRITE
#undef TAWNY_ABS_JUMP
#undef TAWNY_REL_BRANCH
#undef TAWNY_ZP_INDEXED_READ
#undef TAWNY_ZP_INDEXED_WRITE
#undef TAWNY_ZPX_READ
#undef TAWNY_ZPY_READ
#undef TAWNY_ZPX_WRITE
#undef TAWNY_ZPY_WRITE
#undef TAWNY_AB_INDEXED_READ
#undef TAWNY_AB_INDEXED_WRITE
#undef TAWNY_ABX_READ
#undef TAWNY_ABY_READ
#undef TAWNY_ABX_WRITE
#undef TAWNY_ABY_WRITE
#undef TAWNY_IZX_READ
#undef TAWNY_IZX_WRITE
#undef TAWNY_IZY_READ
#undef TAWNY_IZY_WRITE
#undef TAWNY_ACC_RMW
#undef TAWNY_ZP_RMW
#undef TAWNY_ZPX_RMW
#undef TAWNY_ABS_RMW
#undef TAWNY_AB_INDEXED_RMW
#undef TAWNY_ABX_RMW
#undef TAWNY_ABY_RMW
#undef TAWNY_IZX_RMW
#undef TAWNY_IZY_RMW
#undef TAWNY_JMP_IND
#undef TAWNY_JSR
#undef TAWNY_RTS
#undef TAWNY_RTI
#undef TAWNY_PUSH
#undef TAWNY_PULL
#undef TAWNY_JAM

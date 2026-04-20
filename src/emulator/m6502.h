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

// Lightweight "register view" passed to op classes in place of the whole
// M6502 reference. Holds references to run_until's register locals so that
// ops like `Lda::apply(v) { c.a = v; set_nz(c.p, v); }` mutate the locals,
// not the struct fields. Op classes stay template-generic on C and continue
// to work whether called with an M6502 reference or this view.
struct RegView {
    std::uint8_t &a;
    std::uint8_t &x;
    std::uint8_t &y;
    std::uint8_t &s;
    std::uint8_t &p;
};

// Op classes — small structs providing the per-mnemonic transform. Reused
// across addressing-mode macros. All methods templated on the CPU type so
// they compose with the templated M6502<Config>.

// ---- Read ops (consume a byte, update register/flags) ----
struct Lda { template <typename C> static void apply(C &c, std::uint8_t v) { c.a = v; set_nz(c.p, v); } };
struct Ldx { template <typename C> static void apply(C &c, std::uint8_t v) { c.x = v; set_nz(c.p, v); } };
struct Ldy { template <typename C> static void apply(C &c, std::uint8_t v) { c.y = v; set_nz(c.p, v); } };

struct And { template <typename C> static void apply(C &c, std::uint8_t v) { c.a = static_cast<std::uint8_t>(c.a & v); set_nz(c.p, c.a); } };
struct Ora { template <typename C> static void apply(C &c, std::uint8_t v) { c.a = static_cast<std::uint8_t>(c.a | v); set_nz(c.p, c.a); } };
struct Eor { template <typename C> static void apply(C &c, std::uint8_t v) { c.a = static_cast<std::uint8_t>(c.a ^ v); set_nz(c.p, c.a); } };

struct Bit {
    template <typename C> static void apply(C &c, std::uint8_t v)
    {
        set_flag(c.p, flag::N, (v & 0x80) != 0);
        set_flag(c.p, flag::V, (v & 0x40) != 0);
        set_flag(c.p, flag::Z, (c.a & v) == 0);
    }
};

// Compares: reg - v, set N/Z from result; C = (reg >= v).
struct Cmp { template <typename C> static void apply(C &c, std::uint8_t v) { auto r = static_cast<std::uint8_t>(c.a - v); set_nz(c.p, r); set_flag(c.p, flag::C, c.a >= v); } };
struct Cpx { template <typename C> static void apply(C &c, std::uint8_t v) { auto r = static_cast<std::uint8_t>(c.x - v); set_nz(c.p, r); set_flag(c.p, flag::C, c.x >= v); } };
struct Cpy { template <typename C> static void apply(C &c, std::uint8_t v) { auto r = static_cast<std::uint8_t>(c.y - v); set_nz(c.p, r); set_flag(c.p, flag::C, c.y >= v); } };

// ADC / SBC — binary path always exists; decimal path taken when D is set.
// Public `apply_binary` / `apply_decimal` so SLO/RLA/etc. and SBC can reuse
// the binary path directly.
struct Adc {
    template <typename C> static void apply_binary(C &c, std::uint8_t v)
    {
        unsigned a = c.a, m = v, cin = (c.p & flag::C) ? 1u : 0u;
        unsigned sum = a + m + cin;
        bool vflg = ((~(a ^ m) & (a ^ sum)) & 0x80u) != 0;
        c.a = static_cast<std::uint8_t>(sum);
        set_nz(c.p, c.a);
        set_flag(c.p, flag::C, sum > 0xFFu);
        set_flag(c.p, flag::V, vflg);
    }
    template <typename C> static void apply_decimal(C &c, std::uint8_t v)
    {
        // NMOS 6502 decimal ADC: N/V flags reflect the binary calculation's
        // result, while A and C reflect the BCD-adjusted sum.
        unsigned a = c.a, m = v, cin = (c.p & flag::C) ? 1u : 0u;
        unsigned bin_sum = a + m + cin;
        set_flag(c.p, flag::Z, (bin_sum & 0xFFu) == 0);
        unsigned lo = (a & 0x0Fu) + (m & 0x0Fu) + cin;
        if (lo > 0x09u) lo += 0x06u;
        unsigned sum = (a & 0xF0u) + (m & 0xF0u) + (lo > 0x0Fu ? 0x10u : 0u) + (lo & 0x0Fu);
        set_flag(c.p, flag::N, (sum & 0x80u) != 0);
        set_flag(c.p, flag::V, ((~(a ^ m) & (a ^ sum)) & 0x80u) != 0);
        if (sum > 0x9Fu) sum += 0x60u;
        set_flag(c.p, flag::C, sum > 0xFFu);
        c.a = static_cast<std::uint8_t>(sum);
    }
    template <typename C> static void apply(C &c, std::uint8_t v)
    {
        if (c.p & flag::D) apply_decimal(c, v);
        else               apply_binary (c, v);
    }
};
struct Sbc {
    template <typename C> static void apply_binary(C &c, std::uint8_t v)
    {
        Adc::apply_binary(c, static_cast<std::uint8_t>(~v));
    }
    template <typename C> static void apply_decimal(C &c, std::uint8_t v)
    {
        // NMOS 6502 decimal SBC: N/V/Z/C flags reflect the binary calculation;
        // A reflects the BCD-adjusted difference.
        unsigned a = c.a, m = v, cin = (c.p & flag::C) ? 1u : 0u;
        unsigned bin = (a - m - (1u - cin)) & 0xFFFFu;
        set_flag(c.p, flag::Z, (bin & 0xFFu) == 0);
        set_flag(c.p, flag::N, (bin & 0x80u) != 0);
        set_flag(c.p, flag::V, (((a ^ m) & (a ^ bin)) & 0x80u) != 0);
        set_flag(c.p, flag::C, bin < 0x100u);
        unsigned lo = (a & 0x0Fu) - (m & 0x0Fu) - (1u - cin);
        unsigned result;
        if (lo & 0x10u) {
            result = ((lo - 0x06u) & 0x0Fu) | (((a & 0xF0u) - (m & 0xF0u) - 0x10u) & 0xFFF0u);
        } else {
            result = (lo & 0x0Fu) | (((a & 0xF0u) - (m & 0xF0u)) & 0xFFF0u);
        }
        if (result & 0x100u) result -= 0x60u;
        c.a = static_cast<std::uint8_t>(result);
    }
    template <typename C> static void apply(C &c, std::uint8_t v)
    {
        if (c.p & flag::D) apply_decimal(c, v);
        else               apply_binary (c, v);
    }
};

// ---- Store ops (return a byte to write to memory) ----
struct Sta { template <typename C> static auto value(const C &c) -> std::uint8_t { return c.a; } };
struct Stx { template <typename C> static auto value(const C &c) -> std::uint8_t { return c.x; } };
struct Sty { template <typename C> static auto value(const C &c) -> std::uint8_t { return c.y; } };
// SAX stores A & X (illegal but stable).
struct Sax { template <typename C> static auto value(const C &c) -> std::uint8_t { return static_cast<std::uint8_t>(c.a & c.x); } };

// ---- Stack push/pull ops. Use the same shape as Store/Read ops — `value`
// returns the byte to push, `apply(cpu, v)` consumes the pulled byte.
struct Pha { template <typename C> static auto value(const C &c) -> std::uint8_t { return c.a; } };
// PHP always pushes with B and U set (those bits are only meaningful on the stack).
struct Php { template <typename C> static auto value(const C &c) -> std::uint8_t { return static_cast<std::uint8_t>(c.p | flag::B | flag::U); } };
struct Pla { template <typename C> static void apply(C &c, std::uint8_t v) { c.a = v; set_nz(c.p, v); } };
// PLP: strip B, ensure U (they're stack-only bits).
struct Plp { template <typename C> static void apply(C &c, std::uint8_t v) { c.p = static_cast<std::uint8_t>((v & ~flag::B) | flag::U); } };

// ---- Implied ops (operate on registers only) ----
struct Nop {
    template <typename C> static void apply(C &) {}
    // Overload for read-addressing illegal NOPs: fetch happens, result is
    // discarded, no CPU state changes.
    template <typename C> static void apply(C &, std::uint8_t) {}
};
struct Inx { template <typename C> static void apply(C &c) { c.x = static_cast<std::uint8_t>(c.x + 1); set_nz(c.p, c.x); } };
struct Iny { template <typename C> static void apply(C &c) { c.y = static_cast<std::uint8_t>(c.y + 1); set_nz(c.p, c.y); } };
struct Dex { template <typename C> static void apply(C &c) { c.x = static_cast<std::uint8_t>(c.x - 1); set_nz(c.p, c.x); } };
struct Dey { template <typename C> static void apply(C &c) { c.y = static_cast<std::uint8_t>(c.y - 1); set_nz(c.p, c.y); } };
struct Tax { template <typename C> static void apply(C &c) { c.x = c.a; set_nz(c.p, c.x); } };
struct Tay { template <typename C> static void apply(C &c) { c.y = c.a; set_nz(c.p, c.y); } };
struct Tsx { template <typename C> static void apply(C &c) { c.x = c.s; set_nz(c.p, c.x); } };
struct Txa { template <typename C> static void apply(C &c) { c.a = c.x; set_nz(c.p, c.a); } };
struct Txs { template <typename C> static void apply(C &c) { c.s = c.x; /* TXS does NOT set flags */ } };
struct Tya { template <typename C> static void apply(C &c) { c.a = c.y; set_nz(c.p, c.a); } };
struct Clc { template <typename C> static void apply(C &c) { set_flag(c.p, flag::C, false); } };
struct Sec { template <typename C> static void apply(C &c) { set_flag(c.p, flag::C, true); } };
struct Cli { template <typename C> static void apply(C &c) { set_flag(c.p, flag::I, false); } };
struct Sei { template <typename C> static void apply(C &c) { set_flag(c.p, flag::I, true); } };
struct Clv { template <typename C> static void apply(C &c) { set_flag(c.p, flag::V, false); } };
struct Cld { template <typename C> static void apply(C &c) { set_flag(c.p, flag::D, false); } };
struct Sed { template <typename C> static void apply(C &c) { set_flag(c.p, flag::D, true); } };

// ---- RMW ops (read a value, return the modified value) ----
struct Asl { template <typename C> static auto apply(C &c, std::uint8_t v) -> std::uint8_t { set_flag(c.p, flag::C, (v & 0x80) != 0); auto r = static_cast<std::uint8_t>(v << 1); set_nz(c.p, r); return r; } };
struct Lsr { template <typename C> static auto apply(C &c, std::uint8_t v) -> std::uint8_t { set_flag(c.p, flag::C, (v & 0x01) != 0); auto r = static_cast<std::uint8_t>(v >> 1); set_nz(c.p, r); return r; } };
struct Rol { template <typename C> static auto apply(C &c, std::uint8_t v) -> std::uint8_t { auto cin = (c.p & flag::C) ? 1u : 0u; set_flag(c.p, flag::C, (v & 0x80) != 0); auto r = static_cast<std::uint8_t>((v << 1) | cin); set_nz(c.p, r); return r; } };
struct Ror { template <typename C> static auto apply(C &c, std::uint8_t v) -> std::uint8_t { auto cin = (c.p & flag::C) ? 0x80u : 0u; set_flag(c.p, flag::C, (v & 0x01) != 0); auto r = static_cast<std::uint8_t>((v >> 1) | cin); set_nz(c.p, r); return r; } };
struct Inc { template <typename C> static auto apply(C &c, std::uint8_t v) -> std::uint8_t { auto r = static_cast<std::uint8_t>(v + 1); set_nz(c.p, r); return r; } };
struct Dec { template <typename C> static auto apply(C &c, std::uint8_t v) -> std::uint8_t { auto r = static_cast<std::uint8_t>(v - 1); set_nz(c.p, r); return r; } };

// ---- Branch conditions ----
struct BplCond { template <typename C> static auto taken(const C &c) -> bool { return (c.p & flag::N) == 0; } };
struct BmiCond { template <typename C> static auto taken(const C &c) -> bool { return (c.p & flag::N) != 0; } };
struct BvcCond { template <typename C> static auto taken(const C &c) -> bool { return (c.p & flag::V) == 0; } };
struct BvsCond { template <typename C> static auto taken(const C &c) -> bool { return (c.p & flag::V) != 0; } };
struct BccCond { template <typename C> static auto taken(const C &c) -> bool { return (c.p & flag::C) == 0; } };
struct BcsCond { template <typename C> static auto taken(const C &c) -> bool { return (c.p & flag::C) != 0; } };
struct BneCond { template <typename C> static auto taken(const C &c) -> bool { return (c.p & flag::Z) == 0; } };
struct BeqCond { template <typename C> static auto taken(const C &c) -> bool { return (c.p & flag::Z) != 0; } };

// ---- Stable illegal ops combining legal ones ----
// LAX = LDA + LDX (read).
struct Lax { template <typename C> static void apply(C &c, std::uint8_t v) { c.a = v; c.x = v; set_nz(c.p, v); } };
// SLO, RLA, SRE, RRA, DCP, ISC: RMW → combined second op applied to A.
struct Slo { template <typename C> static auto apply(C &c, std::uint8_t v) -> std::uint8_t { auto r = Asl::apply(c, v); Ora::apply(c, r); return r; } };
struct Rla { template <typename C> static auto apply(C &c, std::uint8_t v) -> std::uint8_t { auto r = Rol::apply(c, v); And::apply(c, r); return r; } };
struct Sre { template <typename C> static auto apply(C &c, std::uint8_t v) -> std::uint8_t { auto r = Lsr::apply(c, v); Eor::apply(c, r); return r; } };
struct Rra { template <typename C> static auto apply(C &c, std::uint8_t v) -> std::uint8_t { auto r = Ror::apply(c, v); Adc::apply(c, r); return r; } };
struct Dcp { template <typename C> static auto apply(C &c, std::uint8_t v) -> std::uint8_t { auto r = Dec::apply(c, v); Cmp::apply(c, r); return r; } };
struct Isc { template <typename C> static auto apply(C &c, std::uint8_t v) -> std::uint8_t { auto r = Inc::apply(c, v); Sbc::apply(c, r); return r; } };

// Immediate-only illegals.
struct Anc { template <typename C> static void apply(C &c, std::uint8_t v) { And::apply(c, v); set_flag(c.p, flag::C, (c.a & 0x80) != 0); } };
struct Alr { template <typename C> static void apply(C &c, std::uint8_t v) { And::apply(c, v); c.a = Lsr::apply(c, c.a); } };
// ARR: A = ((A & v) >> 1) | (C << 7), then set flags with the weird ARR rule.
struct Arr {
    template <typename C> static void apply(C &c, std::uint8_t v)
    {
        And::apply(c, v);
        auto cin = (c.p & flag::C) ? 0x80u : 0u;
        c.a = static_cast<std::uint8_t>((c.a >> 1) | cin);
        set_nz(c.p, c.a);
        set_flag(c.p, flag::C, (c.a & 0x40) != 0);
        set_flag(c.p, flag::V, ((c.a >> 5) ^ (c.a >> 6)) & 0x01);
    }
};
// AXS: X = (A & X) - imm. Sets C/N/Z as if compare.
struct Axs {
    template <typename C> static void apply(C &c, std::uint8_t v)
    {
        auto ax = static_cast<std::uint8_t>(c.a & c.x);
        auto r  = static_cast<std::uint8_t>(ax - v);
        set_nz(c.p, r);
        set_flag(c.p, flag::C, ax >= v);
        c.x = r;
    }
};
// USBC is undocumented but bit-equivalent to SBC.
struct Usbc { template <typename C> static void apply(C &c, std::uint8_t v) { Sbc::apply(c, v); } };

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
#define TAWNY_NEXT_STACK(NEXT_TST)                                            \
    addr = static_cast<std::uint16_t>(0x0100u | s);                     \
    TAWNY_STEP_TAIL(access_cost_stack(s), (NEXT_TST))

// Shorthand: set the next phi2 to an opcode fetch at PC, then run the tail.
// (Used at the end of every penultimate step.)
#define TAWNY_NEXT_OPCODE_FETCH(NEXT_TST)                                     \
    addr = pc;                                                          \
    TAWNY_STEP_TAIL(access_cost_opcode(addr), (NEXT_TST))

// FETCH_OPCODE_CASE — the last step of every instruction. Reads the next
// opcode byte (via read_opcode for the sync-read semantics), shifts it into a
// step-0 tstate for the new instruction, sets up the operand-fetch address,
// and breaks out so the while loop re-enters the switch at the new tstate.

#define TAWNY_FETCH_OPCODE_CASE(OPCODE, STEP)                              \
    case ((OPCODE) << 3) | (STEP): {                                       \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        tst  = static_cast<std::uint16_t>(                                 \
                   static_cast<std::uint16_t>(config.read_opcode(addr))    \
                   << 3);                                                  \
        addr = pc;                                                   \
        TAWNY_STEP_TAIL(access_cost(addr), tst);                           \
        break;                                                             \
    }

// IMPLIED(OPCODE, OP_CLASS) — 2 cycles. Step 0 is a discarded operand-fetch
// read; pc is NOT incremented (implied ops consume no bytes after the opcode).

#define TAWNY_IMPLIED(OPCODE, OP_CLASS)                                    \
    case ((OPCODE) << 3) | 0: {                                            \
        (void)config.read(addr);                                           \
        OP_CLASS::apply(view);                                            \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 1);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 1)

// IMM_READ(OPCODE, OP_CLASS) — 2 cycles. Step 0 reads the immediate byte and
// applies the op; pc advances past it.

#define TAWNY_IMM_READ(OPCODE, OP_CLASS)                                   \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        OP_CLASS::apply(view, config.read(addr));                         \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 1);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 1)

// ZP_READ(OPCODE, OP_CLASS) — 3 cycles.
//   step 0: read ZP-addr byte, stash in addr (high bits 0).
//   step 1: read value from ZP, apply op.
//   step 2: opcode fetch for next instruction.

#define TAWNY_ZP_READ(OPCODE, OP_CLASS)                                    \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        addr = config.read(addr);                                          \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 1);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        OP_CLASS::apply(view,                                             \
            config.read_zp(static_cast<std::uint8_t>(addr)));              \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 2);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 2)

// ZP_WRITE(OPCODE, OP_CLASS) — 3 cycles.
//   step 0: read ZP-addr byte.
//   step 1: write op_class::value(cpu) to ZP.
//   step 2: opcode fetch for next instruction.

#define TAWNY_ZP_WRITE(OPCODE, OP_CLASS)                                   \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        addr = config.read(addr);                                          \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 1);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        config.write_zp(static_cast<std::uint8_t>(addr),                   \
                        OP_CLASS::value(view));                           \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 2);                      \
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
        pc = static_cast<std::uint16_t>(pc + 1);               \
        base = config.read(addr);                                          \
        addr = pc;                                                   \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        addr = static_cast<std::uint16_t>(                                 \
            (base & 0x00FFu) |                                             \
            (static_cast<std::uint16_t>(config.read(addr)) << 8));         \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 2);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        OP_CLASS::apply(view, config.read(addr));                         \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 3);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 3)

// ABS_JUMP(OPCODE) — JMP abs, 3 cycles. No op class; updates pc in place.
//   step 0: read addr lo (into base).
//   step 1: read addr hi, set pc = (hi << 8) | (base & 0xFF).
//   step 2: opcode fetch at new pc.

#define TAWNY_ABS_JUMP(OPCODE)                                             \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        base = config.read(addr);                                          \
        addr = pc;                                                   \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        pc = static_cast<std::uint16_t>(                             \
            (base & 0x00FFu) |                                             \
            (static_cast<std::uint16_t>(config.read(addr)) << 8));         \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 2);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 2)

// ABS_WRITE(OPCODE, OP_CLASS) — 4 cycles. Like ABS_READ but the final step
// writes OP_CLASS::value(cpu) instead of reading.
#define TAWNY_ABS_WRITE(OPCODE, OP_CLASS)                                  \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        base = config.read(addr);                                          \
        addr = pc;                                                   \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        addr = static_cast<std::uint16_t>(                                 \
            (base & 0x00FFu) |                                             \
            (static_cast<std::uint16_t>(config.read(addr)) << 8));         \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 2);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        config.write(addr, OP_CLASS::value(view));                        \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 3);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 3)

// ZP_INDEXED_READ / ZP_INDEXED_WRITE — 4 cycles. Parameterised by index reg
// (X or Y). The indexed-zp address wraps within ZP ((base + idx) & 0xFF).
#define TAWNY_ZP_INDEXED_READ(OPCODE, OP_CLASS, IDX)                       \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        addr = config.read(addr);                                          \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 1);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        (void)config.read_zp(static_cast<std::uint8_t>(addr));             \
        addr = static_cast<std::uint16_t>(                                 \
            static_cast<std::uint8_t>(addr + (IDX)));                      \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 2);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        OP_CLASS::apply(view,                                             \
            config.read_zp(static_cast<std::uint8_t>(addr)));              \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 3);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 3)

#define TAWNY_ZP_INDEXED_WRITE(OPCODE, OP_CLASS, IDX)                      \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        addr = config.read(addr);                                          \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 1);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        (void)config.read_zp(static_cast<std::uint8_t>(addr));             \
        addr = static_cast<std::uint16_t>(                                 \
            static_cast<std::uint8_t>(addr + (IDX)));                      \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 2);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        config.write_zp(static_cast<std::uint8_t>(addr),                   \
                        OP_CLASS::value(view));                           \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 3);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 3)

#define TAWNY_ZPX_READ(OPCODE, OP_CLASS)  TAWNY_ZP_INDEXED_READ(OPCODE, OP_CLASS, x)
#define TAWNY_ZPY_READ(OPCODE, OP_CLASS)  TAWNY_ZP_INDEXED_READ(OPCODE, OP_CLASS, y)
#define TAWNY_ZPX_WRITE(OPCODE, OP_CLASS) TAWNY_ZP_INDEXED_WRITE(OPCODE, OP_CLASS, x)
#define TAWNY_ZPY_WRITE(OPCODE, OP_CLASS) TAWNY_ZP_INDEXED_WRITE(OPCODE, OP_CLASS, y)

// ABS_INDEXED_READ — 4 or 5 cycles. Optimistic path: if base_lo + IDX didn't
// carry, the "wrong" address with the un-carried high byte is actually
// correct, and we skip the fix-up cycle. If it did carry, step 2 does a
// dummy read at the wrong page, fixes the high byte, and step 3 does the
// real read.
#define TAWNY_AB_INDEXED_READ(OPCODE, OP_CLASS, IDX)                       \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        base = config.read(addr);                                          \
        addr = pc;                                                   \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        auto _hi     = config.read(addr);                                  \
        auto _lo_sum = static_cast<unsigned>((base & 0x00FFu) + (IDX));    \
        addr = static_cast<std::uint16_t>(                                 \
            (_hi << 8) | (_lo_sum & 0x00FFu));                             \
        if (_lo_sum > 0xFFu) {                                             \
            base = static_cast<std::uint16_t>(addr + 0x0100u);             \
            tst  = ((OPCODE) << 3) | 2;                                    \
        } else {                                                           \
            tst  = ((OPCODE) << 3) | 3;                                    \
        }                                                                  \
        TAWNY_STEP_TAIL(access_cost(addr), tst);                           \
        break;                                                             \
    }                                                                      \
    case ((OPCODE) << 3) | 2: {                                            \
        (void)config.read(addr);                                           \
        addr = base;                                                       \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 3);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 3: {                                            \
        OP_CLASS::apply(view, config.read(addr));                         \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 4);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 4)

// ABS_INDEXED_WRITE — always 5 cycles (no skip — penalty always paid).
#define TAWNY_AB_INDEXED_WRITE(OPCODE, OP_CLASS, IDX)                      \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        base = config.read(addr);                                          \
        addr = pc;                                                   \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        auto _hi     = config.read(addr);                                  \
        auto _lo_sum = static_cast<unsigned>((base & 0x00FFu) + (IDX));    \
        addr = static_cast<std::uint16_t>(                                 \
            (_hi << 8) | (_lo_sum & 0x00FFu));                             \
        /* stash correct addr for step 3 */                                \
        base = static_cast<std::uint16_t>(                                 \
            ((_hi << 8) + (_lo_sum & 0xFF00u)) |                           \
            (_lo_sum & 0x00FFu));                                          \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 2);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        (void)config.read(addr);                                           \
        addr = base;                                                       \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 3);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 3: {                                            \
        config.write(addr, OP_CLASS::value(view));                        \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 4);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 4)

#define TAWNY_ABX_READ(OPCODE, OP_CLASS)  TAWNY_AB_INDEXED_READ(OPCODE, OP_CLASS, x)
#define TAWNY_ABY_READ(OPCODE, OP_CLASS)  TAWNY_AB_INDEXED_READ(OPCODE, OP_CLASS, y)
#define TAWNY_ABX_WRITE(OPCODE, OP_CLASS) TAWNY_AB_INDEXED_WRITE(OPCODE, OP_CLASS, x)
#define TAWNY_ABY_WRITE(OPCODE, OP_CLASS) TAWNY_AB_INDEXED_WRITE(OPCODE, OP_CLASS, y)

// IZX (indirect,X) read/write — 6 cycles. ZP index with X, then fetch 2-byte
// pointer from ZP (wrapping in ZP), then read/write from target.
#define TAWNY_IZX_READ(OPCODE, OP_CLASS)                                   \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        auto _zp = config.read(addr);                                      \
        base = static_cast<std::uint16_t>(                                 \
            static_cast<std::uint8_t>(_zp + x));                     \
        addr = static_cast<std::uint16_t>(_zp);                            \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 1);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        (void)config.read_zp(static_cast<std::uint8_t>(addr));             \
        addr = base;                                                       \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 2);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        base = config.read_zp(static_cast<std::uint8_t>(addr));            \
        addr = static_cast<std::uint16_t>(                                 \
            static_cast<std::uint8_t>(addr + 1));                          \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 3);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 3: {                                            \
        addr = static_cast<std::uint16_t>(                                 \
            (base & 0x00FFu) |                                             \
            (static_cast<std::uint16_t>(                                   \
                config.read_zp(static_cast<std::uint8_t>(addr))) << 8));   \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 4: {                                            \
        OP_CLASS::apply(view, config.read(addr));                         \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

#define TAWNY_IZX_WRITE(OPCODE, OP_CLASS)                                  \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        auto _zp = config.read(addr);                                      \
        base = static_cast<std::uint16_t>(                                 \
            static_cast<std::uint8_t>(_zp + x));                     \
        addr = static_cast<std::uint16_t>(_zp);                            \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 1);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        (void)config.read_zp(static_cast<std::uint8_t>(addr));             \
        addr = base;                                                       \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 2);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        base = config.read_zp(static_cast<std::uint8_t>(addr));            \
        addr = static_cast<std::uint16_t>(                                 \
            static_cast<std::uint8_t>(addr + 1));                          \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 3);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 3: {                                            \
        addr = static_cast<std::uint16_t>(                                 \
            (base & 0x00FFu) |                                             \
            (static_cast<std::uint16_t>(                                   \
                config.read_zp(static_cast<std::uint8_t>(addr))) << 8));   \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 4: {                                            \
        config.write(addr, OP_CLASS::value(view));                        \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

// IZY (indirect),Y read/write — 5 or 6 cycles for reads (page-cross
// penalty); always 6 for writes.
#define TAWNY_IZY_READ(OPCODE, OP_CLASS)                                   \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        addr = static_cast<std::uint16_t>(config.read(addr));              \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 1);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        base = config.read_zp(static_cast<std::uint8_t>(addr));            \
        addr = static_cast<std::uint16_t>(                                 \
            static_cast<std::uint8_t>(addr + 1));                          \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 2);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        auto _hi     = config.read_zp(static_cast<std::uint8_t>(addr));    \
        auto _lo_sum = static_cast<unsigned>((base & 0x00FFu) + y);  \
        addr = static_cast<std::uint16_t>(                                 \
            (_hi << 8) | (_lo_sum & 0x00FFu));                             \
        if (_lo_sum > 0xFFu) {                                             \
            base = static_cast<std::uint16_t>(addr + 0x0100u);             \
            tst  = ((OPCODE) << 3) | 3;                                    \
        } else {                                                           \
            tst  = ((OPCODE) << 3) | 4;                                    \
        }                                                                  \
        TAWNY_STEP_TAIL(access_cost(addr), tst);                           \
        break;                                                             \
    }                                                                      \
    case ((OPCODE) << 3) | 3: {                                            \
        (void)config.read(addr);                                           \
        addr = base;                                                       \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 4: {                                            \
        OP_CLASS::apply(view, config.read(addr));                         \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

#define TAWNY_IZY_WRITE(OPCODE, OP_CLASS)                                  \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        addr = static_cast<std::uint16_t>(config.read(addr));              \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 1);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        base = config.read_zp(static_cast<std::uint8_t>(addr));            \
        addr = static_cast<std::uint16_t>(                                 \
            static_cast<std::uint8_t>(addr + 1));                          \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 2);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        auto _hi     = config.read_zp(static_cast<std::uint8_t>(addr));    \
        auto _lo_sum = static_cast<unsigned>((base & 0x00FFu) + y);  \
        addr = static_cast<std::uint16_t>(                                 \
            (_hi << 8) | (_lo_sum & 0x00FFu));                             \
        base = static_cast<std::uint16_t>(                                 \
            ((_hi << 8) + (_lo_sum & 0xFF00u)) |                           \
            (_lo_sum & 0x00FFu));                                          \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 3);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 3: {                                            \
        (void)config.read(addr);                                           \
        addr = base;                                                       \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 4: {                                            \
        config.write(addr, OP_CLASS::value(view));                        \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

// ACC_RMW(OPCODE, OP_CLASS) — ASL A / LSR A / ROL A / ROR A. 2 cycles:
// dummy operand read, apply op to A, fetch_opcode.
#define TAWNY_ACC_RMW(OPCODE, OP_CLASS)                                    \
    case ((OPCODE) << 3) | 0: {                                            \
        (void)config.read(addr);                                           \
        a = OP_CLASS::apply(view, a);                         \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 1);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 1)

// RMW addressing modes. Each does a read-modify-write: read original value,
// dummy-write it back, then write the transformed value. 6502 quirk.
//
// ZP_RMW: 5 cycles. ZPX_RMW: 6. ABS_RMW: 6. ABX_RMW: 7 (always).
#define TAWNY_ZP_RMW(OPCODE, OP_CLASS)                                     \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        addr = config.read(addr);                                          \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 1);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        base = config.read_zp(static_cast<std::uint8_t>(addr));            \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 2);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        config.write_zp(static_cast<std::uint8_t>(addr),                   \
                        static_cast<std::uint8_t>(base));                  \
        base = OP_CLASS::apply(view, static_cast<std::uint8_t>(base));    \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 3);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 3: {                                            \
        config.write_zp(static_cast<std::uint8_t>(addr),                   \
                        static_cast<std::uint8_t>(base));                  \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 4);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 4)

#define TAWNY_ZPX_RMW(OPCODE, OP_CLASS)                                    \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        addr = config.read(addr);                                          \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 1);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        (void)config.read_zp(static_cast<std::uint8_t>(addr));             \
        addr = static_cast<std::uint16_t>(                                 \
            static_cast<std::uint8_t>(addr + x));                    \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 2);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        base = config.read_zp(static_cast<std::uint8_t>(addr));            \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 3);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 3: {                                            \
        config.write_zp(static_cast<std::uint8_t>(addr),                   \
                        static_cast<std::uint8_t>(base));                  \
        base = OP_CLASS::apply(view, static_cast<std::uint8_t>(base));    \
        TAWNY_STEP_TAIL(access_cost_zp(static_cast<std::uint8_t>(addr)),   \
                        ((OPCODE) << 3) | 4);                              \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 4: {                                            \
        config.write_zp(static_cast<std::uint8_t>(addr),                   \
                        static_cast<std::uint8_t>(base));                  \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

#define TAWNY_ABS_RMW(OPCODE, OP_CLASS)                                    \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        base = config.read(addr);                                          \
        addr = pc;                                                   \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        addr = static_cast<std::uint16_t>(                                 \
            (base & 0x00FFu) |                                             \
            (static_cast<std::uint16_t>(config.read(addr)) << 8));         \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 2);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        base = config.read(addr);                                          \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 3);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 3: {                                            \
        config.write(addr, static_cast<std::uint8_t>(base));               \
        base = OP_CLASS::apply(view, static_cast<std::uint8_t>(base));    \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 4: {                                            \
        config.write(addr, static_cast<std::uint8_t>(base));               \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

#define TAWNY_ABX_RMW(OPCODE, OP_CLASS)                                    \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        base = config.read(addr);                                          \
        addr = pc;                                                   \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        auto _hi     = config.read(addr);                                  \
        auto _lo_sum = static_cast<unsigned>((base & 0x00FFu) + x);  \
        addr = static_cast<std::uint16_t>(                                 \
            (_hi << 8) | (_lo_sum & 0x00FFu));                             \
        base = static_cast<std::uint16_t>(                                 \
            ((_hi << 8) + (_lo_sum & 0xFF00u)) |                           \
            (_lo_sum & 0x00FFu));                                          \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 2);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        (void)config.read(addr);                                           \
        addr = base;                                                       \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 3);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 3: {                                            \
        base = config.read(addr);                                          \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 4: {                                            \
        config.write(addr, static_cast<std::uint8_t>(base));               \
        base = OP_CLASS::apply(view, static_cast<std::uint8_t>(base));    \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 5);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 5: {                                            \
        config.write(addr, static_cast<std::uint8_t>(base));               \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 6);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 6)

// JMP (indirect) — 5 cycles. The NMOS 6502 has a well-known page-wrap bug:
// when the pointer low byte is at $xxFF, the high byte is fetched from $xx00
// (same page) rather than $(xx+1)00. We replicate.
#define TAWNY_JMP_IND(OPCODE)                                              \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        base = config.read(addr);                                          \
        addr = pc;                                                   \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 1);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        addr = static_cast<std::uint16_t>(                                 \
            (base & 0x00FFu) |                                             \
            (static_cast<std::uint16_t>(config.read(addr)) << 8));         \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 2);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        base = config.read(addr);                                          \
        /* NMOS bug: +1 wraps within the same page. */                     \
        addr = static_cast<std::uint16_t>(                                 \
            (addr & 0xFF00u) |                                             \
            static_cast<std::uint8_t>((addr & 0xFFu) + 1));                \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 3);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 3: {                                            \
        pc = static_cast<std::uint16_t>(                             \
            (base & 0x00FFu) |                                             \
            (static_cast<std::uint16_t>(config.read(addr)) << 8));         \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 4);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 4)

// JSR — 6 cycles. Reads addr lo, dummies a stack read, pushes PCH, pushes
// PCL, reads addr hi (now forming the jump target), then fetch_opcode.
#define TAWNY_JSR(OPCODE)                                                  \
    case ((OPCODE) << 3) | 0: {                                            \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        base = config.read(addr);                                          \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 1);                             \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        (void)config.read_stack(s);                                  \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 2);                             \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        config.write_stack(s, static_cast<std::uint8_t>(pc >> 8)); \
        s = static_cast<std::uint8_t>(s - 1);                  \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 3);                             \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 3: {                                            \
        config.write_stack(s, static_cast<std::uint8_t>(pc));  \
        s = static_cast<std::uint8_t>(s - 1);                  \
        addr = pc;                                                   \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 4: {                                            \
        pc = static_cast<std::uint16_t>(                             \
            (base & 0x00FFu) |                                             \
            (static_cast<std::uint16_t>(config.read(addr)) << 8));         \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

// RTS — 6 cycles. Dummy operand read, dummy stack read, pull PCL, pull PCH
// (forming PC), dummy read at PC (pc++ afterwards), fetch_opcode.
#define TAWNY_RTS(OPCODE)                                                  \
    case ((OPCODE) << 3) | 0: {                                            \
        (void)config.read(addr);                                           \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 1);                             \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        (void)config.read_stack(s);                                  \
        s = static_cast<std::uint8_t>(s + 1);                  \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 2);                             \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        base = config.read_stack(s);                                 \
        s = static_cast<std::uint8_t>(s + 1);                  \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 3);                             \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 3: {                                            \
        pc = static_cast<std::uint16_t>(                             \
            (base & 0x00FFu) |                                             \
            (static_cast<std::uint16_t>(config.read_stack(s)) << 8));\
        addr = pc;                                                   \
        TAWNY_STEP_TAIL(access_cost(addr), ((OPCODE) << 3) | 4);           \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 4: {                                            \
        (void)config.read(addr);                                           \
        pc = static_cast<std::uint16_t>(pc + 1);               \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

// RTI — 6 cycles. Dummy operand read, dummy stack read, pull P, pull PCL,
// pull PCH (forming PC), fetch_opcode.
#define TAWNY_RTI(OPCODE)                                                  \
    case ((OPCODE) << 3) | 0: {                                            \
        (void)config.read(addr);                                           \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 1);                             \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        (void)config.read_stack(s);                                  \
        s = static_cast<std::uint8_t>(s + 1);                  \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 2);                             \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        p = static_cast<std::uint8_t>(                               \
            (config.read_stack(s) & ~flag::B) | flag::U);            \
        s = static_cast<std::uint8_t>(s + 1);                  \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 3);                             \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 3: {                                            \
        base = config.read_stack(s);                                 \
        s = static_cast<std::uint8_t>(s + 1);                  \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 4);                             \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 4: {                                            \
        pc = static_cast<std::uint16_t>(                             \
            (base & 0x00FFu) |                                             \
            (static_cast<std::uint16_t>(config.read_stack(s)) << 8));\
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 5);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 5)

// PHA / PHP — 3 cycles. Dummy operand read, push OP_CLASS::value(cpu),
// fetch_opcode.
#define TAWNY_PUSH(OPCODE, OP_CLASS)                                       \
    case ((OPCODE) << 3) | 0: {                                            \
        (void)config.read(addr);                                           \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 1);                             \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        config.write_stack(s, OP_CLASS::value(view));               \
        s = static_cast<std::uint8_t>(s - 1);                  \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 2);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 2)

// PLA / PLP — 4 cycles. Dummy operand read, dummy stack read (pre-increment),
// pull byte and apply to CPU via OP_CLASS::apply(cpu, pulled), fetch_opcode.
#define TAWNY_PULL(OPCODE, OP_CLASS)                                       \
    case ((OPCODE) << 3) | 0: {                                            \
        (void)config.read(addr);                                           \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 1);                             \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 1: {                                            \
        (void)config.read_stack(s);                                  \
        s = static_cast<std::uint8_t>(s + 1);                  \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 2);                             \
    }                                                                      \
    [[fallthrough]];                                                       \
    case ((OPCODE) << 3) | 2: {                                            \
        OP_CLASS::apply(view, config.read_stack(s));                \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 3);                      \
    }                                                                      \
    [[fallthrough]];                                                       \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 3)

// JAM — halts the CPU on an illegal opcode by redirecting the next phi2 back
// to the same opcode, which the Dormann trap detection catches. Simple stub.
#define TAWNY_JAM(OPCODE)                                                  \
    case ((OPCODE) << 3) | 0: {                                            \
        /* Leave addr pointing at the JAM opcode — next opcode fetch will  \
           hit the same address, triggering the trap or at least spinning. */ \
        pc = static_cast<std::uint16_t>(pc - 1);               \
        addr = pc;                                                   \
        TAWNY_STEP_TAIL(access_cost_opcode(addr), ((OPCODE) << 3) | 0);    \
        break;                                                             \
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
#define TAWNY_BRK(OPCODE)                                                     \
    case ((OPCODE) << 3) | 0: {                                               \
        (void)config.read(addr);                                              \
        if (brk_flags == BrkFlags::None) {                                    \
            pc = static_cast<std::uint16_t>(pc + 1);              \
        }                                                                     \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 1);                                \
    }                                                                         \
    [[fallthrough]];                                                          \
    case ((OPCODE) << 3) | 1: {                                               \
        if (brk_flags == BrkFlags::Reset) {                                   \
            (void)config.read_stack(s);                                 \
        } else {                                                              \
            config.write_stack(s, static_cast<std::uint8_t>(pc >> 8)); \
        }                                                                     \
        s = static_cast<std::uint8_t>(s - 1);                     \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 2);                                \
    }                                                                         \
    [[fallthrough]];                                                          \
    case ((OPCODE) << 3) | 2: {                                               \
        if (brk_flags == BrkFlags::Reset) {                                   \
            (void)config.read_stack(s);                                 \
        } else {                                                              \
            config.write_stack(s, static_cast<std::uint8_t>(pc)); \
        }                                                                     \
        s = static_cast<std::uint8_t>(s - 1);                     \
        TAWNY_NEXT_STACK(((OPCODE) << 3) | 3);                                \
    }                                                                         \
    [[fallthrough]];                                                          \
    case ((OPCODE) << 3) | 3: {                                               \
        if (brk_flags == BrkFlags::Reset) {                                   \
            (void)config.read_stack(s);                                 \
        } else {                                                              \
            std::uint8_t _pushed_p = (brk_flags == BrkFlags::None)            \
                ? static_cast<std::uint8_t>(p | flag::B | flag::U)      \
                : static_cast<std::uint8_t>(p | flag::U);               \
            config.write_stack(s, _pushed_p);                           \
        }                                                                     \
        s = static_cast<std::uint8_t>(s - 1);                     \
        p = static_cast<std::uint8_t>(p | flag::I);               \
        addr = (brk_flags == BrkFlags::Nmi)   ? 0xFFFAu                       \
             : (brk_flags == BrkFlags::Reset) ? 0xFFFCu                       \
             :                                  0xFFFEu;                      \
        TAWNY_STEP_TAIL(access_cost_vector(addr), ((OPCODE) << 3) | 4);       \
    }                                                                         \
    [[fallthrough]];                                                          \
    case ((OPCODE) << 3) | 4: {                                               \
        base = config.read_vector(addr);                                      \
        addr = static_cast<std::uint16_t>(addr + 1);                          \
        TAWNY_STEP_TAIL(access_cost_vector(addr), ((OPCODE) << 3) | 5);       \
    }                                                                         \
    [[fallthrough]];                                                          \
    case ((OPCODE) << 3) | 5: {                                               \
        pc = static_cast<std::uint16_t>(                                \
            (base & 0x00FFu) |                                                \
            (static_cast<std::uint16_t>(config.read_vector(addr)) << 8));     \
        brk_flags = BrkFlags::None;                                     \
        TAWNY_NEXT_OPCODE_FETCH(((OPCODE) << 3) | 6);                         \
    }                                                                         \
    [[fallthrough]];                                                          \
    TAWNY_FETCH_OPCODE_CASE(OPCODE, 6)

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
        pc = static_cast<std::uint16_t>(pc + 1);               \
        auto _off = static_cast<std::int8_t>(config.read(addr));           \
        if (!COND::taken(view)) {                                         \
            tst = ((OPCODE) << 3) | 3;                                     \
            TAWNY_NEXT_OPCODE_FETCH(tst);                                  \
            break;                                                         \
        }                                                                  \
        auto _tgt = static_cast<std::uint16_t>(                            \
            pc + static_cast<std::int16_t>(_off));                   \
        auto _wrong = static_cast<std::uint16_t>(                          \
            (pc & 0xFF00u) | (_tgt & 0x00FFu));                      \
        if (_wrong == _tgt) {                                              \
            pc = _tgt;                                               \
            addr = pc;                                               \
            tst  = ((OPCODE) << 3) | 1;                                    \
            TAWNY_STEP_TAIL(access_cost(addr), tst);                       \
            break;                                                         \
        }                                                                  \
        pc = _wrong;                                                 \
        base     = _tgt;                                                   \
        addr     = pc;                                               \
        tst      = ((OPCODE) << 3) | 2;                                    \
        TAWNY_STEP_TAIL(access_cost(addr), tst);                           \
        break;                                                             \
    }                                                                      \
    case ((OPCODE) << 3) | 1: {                                            \
        (void)config.read(addr);                                           \
        tst = ((OPCODE) << 3) | 3;                                         \
        TAWNY_NEXT_OPCODE_FETCH(tst);                                      \
        break;                                                             \
    }                                                                      \
    case ((OPCODE) << 3) | 2: {                                            \
        (void)config.read(addr);                                           \
        pc = base;                                                   \
        tst = ((OPCODE) << 3) | 3;                                         \
        TAWNY_NEXT_OPCODE_FETCH(tst);                                      \
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
        auto a       = this->a;
        auto x       = this->x;
        auto y       = this->y;
        auto s       = this->s;
        auto p       = this->p;
        detail::RegView view{a, x, y, s, p};  // refs to the locals; passed to op classes

        while (current < horizon) {
            switch (tst) {
                // Full NMOS 6502 opcode table, laid out in opcode order. * marks
                // illegal-but-stable ops; ! marks illegal-unstable (stubbed as
                // JAM here — implementation would need CPU-specific magic
                // constants). The stable-illegal RMW ops (SLO/RLA/SRE/RRA/DCP/ISC)
                // are implemented only in their zp/zpx/abs/abx forms for now;
                // the aby/izx/izy variants stub as JAM pending ABY_RMW/IZX_RMW/
                // IZY_RMW macros.

                // --- 0x00-0x0F ---
                TAWNY_BRK       (0x00)                       // BRK
                TAWNY_IZX_READ  (0x01, detail::Ora)          // ORA (zp,X)
                TAWNY_JAM       (0x02)                       // JAM*
                TAWNY_JAM       (0x03)                       // SLO (zp,X)* — TODO izx RMW
                TAWNY_ZP_READ   (0x04, detail::Nop)          // NOP zp*
                TAWNY_ZP_READ   (0x05, detail::Ora)          // ORA zp
                TAWNY_ZP_RMW    (0x06, detail::Asl)          // ASL zp
                TAWNY_ZP_RMW    (0x07, detail::Slo)          // SLO zp*
                TAWNY_PUSH      (0x08, detail::Php)           // PHP
                TAWNY_IMM_READ  (0x09, detail::Ora)          // ORA #
                TAWNY_ACC_RMW   (0x0A, detail::Asl)          // ASL A
                TAWNY_IMM_READ  (0x0B, detail::Anc)          // ANC #*
                TAWNY_ABS_READ  (0x0C, detail::Nop)          // NOP abs*
                TAWNY_ABS_READ  (0x0D, detail::Ora)          // ORA abs
                TAWNY_ABS_RMW   (0x0E, detail::Asl)          // ASL abs
                TAWNY_ABS_RMW   (0x0F, detail::Slo)          // SLO abs*

                // --- 0x10-0x1F ---
                TAWNY_REL_BRANCH(0x10, detail::BplCond)      // BPL
                TAWNY_IZY_READ  (0x11, detail::Ora)          // ORA (zp),Y
                TAWNY_JAM       (0x12)                       // JAM*
                TAWNY_JAM       (0x13)                       // SLO (zp),Y* — TODO izy RMW
                TAWNY_ZPX_READ  (0x14, detail::Nop)          // NOP zp,X*
                TAWNY_ZPX_READ  (0x15, detail::Ora)          // ORA zp,X
                TAWNY_ZPX_RMW   (0x16, detail::Asl)          // ASL zp,X
                TAWNY_ZPX_RMW   (0x17, detail::Slo)          // SLO zp,X*
                TAWNY_IMPLIED   (0x18, detail::Clc)          // CLC
                TAWNY_ABY_READ  (0x19, detail::Ora)          // ORA abs,Y
                TAWNY_IMPLIED   (0x1A, detail::Nop)          // NOP*
                TAWNY_JAM       (0x1B)                       // SLO abs,Y* — TODO aby RMW
                TAWNY_ABX_READ  (0x1C, detail::Nop)          // NOP abs,X*
                TAWNY_ABX_READ  (0x1D, detail::Ora)          // ORA abs,X
                TAWNY_ABX_RMW   (0x1E, detail::Asl)          // ASL abs,X
                TAWNY_ABX_RMW   (0x1F, detail::Slo)          // SLO abs,X*

                // --- 0x20-0x2F ---
                TAWNY_JSR       (0x20)                       // JSR abs
                TAWNY_IZX_READ  (0x21, detail::And)          // AND (zp,X)
                TAWNY_JAM       (0x22)                       // JAM*
                TAWNY_JAM       (0x23)                       // RLA (zp,X)* — TODO izx RMW
                TAWNY_ZP_READ   (0x24, detail::Bit)          // BIT zp
                TAWNY_ZP_READ   (0x25, detail::And)          // AND zp
                TAWNY_ZP_RMW    (0x26, detail::Rol)          // ROL zp
                TAWNY_ZP_RMW    (0x27, detail::Rla)          // RLA zp*
                TAWNY_PULL      (0x28, detail::Plp)           // PLP
                TAWNY_IMM_READ  (0x29, detail::And)          // AND #
                TAWNY_ACC_RMW   (0x2A, detail::Rol)          // ROL A
                TAWNY_IMM_READ  (0x2B, detail::Anc)          // ANC #*
                TAWNY_ABS_READ  (0x2C, detail::Bit)          // BIT abs
                TAWNY_ABS_READ  (0x2D, detail::And)          // AND abs
                TAWNY_ABS_RMW   (0x2E, detail::Rol)          // ROL abs
                TAWNY_ABS_RMW   (0x2F, detail::Rla)          // RLA abs*

                // --- 0x30-0x3F ---
                TAWNY_REL_BRANCH(0x30, detail::BmiCond)      // BMI
                TAWNY_IZY_READ  (0x31, detail::And)          // AND (zp),Y
                TAWNY_JAM       (0x32)                       // JAM*
                TAWNY_JAM       (0x33)                       // RLA (zp),Y* — TODO
                TAWNY_ZPX_READ  (0x34, detail::Nop)          // NOP zp,X*
                TAWNY_ZPX_READ  (0x35, detail::And)          // AND zp,X
                TAWNY_ZPX_RMW   (0x36, detail::Rol)          // ROL zp,X
                TAWNY_ZPX_RMW   (0x37, detail::Rla)          // RLA zp,X*
                TAWNY_IMPLIED   (0x38, detail::Sec)          // SEC
                TAWNY_ABY_READ  (0x39, detail::And)          // AND abs,Y
                TAWNY_IMPLIED   (0x3A, detail::Nop)          // NOP*
                TAWNY_JAM       (0x3B)                       // RLA abs,Y*
                TAWNY_ABX_READ  (0x3C, detail::Nop)          // NOP abs,X*
                TAWNY_ABX_READ  (0x3D, detail::And)          // AND abs,X
                TAWNY_ABX_RMW   (0x3E, detail::Rol)          // ROL abs,X
                TAWNY_ABX_RMW   (0x3F, detail::Rla)          // RLA abs,X*

                // --- 0x40-0x4F ---
                TAWNY_RTI       (0x40)                       // RTI
                TAWNY_IZX_READ  (0x41, detail::Eor)          // EOR (zp,X)
                TAWNY_JAM       (0x42)                       // JAM*
                TAWNY_JAM       (0x43)                       // SRE (zp,X)*
                TAWNY_ZP_READ   (0x44, detail::Nop)          // NOP zp*
                TAWNY_ZP_READ   (0x45, detail::Eor)          // EOR zp
                TAWNY_ZP_RMW    (0x46, detail::Lsr)          // LSR zp
                TAWNY_ZP_RMW    (0x47, detail::Sre)          // SRE zp*
                TAWNY_PUSH      (0x48, detail::Pha)           // PHA
                TAWNY_IMM_READ  (0x49, detail::Eor)          // EOR #
                TAWNY_ACC_RMW   (0x4A, detail::Lsr)          // LSR A
                TAWNY_IMM_READ  (0x4B, detail::Alr)          // ALR #*
                TAWNY_ABS_JUMP  (0x4C)                       // JMP abs
                TAWNY_ABS_READ  (0x4D, detail::Eor)          // EOR abs
                TAWNY_ABS_RMW   (0x4E, detail::Lsr)          // LSR abs
                TAWNY_ABS_RMW   (0x4F, detail::Sre)          // SRE abs*

                // --- 0x50-0x5F ---
                TAWNY_REL_BRANCH(0x50, detail::BvcCond)      // BVC
                TAWNY_IZY_READ  (0x51, detail::Eor)          // EOR (zp),Y
                TAWNY_JAM       (0x52)                       // JAM*
                TAWNY_JAM       (0x53)                       // SRE (zp),Y*
                TAWNY_ZPX_READ  (0x54, detail::Nop)          // NOP zp,X*
                TAWNY_ZPX_READ  (0x55, detail::Eor)          // EOR zp,X
                TAWNY_ZPX_RMW   (0x56, detail::Lsr)          // LSR zp,X
                TAWNY_ZPX_RMW   (0x57, detail::Sre)          // SRE zp,X*
                TAWNY_IMPLIED   (0x58, detail::Cli)          // CLI
                TAWNY_ABY_READ  (0x59, detail::Eor)          // EOR abs,Y
                TAWNY_IMPLIED   (0x5A, detail::Nop)          // NOP*
                TAWNY_JAM       (0x5B)                       // SRE abs,Y*
                TAWNY_ABX_READ  (0x5C, detail::Nop)          // NOP abs,X*
                TAWNY_ABX_READ  (0x5D, detail::Eor)          // EOR abs,X
                TAWNY_ABX_RMW   (0x5E, detail::Lsr)          // LSR abs,X
                TAWNY_ABX_RMW   (0x5F, detail::Sre)          // SRE abs,X*

                // --- 0x60-0x6F ---
                TAWNY_RTS       (0x60)                       // RTS
                TAWNY_IZX_READ  (0x61, detail::Adc)       // ADC (zp,X)
                TAWNY_JAM       (0x62)                       // JAM*
                TAWNY_JAM       (0x63)                       // RRA (zp,X)*
                TAWNY_ZP_READ   (0x64, detail::Nop)          // NOP zp*
                TAWNY_ZP_READ   (0x65, detail::Adc)       // ADC zp
                TAWNY_ZP_RMW    (0x66, detail::Ror)          // ROR zp
                TAWNY_ZP_RMW    (0x67, detail::Rra)          // RRA zp*
                TAWNY_PULL      (0x68, detail::Pla)           // PLA
                TAWNY_IMM_READ  (0x69, detail::Adc)       // ADC #
                TAWNY_ACC_RMW   (0x6A, detail::Ror)          // ROR A
                TAWNY_IMM_READ  (0x6B, detail::Arr)          // ARR #*
                TAWNY_JMP_IND   (0x6C)                       // JMP (ind)
                TAWNY_ABS_READ  (0x6D, detail::Adc)       // ADC abs
                TAWNY_ABS_RMW   (0x6E, detail::Ror)          // ROR abs
                TAWNY_ABS_RMW   (0x6F, detail::Rra)          // RRA abs*

                // --- 0x70-0x7F ---
                TAWNY_REL_BRANCH(0x70, detail::BvsCond)      // BVS
                TAWNY_IZY_READ  (0x71, detail::Adc)       // ADC (zp),Y
                TAWNY_JAM       (0x72)                       // JAM*
                TAWNY_JAM       (0x73)                       // RRA (zp),Y*
                TAWNY_ZPX_READ  (0x74, detail::Nop)          // NOP zp,X*
                TAWNY_ZPX_READ  (0x75, detail::Adc)       // ADC zp,X
                TAWNY_ZPX_RMW   (0x76, detail::Ror)          // ROR zp,X
                TAWNY_ZPX_RMW   (0x77, detail::Rra)          // RRA zp,X*
                TAWNY_IMPLIED   (0x78, detail::Sei)          // SEI
                TAWNY_ABY_READ  (0x79, detail::Adc)       // ADC abs,Y
                TAWNY_IMPLIED   (0x7A, detail::Nop)          // NOP*
                TAWNY_JAM       (0x7B)                       // RRA abs,Y*
                TAWNY_ABX_READ  (0x7C, detail::Nop)          // NOP abs,X*
                TAWNY_ABX_READ  (0x7D, detail::Adc)       // ADC abs,X
                TAWNY_ABX_RMW   (0x7E, detail::Ror)          // ROR abs,X
                TAWNY_ABX_RMW   (0x7F, detail::Rra)          // RRA abs,X*

                // --- 0x80-0x8F ---
                TAWNY_IMM_READ  (0x80, detail::Nop)          // NOP #*
                TAWNY_IZX_WRITE (0x81, detail::Sta)          // STA (zp,X)
                TAWNY_IMM_READ  (0x82, detail::Nop)          // NOP #*
                TAWNY_IZX_WRITE (0x83, detail::Sax)          // SAX (zp,X)*
                TAWNY_ZP_WRITE  (0x84, detail::Sty)          // STY zp
                TAWNY_ZP_WRITE  (0x85, detail::Sta)          // STA zp
                TAWNY_ZP_WRITE  (0x86, detail::Stx)          // STX zp
                TAWNY_ZP_WRITE  (0x87, detail::Sax)          // SAX zp*
                TAWNY_IMPLIED   (0x88, detail::Dey)          // DEY
                TAWNY_IMM_READ  (0x89, detail::Nop)          // NOP #*
                TAWNY_IMPLIED   (0x8A, detail::Txa)          // TXA
                TAWNY_JAM       (0x8B)                       // ANE #!
                TAWNY_ABS_WRITE (0x8C, detail::Sty)          // STY abs
                TAWNY_ABS_WRITE (0x8D, detail::Sta)          // STA abs
                TAWNY_ABS_WRITE (0x8E, detail::Stx)          // STX abs
                TAWNY_ABS_WRITE (0x8F, detail::Sax)          // SAX abs*

                // --- 0x90-0x9F ---
                TAWNY_REL_BRANCH(0x90, detail::BccCond)      // BCC
                TAWNY_IZY_WRITE (0x91, detail::Sta)          // STA (zp),Y
                TAWNY_JAM       (0x92)                       // JAM*
                TAWNY_JAM       (0x93)                       // SHA (zp),Y!
                TAWNY_ZPX_WRITE (0x94, detail::Sty)          // STY zp,X
                TAWNY_ZPX_WRITE (0x95, detail::Sta)          // STA zp,X
                TAWNY_ZPY_WRITE (0x96, detail::Stx)          // STX zp,Y
                TAWNY_ZPY_WRITE (0x97, detail::Sax)          // SAX zp,Y*
                TAWNY_IMPLIED   (0x98, detail::Tya)          // TYA
                TAWNY_ABY_WRITE (0x99, detail::Sta)          // STA abs,Y
                TAWNY_IMPLIED   (0x9A, detail::Txs)          // TXS
                TAWNY_JAM       (0x9B)                       // TAS abs,Y!
                TAWNY_JAM       (0x9C)                       // SHY abs,X!
                TAWNY_ABX_WRITE (0x9D, detail::Sta)          // STA abs,X
                TAWNY_JAM       (0x9E)                       // SHX abs,Y!
                TAWNY_JAM       (0x9F)                       // SHA abs,Y!

                // --- 0xA0-0xAF ---
                TAWNY_IMM_READ  (0xA0, detail::Ldy)          // LDY #
                TAWNY_IZX_READ  (0xA1, detail::Lda)          // LDA (zp,X)
                TAWNY_IMM_READ  (0xA2, detail::Ldx)          // LDX #
                TAWNY_IZX_READ  (0xA3, detail::Lax)          // LAX (zp,X)*
                TAWNY_ZP_READ   (0xA4, detail::Ldy)          // LDY zp
                TAWNY_ZP_READ   (0xA5, detail::Lda)          // LDA zp
                TAWNY_ZP_READ   (0xA6, detail::Ldx)          // LDX zp
                TAWNY_ZP_READ   (0xA7, detail::Lax)          // LAX zp*
                TAWNY_IMPLIED   (0xA8, detail::Tay)          // TAY
                TAWNY_IMM_READ  (0xA9, detail::Lda)          // LDA #
                TAWNY_IMPLIED   (0xAA, detail::Tax)          // TAX
                TAWNY_JAM       (0xAB)                       // LXA #!
                TAWNY_ABS_READ  (0xAC, detail::Ldy)          // LDY abs
                TAWNY_ABS_READ  (0xAD, detail::Lda)          // LDA abs
                TAWNY_ABS_READ  (0xAE, detail::Ldx)          // LDX abs
                TAWNY_ABS_READ  (0xAF, detail::Lax)          // LAX abs*

                // --- 0xB0-0xBF ---
                TAWNY_REL_BRANCH(0xB0, detail::BcsCond)      // BCS
                TAWNY_IZY_READ  (0xB1, detail::Lda)          // LDA (zp),Y
                TAWNY_JAM       (0xB2)                       // JAM*
                TAWNY_IZY_READ  (0xB3, detail::Lax)          // LAX (zp),Y*
                TAWNY_ZPX_READ  (0xB4, detail::Ldy)          // LDY zp,X
                TAWNY_ZPX_READ  (0xB5, detail::Lda)          // LDA zp,X
                TAWNY_ZPY_READ  (0xB6, detail::Ldx)          // LDX zp,Y
                TAWNY_ZPY_READ  (0xB7, detail::Lax)          // LAX zp,Y*
                TAWNY_IMPLIED   (0xB8, detail::Clv)          // CLV
                TAWNY_ABY_READ  (0xB9, detail::Lda)          // LDA abs,Y
                TAWNY_IMPLIED   (0xBA, detail::Tsx)          // TSX
                TAWNY_JAM       (0xBB)                       // LAS abs,Y!
                TAWNY_ABX_READ  (0xBC, detail::Ldy)          // LDY abs,X
                TAWNY_ABX_READ  (0xBD, detail::Lda)          // LDA abs,X
                TAWNY_ABY_READ  (0xBE, detail::Ldx)          // LDX abs,Y
                TAWNY_ABY_READ  (0xBF, detail::Lax)          // LAX abs,Y*

                // --- 0xC0-0xCF ---
                TAWNY_IMM_READ  (0xC0, detail::Cpy)          // CPY #
                TAWNY_IZX_READ  (0xC1, detail::Cmp)          // CMP (zp,X)
                TAWNY_IMM_READ  (0xC2, detail::Nop)          // NOP #*
                TAWNY_JAM       (0xC3)                       // DCP (zp,X)* — TODO
                TAWNY_ZP_READ   (0xC4, detail::Cpy)          // CPY zp
                TAWNY_ZP_READ   (0xC5, detail::Cmp)          // CMP zp
                TAWNY_ZP_RMW    (0xC6, detail::Dec)          // DEC zp
                TAWNY_ZP_RMW    (0xC7, detail::Dcp)          // DCP zp*
                TAWNY_IMPLIED   (0xC8, detail::Iny)          // INY
                TAWNY_IMM_READ  (0xC9, detail::Cmp)          // CMP #
                TAWNY_IMPLIED   (0xCA, detail::Dex)          // DEX
                TAWNY_IMM_READ  (0xCB, detail::Axs)          // AXS #*
                TAWNY_ABS_READ  (0xCC, detail::Cpy)          // CPY abs
                TAWNY_ABS_READ  (0xCD, detail::Cmp)          // CMP abs
                TAWNY_ABS_RMW   (0xCE, detail::Dec)          // DEC abs
                TAWNY_ABS_RMW   (0xCF, detail::Dcp)          // DCP abs*

                // --- 0xD0-0xDF ---
                TAWNY_REL_BRANCH(0xD0, detail::BneCond)      // BNE
                TAWNY_IZY_READ  (0xD1, detail::Cmp)          // CMP (zp),Y
                TAWNY_JAM       (0xD2)                       // JAM*
                TAWNY_JAM       (0xD3)                       // DCP (zp),Y*
                TAWNY_ZPX_READ  (0xD4, detail::Nop)          // NOP zp,X*
                TAWNY_ZPX_READ  (0xD5, detail::Cmp)          // CMP zp,X
                TAWNY_ZPX_RMW   (0xD6, detail::Dec)          // DEC zp,X
                TAWNY_ZPX_RMW   (0xD7, detail::Dcp)          // DCP zp,X*
                TAWNY_IMPLIED   (0xD8, detail::Cld)          // CLD
                TAWNY_ABY_READ  (0xD9, detail::Cmp)          // CMP abs,Y
                TAWNY_IMPLIED   (0xDA, detail::Nop)          // NOP*
                TAWNY_JAM       (0xDB)                       // DCP abs,Y*
                TAWNY_ABX_READ  (0xDC, detail::Nop)          // NOP abs,X*
                TAWNY_ABX_READ  (0xDD, detail::Cmp)          // CMP abs,X
                TAWNY_ABX_RMW   (0xDE, detail::Dec)          // DEC abs,X
                TAWNY_ABX_RMW   (0xDF, detail::Dcp)          // DCP abs,X*

                // --- 0xE0-0xEF ---
                TAWNY_IMM_READ  (0xE0, detail::Cpx)          // CPX #
                TAWNY_IZX_READ  (0xE1, detail::Sbc)       // SBC (zp,X)
                TAWNY_IMM_READ  (0xE2, detail::Nop)          // NOP #*
                TAWNY_JAM       (0xE3)                       // ISC (zp,X)*
                TAWNY_ZP_READ   (0xE4, detail::Cpx)          // CPX zp
                TAWNY_ZP_READ   (0xE5, detail::Sbc)       // SBC zp
                TAWNY_ZP_RMW    (0xE6, detail::Inc)          // INC zp
                TAWNY_ZP_RMW    (0xE7, detail::Isc)          // ISC zp*
                TAWNY_IMPLIED   (0xE8, detail::Inx)          // INX
                TAWNY_IMM_READ  (0xE9, detail::Sbc)       // SBC #
                TAWNY_IMPLIED   (0xEA, detail::Nop)          // NOP
                TAWNY_IMM_READ  (0xEB, detail::Usbc)         // USBC #*
                TAWNY_ABS_READ  (0xEC, detail::Cpx)          // CPX abs
                TAWNY_ABS_READ  (0xED, detail::Sbc)       // SBC abs
                TAWNY_ABS_RMW   (0xEE, detail::Inc)          // INC abs
                TAWNY_ABS_RMW   (0xEF, detail::Isc)          // ISC abs*

                // --- 0xF0-0xFF ---
                TAWNY_REL_BRANCH(0xF0, detail::BeqCond)      // BEQ
                TAWNY_IZY_READ  (0xF1, detail::Sbc)       // SBC (zp),Y
                TAWNY_JAM       (0xF2)                       // JAM*
                TAWNY_JAM       (0xF3)                       // ISC (zp),Y*
                TAWNY_ZPX_READ  (0xF4, detail::Nop)          // NOP zp,X*
                TAWNY_ZPX_READ  (0xF5, detail::Sbc)       // SBC zp,X
                TAWNY_ZPX_RMW   (0xF6, detail::Inc)          // INC zp,X
                TAWNY_ZPX_RMW   (0xF7, detail::Isc)          // ISC zp,X*
                TAWNY_IMPLIED   (0xF8, detail::Sed)          // SED
                TAWNY_ABY_READ  (0xF9, detail::Sbc)       // SBC abs,Y
                TAWNY_IMPLIED   (0xFA, detail::Nop)          // NOP*
                TAWNY_JAM       (0xFB)                       // ISC abs,Y*
                TAWNY_ABX_READ  (0xFC, detail::Nop)          // NOP abs,X*
                TAWNY_ABX_READ  (0xFD, detail::Sbc)       // SBC abs,X
                TAWNY_ABX_RMW   (0xFE, detail::Inc)          // INC abs,X
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
        this->pc     = pc;       // disambiguate: left-hand is the struct member
        this->a      = a;
        this->x      = x;
        this->y      = y;
        this->s      = s;
        this->p      = p;
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
#undef TAWNY_ABX_RMW
#undef TAWNY_JMP_IND
#undef TAWNY_JSR
#undef TAWNY_RTS
#undef TAWNY_RTI
#undef TAWNY_PUSH
#undef TAWNY_PULL
#undef TAWNY_JAM

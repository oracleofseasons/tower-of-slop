// GML value semantics, ported from OpenGMK gm8emulator/src/gml/value.rs and math.rs.
#include <cstdio>

#include "gml_runtime.h"

namespace gml {

double gm_round(double v) {
    double i = std::floor(v);
    double diff = v - i;
    if (diff < 0.5) return i;
    if (diff > 0.5) return i + 1.0;
    return i + static_cast<double>(static_cast<int64_t>(i) & 1);
}

static int to_i32(double v) { return static_cast<int32_t>(static_cast<int64_t>(v)); }
static uint64_t to_u64(double v) { return v <= 0 ? 0 : static_cast<uint64_t>(v); }

int Value::i32() const { return is_str ? 0 : to_i32(gm_round(r)); }

std::string Value::repr() const {
    if (is_str) return s;
    char buf[64];
    if (r == std::trunc(r)) std::snprintf(buf, sizeof buf, "%.0f", r);
    else std::snprintf(buf, sizeof buf, "%.2f", r);
    return buf;
}

bool truthy(const Value& v) { return !v.is_str && v.r >= 0.5; }

int repeat_count(const Value& v) { return v.i32(); }

int idx_key(const Idx& idx) {
    return to_i32(gm_round(idx.i.real())) * 32000 + to_i32(gm_round(idx.j.real()));
}

[[noreturn]] static void bad(const char* op, const Value& a, const Value& b) {
    throw GmlError{std::string("invalid operands for ") + op + ": " + (a.is_str ? "string" : "real") + ", " +
                   (b.is_str ? "string" : "real")};
}

#define REAL_OP(name, sym, expr)                                   \
    Value op_##name(Pair p) {                                      \
        if (p.a.is_str || p.b.is_str) bad(sym, p.a, p.b);          \
        double l = p.a.r, r = p.b.r;                               \
        return Value(expr);                                        \
    }

Value op_add(Pair p) {
    if (!p.a.is_str && !p.b.is_str) return Value(p.a.r + p.b.r);
    if (p.a.is_str && p.b.is_str) return Value(p.a.s + p.b.s);
    bad("+", p.a, p.b);
}

Value op_mul(Pair p) {
    if (!p.a.is_str && !p.b.is_str) return Value(p.a.r * p.b.r);
    if (!p.a.is_str && p.b.is_str) {
        int n = to_i32(gm_round(p.a.r));
        std::string out;
        for (int i = 0; i < n; i++) out += p.b.s;
        return Value(out);
    }
    bad("*", p.a, p.b);
}

REAL_OP(sub, "-", l - r)
REAL_OP(div, "/", l / r)
REAL_OP(idiv, "div", std::floor(l / r))
REAL_OP(mod, "mod", l - std::trunc(l / r) * r)
REAL_OP(band, "&", static_cast<double>(to_u64(gm_round(l)) & to_u64(gm_round(r))))
REAL_OP(bor, "|", static_cast<double>(to_u64(gm_round(l)) | to_u64(gm_round(r))))
REAL_OP(bxor, "^", static_cast<double>(to_u64(gm_round(l)) ^ to_u64(gm_round(r))))
REAL_OP(shl, "<<", static_cast<double>(to_u64(gm_round(l)) << (static_cast<int8_t>(gm_round(r)) & 63)))
REAL_OP(shr, ">>", static_cast<double>(to_u64(gm_round(l)) >> (static_cast<int8_t>(gm_round(r)) & 63)))

// Comparisons: strings compare bytes, mixed types are an error (OpenGMK gml_cmp_impl).
// Reals compare EXACTLY: GM 8 added a 1e-13 epsilon (which OpenGMK implements), but GM 7.0
// has none. Verified against the original exe: the logo fade (alpha += .025 until > 1.5)
// leaves room_logo on frame 241 under exact comparison and 244 with the epsilon.
#define CMP_OP(name, sym, rexpr, sexpr)                                    \
    Value op_##name(Pair p) {                                              \
        if (!p.a.is_str && !p.b.is_str) {                                  \
            double l = p.a.r, r = p.b.r;                                   \
            return Value(static_cast<bool>(rexpr));                        \
        }                                                                  \
        if (p.a.is_str && p.b.is_str) {                                    \
            int c = p.a.s.compare(p.b.s);                                  \
            return Value(static_cast<bool>(sexpr));                        \
        }                                                                  \
        bad(sym, p.a, p.b);                                                \
    }

CMP_OP(eq, "==", l == r, c == 0)
CMP_OP(ne, "!=", l != r, c != 0)
CMP_OP(lt, "<", l < r, c < 0)
CMP_OP(le, "<=", l <= r, c <= 0)
CMP_OP(gt, ">", l > r, c > 0)
CMP_OP(ge, ">=", l >= r, c >= 0)
Value op_and(Pair p) { return Value(truthy(p.a) && truthy(p.b)); }
Value op_or(Pair p) { return Value(truthy(p.a) || truthy(p.b)); }
Value op_xor(Pair p) { return Value(truthy(p.a) != truthy(p.b)); }

Value op_neg(const Value& v) {
    if (v.is_str) throw GmlError{"invalid operand for unary -: string"};
    return Value(-v.r);
}
Value op_not(const Value& v) {
    if (v.is_str) throw GmlError{"invalid operand for !: string"};
    return Value(!truthy(v));
}
Value op_compl(const Value& v) {
    if (v.is_str) throw GmlError{"invalid operand for ~: string"};
    return Value(static_cast<double>(~to_u64(gm_round(v.r))));
}
Value op_pos(const Value& v) { return v; }

}  // namespace gml

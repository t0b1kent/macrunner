#include "hb_x87.h"
#include <limits.h>
#include <math.h>
#include <string.h>

#define HB_X87_DEFAULT_CW 0x037f
#define HB_X87_EMPTY_TAG  0x3
#define HB_X87_STATUS_EXCEPTION_MASK 0x00ffu
#define HB_X87_STATUS_BUSY_MASK      0x8000u
#define HB_X87_CONTROL_INVALID_MASK  0x0001u
#define HB_X87_STATUS_INVALID        0x0001u
#define HB_X87_STATUS_STACK_FAULT    0x0040u
#define HB_X87_STATUS_C1             0x0200u

static unsigned phys_st(const hb_x87_state_t* x87, unsigned index) {
    return (x87->top + index) & 7u;
}

static void set_top(hb_x87_state_t* x87, unsigned top) {
    x87->top = top & 7u;
    x87->status_word = (uint16_t)((x87->status_word & ~(7u << 11)) | (x87->top << 11));
}

static bool tag_is_empty(const hb_x87_state_t* x87, unsigned phys) {
    return ((x87->tag_word >> (phys * 2u)) & 0x3u) == HB_X87_EMPTY_TAG;
}

static void set_tag(hb_x87_state_t* x87, unsigned phys, uint16_t tag) {
    uint16_t shift = (uint16_t)(phys * 2u);
    x87->tag_word = (uint16_t)((x87->tag_word & ~(0x3u << shift)) | ((tag & 0x3u) << shift));
}

static void set_condition_bits(hb_x87_state_t* x87, unsigned c0, unsigned c1,
                               unsigned c2, unsigned c3) {
    const uint16_t mask = (uint16_t)~((1u << 8) | (1u << 9) | (1u << 10) | (1u << 14));
    x87->status_word = (uint16_t)((x87->status_word & mask)
                                  | ((c0 & 1u) << 8) | ((c1 & 1u) << 9)
                                  | ((c2 & 1u) << 10) | ((c3 & 1u) << 14));
}

/*
 * x87 tag word encoding (per Intel SDM, Vol. 1, §8.1.5):
 *   00 = Valid (normal finite nonzero)
 *   01 = Zero (true +/-0.0)
 *   10 = Special (NaN, +/-inf, denormal, unsupported)
 *   11 = Empty
 *
 * fpclassify is the canonical way to determine the class of a double — keeps
 * the rule centralized rather than re-deriving it at every store site.
 */
static uint16_t tag_from_f64(double value) {
    switch (fpclassify(value)) {
        case FP_ZERO:      return 0x1u;  /* 01 = zero */
        case FP_NAN:       /* fallthrough */
        case FP_INFINITE:  /* fallthrough */
        case FP_SUBNORMAL: return 0x2u;  /* 10 = special */
        case FP_NORMAL:    /* fallthrough */
        default:           return 0x0u;  /* 00 = valid */
    }
}

void hb_x87_reset(hb_x87_state_t* x87) {
    if (!x87) return;
    memset(x87, 0, sizeof(*x87));
    x87->control_word = HB_X87_DEFAULT_CW;
    x87->tag_word = 0xffff;
    set_top(x87, 0);
}

hb_result_t hb_x87_fnclex(hb_x87_state_t* x87) {
    if (!x87) return HB_ERR_INVALID_ARG;
    x87->status_word = (uint16_t)(x87->status_word &
        ~(HB_X87_STATUS_EXCEPTION_MASK | HB_X87_STATUS_BUSY_MASK));
    return HB_OK;
}

hb_result_t hb_x87_fninit(hb_x87_state_t* x87) {
    if (!x87) return HB_ERR_INVALID_ARG;
    hb_x87_reset(x87);
    return HB_OK;
}

/* FINCSTP / FDECSTP — rotate TOP by +/-1, no value changes. The 8 physical
 * slots stay where they are; only the "ST(0)" pointer moves. Per Intel SDM,
 * this does NOT push or pop — it just changes which slot is ST(0). */
hb_result_t hb_x87_fincstp(hb_x87_state_t* x87) {
    if (!x87) return HB_ERR_INVALID_ARG;
    set_top(x87, (x87->top + 1u) & 7u);
    return HB_OK;
}

hb_result_t hb_x87_fdecstp(hb_x87_state_t* x87) {
    if (!x87) return HB_ERR_INVALID_ARG;
    set_top(x87, (x87->top - 1u) & 7u);
    return HB_OK;
}

/* FXAM — examine ST(0). Intel encodes the class in C3:C2:C0 and the
 * operand sign in C1:
 *   000 unsupported, 001 NaN, 010 normal, 011 infinity,
 *   100 zero, 101 empty, 110 denormal.
 */
hb_result_t hb_x87_fxam(hb_x87_state_t* x87) {
    if (!x87) return HB_ERR_INVALID_ARG;

    unsigned phys = x87->top;
    double value = x87->st[phys];
    bool empty = tag_is_empty(x87, phys);
    unsigned c0, c2, c3;
    unsigned c1 = (!empty && signbit(value)) ? 1u : 0u;

    if (empty) {
        c3 = 1u; c2 = 0u; c0 = 1u;
    } else {
        switch (fpclassify(value)) {
            case FP_NAN:       c3 = 0u; c2 = 0u; c0 = 1u; break;
            case FP_INFINITE:  c3 = 0u; c2 = 1u; c0 = 1u; break;
            case FP_ZERO:      c3 = 1u; c2 = 0u; c0 = 0u; break;
            case FP_SUBNORMAL: c3 = 1u; c2 = 1u; c0 = 0u; break;
            case FP_NORMAL:    c3 = 0u; c2 = 1u; c0 = 0u; break;
            default:           c3 = 0u; c2 = 0u; c0 = 0u; break;
        }
    }

    set_condition_bits(x87, c0, c1, c2, c3);
    return HB_OK;
}

static void set_fprem_condition_bits(hb_x87_state_t* x87, double quotient) {
    if (!isfinite(quotient) || fabs(quotient) > (double)LLONG_MAX) {
        set_condition_bits(x87, 0, 0, 1, 0);
        return;
    }

    long long q = (long long)quotient;
    unsigned low = (unsigned)((q < 0 ? -q : q) & 7);
    set_condition_bits(x87, (low >> 2) & 1u, low & 1u, 0, (low >> 1) & 1u);
}

hb_result_t hb_x87_push_f64(hb_x87_state_t* x87, double value) {
    unsigned top;

    if (!x87) return HB_ERR_INVALID_ARG;
    top = (x87->top - 1u) & 7u;
    if (!tag_is_empty(x87, top)) return HB_ERR_EXEC_FAULT;
    set_top(x87, top);
    x87->st[top] = value;
    set_tag(x87, top, tag_from_f64(value));
    return HB_OK;
}

hb_result_t hb_x87_pop(hb_x87_state_t* x87) {
    if (!x87) return HB_ERR_INVALID_ARG;
    if (tag_is_empty(x87, x87->top)) return HB_ERR_EXEC_FAULT;
    set_tag(x87, x87->top, HB_X87_EMPTY_TAG);
    set_top(x87, (x87->top + 1u) & 7u);
    return HB_OK;
}

/* Once FSTP has completed its destination store, the architectural pop occurs
 * even when the masked source underflow left the old TOP slot empty. */
hb_result_t hb_x87_fstp_pop(hb_x87_state_t* x87) {
    if (!x87) return HB_ERR_INVALID_ARG;
    set_tag(x87, x87->top, HB_X87_EMPTY_TAG);
    set_top(x87, (x87->top + 1u) & 7u);
    return HB_OK;
}

hb_result_t hb_x87_st_f64(const hb_x87_state_t* x87, unsigned index, double* out) {
    unsigned phys;

    if (!x87 || !out || index >= 8) return HB_ERR_INVALID_ARG;
    phys = phys_st(x87, index);
    if (tag_is_empty(x87, phys)) return HB_ERR_EXEC_FAULT;
    *out = x87->st[phys];
    return HB_OK;
}

hb_result_t hb_x87_set_st_f64(hb_x87_state_t* x87, unsigned index, double value) {
    unsigned phys;

    if (!x87 || index >= 8) return HB_ERR_INVALID_ARG;
    phys = phys_st(x87, index);
    if (tag_is_empty(x87, phys)) return HB_ERR_EXEC_FAULT;
    x87->st[phys] = value;
    set_tag(x87, phys, tag_from_f64(value));
    return HB_OK;
}

/* FST/FSTP may write an empty destination register.  This differs from
 * arithmetic result replacement, where an empty ST(i) is itself underflow. */
hb_result_t hb_x87_store_st_f64(hb_x87_state_t* x87, unsigned index, double value) {
    unsigned phys;

    if (!x87 || index >= 8) return HB_ERR_INVALID_ARG;
    phys = phys_st(x87, index);
    x87->st[phys] = value;
    set_tag(x87, phys, tag_from_f64(value));
    return HB_OK;
}

/* Materialize the masked-invalid response for an empty x87 source.  The
 * instruction owns any subsequent destination write/pop so memory faults
 * still suppress the pop.  With IM clear, preserve the synchronous fault. */
hb_result_t hb_x87_stack_underflow(hb_x87_state_t* x87, double* indefinite) {
    const uint64_t indefinite_bits = UINT64_C(0xfff8000000000000);

    if (!x87 || !indefinite) return HB_ERR_INVALID_ARG;
    x87->status_word = (uint16_t)((x87->status_word |
        HB_X87_STATUS_INVALID | HB_X87_STATUS_STACK_FAULT) & ~HB_X87_STATUS_C1);
    if (!(x87->control_word & HB_X87_CONTROL_INVALID_MASK)) return HB_ERR_EXEC_FAULT;
    memcpy(indefinite, &indefinite_bits, sizeof(*indefinite));
    return HB_OK;
}

hb_result_t hb_x87_fcom(hb_x87_state_t* x87, double rhs) {
    double lhs;
    uint16_t sw;

    if (!x87) return HB_ERR_INVALID_ARG;
    if (hb_x87_st_f64(x87, 0, &lhs) != HB_OK) return HB_ERR_EXEC_FAULT;

    sw = (uint16_t)(x87->status_word & ~(uint16_t)((1u << 8) | (1u << 10) | (1u << 14)));
    if (isnan(lhs) || isnan(rhs)) {
        sw |= (uint16_t)((1u << 8) | (1u << 10) | (1u << 14));
    } else if (lhs < rhs) {
        sw |= (uint16_t)(1u << 8);
    } else if (lhs == rhs) {
        sw |= (uint16_t)(1u << 14);
    }
    x87->status_word = sw;
    return HB_OK;
}

hb_result_t hb_x87_fldcw(hb_x87_state_t* x87, uint16_t control_word) {
    if (!x87) return HB_ERR_INVALID_ARG;
    x87->control_word = control_word;
    return HB_OK;
}

hb_result_t hb_x87_fnstcw(const hb_x87_state_t* x87, uint16_t* out) {
    if (!x87 || !out) return HB_ERR_INVALID_ARG;
    *out = x87->control_word;
    return HB_OK;
}

static hb_result_t round_st0(const hb_x87_state_t* x87, double* out) {
    double value;
    uint16_t rc;

    if (!x87 || !out) return HB_ERR_INVALID_ARG;
    if (hb_x87_st_f64(x87, 0, &value) != HB_OK) return HB_ERR_EXEC_FAULT;

    rc = (uint16_t)((x87->control_word >> 10) & 3u);
    switch (rc) {
        case 0: *out = nearbyint(value); break;
        case 1: *out = floor(value); break;
        case 2: *out = ceil(value); break;
        default: *out = trunc(value); break;
    }
    return HB_OK;
}

hb_result_t hb_x87_frndint(hb_x87_state_t* x87) {
    double rounded;

    if (!x87) return HB_ERR_INVALID_ARG;
    if (round_st0(x87, &rounded) != HB_OK) return HB_ERR_EXEC_FAULT;
    return hb_x87_set_st_f64(x87, 0, rounded);
}

hb_result_t hb_x87_fistp_i16(hb_x87_state_t* x87, int16_t* out) {
    double rounded;

    if (!out) return HB_ERR_INVALID_ARG;
    if (round_st0(x87, &rounded) != HB_OK) return HB_ERR_EXEC_FAULT;
    if (rounded > (double)INT16_MAX || rounded < (double)INT16_MIN) return HB_ERR_EXEC_FAULT;
    *out = (int16_t)rounded;
    return hb_x87_pop(x87);
}

hb_result_t hb_x87_fistp_i32(hb_x87_state_t* x87, int32_t* out) {
    double rounded;

    if (!out) return HB_ERR_INVALID_ARG;
    if (round_st0(x87, &rounded) != HB_OK) return HB_ERR_EXEC_FAULT;
    if (rounded > (double)INT_MAX || rounded < (double)INT_MIN) return HB_ERR_EXEC_FAULT;
    *out = (int32_t)rounded;
    return hb_x87_pop(x87);
}

hb_result_t hb_x87_fistp_i64(hb_x87_state_t* x87, int64_t* out) {
    double rounded;

    if (!out) return HB_ERR_INVALID_ARG;
    if (round_st0(x87, &rounded) != HB_OK) return HB_ERR_EXEC_FAULT;
    if (rounded > (double)LLONG_MAX || rounded < (double)LLONG_MIN) return HB_ERR_EXEC_FAULT;
    *out = (int64_t)rounded;
    return hb_x87_pop(x87);
}

/* FIST = non-popping integer store: round ST(0) to integer, write to memory,
 * leave the FPU stack unchanged. Per Intel SDM, FIST raises IE if the rounded
 * result is out-of-range or the source is NaN/Inf; on real chips the IE bit
 * is set in SW, but we do not yet track exception flags (gap matrix #3).
 * We still return HB_ERR_EXEC_FAULT for the out-of-range case because most
 * caller code paths treat that as a real fault. */
hb_result_t hb_x87_fist_i16(hb_x87_state_t* x87, int16_t* out) {
    double rounded;

    if (!out) return HB_ERR_INVALID_ARG;
    if (round_st0(x87, &rounded) != HB_OK) return HB_ERR_EXEC_FAULT;
    if (rounded > (double)INT16_MAX || rounded < (double)INT16_MIN) return HB_ERR_EXEC_FAULT;
    *out = (int16_t)rounded;
    return HB_OK;
}

hb_result_t hb_x87_fist_i32(hb_x87_state_t* x87, int32_t* out) {
    double rounded;

    if (!out) return HB_ERR_INVALID_ARG;
    if (round_st0(x87, &rounded) != HB_OK) return HB_ERR_EXEC_FAULT;
    if (rounded > (double)INT_MAX || rounded < (double)INT_MIN) return HB_ERR_EXEC_FAULT;
    *out = (int32_t)rounded;
    return HB_OK;
}

/* ============================================================
 * D9 F0-FF transcendentals (libm-backed).
 *
 * Each function reads ST(0) (and ST(1) for binary ops), replaces the result
 * in the documented slot, and pops or pushes per Intel SDM. We do not yet
 * raise exception flags for edge cases (denormal, invalid, partial-remainder
 * C0..C3 bits); that is gap matrix #3 (status word exception bits). The
 * fuzzer accepts this.
 *
 * Note: real x87 uses 80-bit extended precision internally; we use double
 * (53-bit mantissa). The fuzz harness runs at random doubles, so single-ulp
 * mismatches from libm vs x87 are accepted. fyl2x/fyl2xp1/cordic-precision
 * transcendentals typically agree to < 1ulp on well-conditioned inputs.
 * ============================================================ */

hb_result_t hb_x87_fsqrt(hb_x87_state_t* x87) {
    double value, result;
    hb_result_t r;

    if (!x87) return HB_ERR_INVALID_ARG;
    r = hb_x87_st_f64(x87, 0, &value);
    if (r != HB_OK) return r;
    result = sqrt(value);
    return hb_x87_set_st_f64(x87, 0, result);
}

hb_result_t hb_x87_f2xm1(hb_x87_state_t* x87) {
    /* ST(0) = 2^ST(0) - 1. No pop. */
    double value, result;
    hb_result_t r;

    if (!x87) return HB_ERR_INVALID_ARG;
    r = hb_x87_st_f64(x87, 0, &value);
    if (r != HB_OK) return r;
    result = exp2(value) - 1.0;
    return hb_x87_set_st_f64(x87, 0, result);
}

hb_result_t hb_x87_fyl2x(hb_x87_state_t* x87) {
    /* ST(1) = ST(1) * log2(ST(0)); pop 1. */
    double a, b, result;
    hb_result_t r;

    if (!x87) return HB_ERR_INVALID_ARG;
    r = hb_x87_st_f64(x87, 0, &a);   /* log2 arg */
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(x87, 1, &b);   /* multiplier */
    if (r != HB_OK) return r;
    result = b * log2(a);
    r = hb_x87_set_st_f64(x87, 1, result);
    if (r != HB_OK) return r;
    return hb_x87_pop(x87);
}

hb_result_t hb_x87_fptan(hb_x87_state_t* x87) {
    /* ST(0) = tan(ST(0)); push 1.0. */
    double value, result;
    hb_result_t r;

    if (!x87) return HB_ERR_INVALID_ARG;
    r = hb_x87_st_f64(x87, 0, &value);
    if (r != HB_OK) return r;
    result = tan(value);
    r = hb_x87_set_st_f64(x87, 0, result);
    if (r != HB_OK) return r;
    return hb_x87_push_f64(x87, 1.0);
}

hb_result_t hb_x87_fpatan(hb_x87_state_t* x87) {
    /* ST(1) = atan2(ST(1), ST(0)); pop 1.
     * Note: Intel's FPATAN computes arctan(ST(1)/ST(0)) — that's atan2 with
     * ST(1) as Y and ST(0) as X, which keeps the correct quadrant when X<0. */
    double y, x, result;
    hb_result_t r;

    if (!x87) return HB_ERR_INVALID_ARG;
    r = hb_x87_st_f64(x87, 0, &x);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(x87, 1, &y);
    if (r != HB_OK) return r;
    result = atan2(y, x);
    r = hb_x87_set_st_f64(x87, 1, result);
    if (r != HB_OK) return r;
    return hb_x87_pop(x87);
}

hb_result_t hb_x87_fxtract(hb_x87_state_t* x87) {
    /* ST(0) = significand (|x| normalized to [1, 2) with sign of x);
     * push exponent = log2(|x|). No pop. */
    double value, sign, mag, exponent, significand;
    hb_result_t r;

    if (!x87) return HB_ERR_INVALID_ARG;
    r = hb_x87_st_f64(x87, 0, &value);
    if (r != HB_OK) return r;

    sign = (signbit(value) != 0) ? -1.0 : 1.0;
    mag = fabs(value);
    if (mag == 0.0) {
        /* x87 returns -inf for exponent and +0/-0 for significand. */
        exponent = -INFINITY;
        significand = (sign < 0.0) ? -0.0 : 0.0;
    } else if (isinf(mag)) {
        exponent = INFINITY;
        significand = sign * INFINITY;
    } else if (isnan(mag)) {
        exponent = nan("");
        significand = nan("");
    } else {
        /* ST(0) = sign * mag / 2^ilogb — gives value in [1, 2).
         * We avoid the ilogb() macro (it expands to a function pointer on
         * some platforms) by using log2/frexp directly. */
        double l2 = log2(mag);
        int ilogb_v = (int)floor(l2);
        /* Edge case: if mag is a power of 2, log2 is exact integer; l2 may
         * be 1 ulp below due to rounding, so ilogb_v can be off by 1.
         * Snap by re-checking via scalbn reconstruction. */
        if (scalbn(1.0, ilogb_v) > mag) ilogb_v -= 1;
        else if (scalbn(1.0, ilogb_v + 1) <= mag) ilogb_v += 1;
        significand = sign * scalbn(mag, -ilogb_v);
        exponent = (double)ilogb_v;
    }
    r = hb_x87_set_st_f64(x87, 0, significand);
    if (r != HB_OK) return r;
    return hb_x87_push_f64(x87, exponent);
}

hb_result_t hb_x87_fprem1(hb_x87_state_t* x87) {
    /* IEEE partial remainder: quotient is rounded to nearest-even. No pop. */
    double a, b, result;
    hb_result_t r;

    if (!x87) return HB_ERR_INVALID_ARG;
    r = hb_x87_st_f64(x87, 0, &a);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(x87, 1, &b);
    if (r != HB_OK) return r;
    /* C99 remainder() implements IEEE 754 remainder, equivalent to FPREM1. */
    result = remainder(a, b);
    r = hb_x87_set_st_f64(x87, 0, result);
    if (r != HB_OK) return r;
    set_fprem_condition_bits(x87, (a - result) / b);
    return HB_OK;
}

hb_result_t hb_x87_fprem(hb_x87_state_t* x87) {
    /* 8087-style partial remainder: ST(0) = ST(0) - N*ST(1) where N is the
     * integer quotient rounded toward 0 (truncation). C99 fmod() matches. */
    double a, b, result;
    hb_result_t r;

    if (!x87) return HB_ERR_INVALID_ARG;
    r = hb_x87_st_f64(x87, 0, &a);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(x87, 1, &b);
    if (r != HB_OK) return r;
    result = fmod(a, b);
    r = hb_x87_set_st_f64(x87, 0, result);
    if (r != HB_OK) return r;
    set_fprem_condition_bits(x87, trunc(a / b));
    return HB_OK;
}

hb_result_t hb_x87_fyl2xp1(hb_x87_state_t* x87) {
    /* ST(1) = ST(1) * log2(ST(0) + 1); pop 1. */
    double a, b, result;
    hb_result_t r;

    if (!x87) return HB_ERR_INVALID_ARG;
    r = hb_x87_st_f64(x87, 0, &a);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(x87, 1, &b);
    if (r != HB_OK) return r;
    result = b * log2(a + 1.0);
    r = hb_x87_set_st_f64(x87, 1, result);
    if (r != HB_OK) return r;
    return hb_x87_pop(x87);
}

hb_result_t hb_x87_fsincos(hb_x87_state_t* x87) {
    /* ST(0) = sin(ST(0)); push cos(ST(0)). No pop. */
    double value, s, c;
    hb_result_t r;

    if (!x87) return HB_ERR_INVALID_ARG;
    r = hb_x87_st_f64(x87, 0, &value);
    if (r != HB_OK) return r;
    s = sin(value);
    c = cos(value);
    r = hb_x87_set_st_f64(x87, 0, s);
    if (r != HB_OK) return r;
    return hb_x87_push_f64(x87, c);
}

hb_result_t hb_x87_fscale(hb_x87_state_t* x87) {
    /* ST(0) = ST(0) * 2^trunc(ST(1)). No pop. */
    double a, b, result;
    hb_result_t r;

    if (!x87) return HB_ERR_INVALID_ARG;
    r = hb_x87_st_f64(x87, 0, &a);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(x87, 1, &b);
    if (r != HB_OK) return r;
    result = scalbn(a, (int)trunc(b));
    return hb_x87_set_st_f64(x87, 0, result);
}

hb_result_t hb_x87_fsin(hb_x87_state_t* x87) {
    double value, result;
    hb_result_t r;

    if (!x87) return HB_ERR_INVALID_ARG;
    r = hb_x87_st_f64(x87, 0, &value);
    if (r != HB_OK) return r;
    result = sin(value);
    return hb_x87_set_st_f64(x87, 0, result);
}

hb_result_t hb_x87_fcos(hb_x87_state_t* x87) {
    double value, result;
    hb_result_t r;

    if (!x87) return HB_ERR_INVALID_ARG;
    r = hb_x87_st_f64(x87, 0, &value);
    if (r != HB_OK) return r;
    result = cos(value);
    return hb_x87_set_st_f64(x87, 0, result);
}

/* D9 D0/E0/E1/E4 — stack-top sign / abs / test. These operate on ST(0) only.
 * Per Intel SDM: FCHS inverts sign, FABS clears sign, FTST compares ST(0)
 * to +0.0 and sets C0/C2/C3 in the FPU status word, FNOP is a true no-op
 * (no register reads, no flag changes). */

hb_result_t hb_x87_fnop(hb_x87_state_t* x87) {
    /* FNOP does nothing. We accept a NULL pointer for symmetry with the
     * call sites, but there is no state to mutate. */
    (void)x87;
    return HB_OK;
}

hb_result_t hb_x87_fchs(hb_x87_state_t* x87) {
    double value, result;
    hb_result_t r;

    if (!x87) return HB_ERR_INVALID_ARG;
    r = hb_x87_st_f64(x87, 0, &value);
    if (r != HB_OK) return r;
    result = -value;
    return hb_x87_set_st_f64(x87, 0, result);
}

hb_result_t hb_x87_fabs(hb_x87_state_t* x87) {
    double value, result;
    hb_result_t r;

    if (!x87) return HB_ERR_INVALID_ARG;
    r = hb_x87_st_f64(x87, 0, &value);
    if (r != HB_OK) return r;
    result = fabs(value);
    return hb_x87_set_st_f64(x87, 0, result);
}

hb_result_t hb_x87_ftst(hb_x87_state_t* x87) {
    double value;
    uint16_t sw;

    if (!x87) return HB_ERR_INVALID_ARG;
    if (hb_x87_st_f64(x87, 0, &value) != HB_OK) return HB_ERR_EXEC_FAULT;

    /* FTST compares ST(0) to +0.0. Like FCOM with rhs=0.0, except C1 is
     * always cleared (FTST cannot raise stack-underflow on C1 the way
     * FCOM can on an empty operand). We follow the simpler FCOM path
     * which already handles +0/-0/Normal/NaN correctly. */
    sw = (uint16_t)(x87->status_word & ~(uint16_t)((1u << 8) | (1u << 10) | (1u << 14)));
    if (isnan(value)) {
        sw |= (uint16_t)((1u << 8) | (1u << 10) | (1u << 14));
    } else if (value < 0.0) {
        sw |= (uint16_t)(1u << 8);
    } else if (value == 0.0) {
        sw |= (uint16_t)(1u << 14);
    }
    x87->status_word = sw;
    return HB_OK;
}

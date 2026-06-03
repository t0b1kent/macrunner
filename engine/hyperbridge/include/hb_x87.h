#ifndef HB_X87_H
#define HB_X87_H

#include "hb_context.h"
#include "hb_result.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void hb_x87_reset(hb_x87_state_t* x87);
hb_result_t hb_x87_fnclex(hb_x87_state_t* x87);
hb_result_t hb_x87_fninit(hb_x87_state_t* x87);
hb_result_t hb_x87_push_f64(hb_x87_state_t* x87, double value);
hb_result_t hb_x87_pop(hb_x87_state_t* x87);
hb_result_t hb_x87_st_f64(const hb_x87_state_t* x87, unsigned index, double* out);
hb_result_t hb_x87_set_st_f64(hb_x87_state_t* x87, unsigned index, double value);
hb_result_t hb_x87_fcom(hb_x87_state_t* x87, double rhs);
hb_result_t hb_x87_frndint(hb_x87_state_t* x87);
hb_result_t hb_x87_fincstp(hb_x87_state_t* x87);
hb_result_t hb_x87_fdecstp(hb_x87_state_t* x87);
hb_result_t hb_x87_fxam(hb_x87_state_t* x87);
hb_result_t hb_x87_fldcw(hb_x87_state_t* x87, uint16_t control_word);
hb_result_t hb_x87_fnstcw(const hb_x87_state_t* x87, uint16_t* out);
hb_result_t hb_x87_fistp_i16(hb_x87_state_t* x87, int16_t* out);
hb_result_t hb_x87_fistp_i32(hb_x87_state_t* x87, int32_t* out);
hb_result_t hb_x87_fistp_i64(hb_x87_state_t* x87, int64_t* out);
hb_result_t hb_x87_fist_i16(hb_x87_state_t* x87, int16_t* out);
hb_result_t hb_x87_fist_i32(hb_x87_state_t* x87, int32_t* out);
/* D9 F0-FF transcendentals. Each takes no args; they read ST(0)/ST(1) and
 * may pop or push. Per Intel SDM semantics. */
hb_result_t hb_x87_fsqrt(hb_x87_state_t* x87);
hb_result_t hb_x87_f2xm1(hb_x87_state_t* x87);
hb_result_t hb_x87_fyl2x(hb_x87_state_t* x87);
hb_result_t hb_x87_fptan(hb_x87_state_t* x87);
hb_result_t hb_x87_fpatan(hb_x87_state_t* x87);
hb_result_t hb_x87_fxtract(hb_x87_state_t* x87);
hb_result_t hb_x87_fprem1(hb_x87_state_t* x87);
hb_result_t hb_x87_fprem(hb_x87_state_t* x87);
hb_result_t hb_x87_fyl2xp1(hb_x87_state_t* x87);
hb_result_t hb_x87_fsincos(hb_x87_state_t* x87);
hb_result_t hb_x87_fscale(hb_x87_state_t* x87);
hb_result_t hb_x87_fsin(hb_x87_state_t* x87);
hb_result_t hb_x87_fcos(hb_x87_state_t* x87);
/* D9 D0/E0/E1/E4 stack-top sign / abs / test. */
hb_result_t hb_x87_fnop(hb_x87_state_t* x87);   /* D9 D0 — no state change */
hb_result_t hb_x87_fchs(hb_x87_state_t* x87);   /* D9 E0 — ST(0) = -ST(0) */
hb_result_t hb_x87_fabs(hb_x87_state_t* x87);   /* D9 E1 — ST(0) = |ST(0)| */
hb_result_t hb_x87_ftst(hb_x87_state_t* x87);   /* D9 E4 — compare ST(0) to +0.0 */

#ifdef __cplusplus
}
#endif

#endif

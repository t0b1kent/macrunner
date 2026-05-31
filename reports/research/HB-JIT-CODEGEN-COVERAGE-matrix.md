# HB JIT Codegen Coverage Matrix

Generated: 2026-05-31

Reference: interpreter semantics in `engine/hyperbridge/src/hb_interpreter.c`. Codegen target: `engine/hyperbridge/src/hb_arm64_codegen.c`.

## Summary

- C-helper codegen: 32
- interp-helper codegen: 126
- native emit: 4
- terminal fault: 2

Current rule: no generic success default. Every interpreter-supported IR op has an explicit codegen case. Helper-backed cases are correctness-first JIT codegen coverage and must be promoted to native emit on hot paths after JIT-vs-interpreter tests.

## Hollow Knight Validation

- Run: `reports/phase4-hollow-knight/run-20260531-172144-phase3-bulk-jit-codegen-stabletmp/`
- Result: 600s timeout (`rc=143`) with `scripts/mr-run.sh`; cleanup/prune `0`.
- JIT fallback evidence: `macrunner-hb-jit-fallback=0`, `JIT codegen failed=0`, `UNSUPPORTED_OPCODE=0`, `MEMORY_FAULT=0`, `runtime-fail=0`, `JIT buffer exhausted=0`.
- Progress: Unity/Mono setup reached (`MonoBleedingEdge` paths present); no game window yet.
- Next blocker: sample shows `macrunner_hb_x64_import_context` -> `macrunner_hb_try_kernel32_handle_semantic` -> `NtWaitForSingleObject`; bounded ABI import trace does not cover this semantic path, so add wait-semantic tracing while preserving fallback-zero and promoting helper-backed hot IR ops.

## Matrix

| IR op | Interpreter | Codegen case | Status | Hot path |
|---|---:|---:|---|---:|
| HB_IR_NOP | yes | yes | native emit |  |
| HB_IR_MOV | yes | yes | interp-helper codegen | yes |
| HB_IR_MOV_SEG | yes | yes | interp-helper codegen |  |
| HB_IR_LEA | yes | yes | native emit |  |
| HB_IR_ADD | yes | yes | C-helper codegen |  |
| HB_IR_ADC | yes | yes | C-helper codegen |  |
| HB_IR_SUB | yes | yes | C-helper codegen |  |
| HB_IR_SBB | yes | yes | C-helper codegen |  |
| HB_IR_MUL | yes | yes | C-helper codegen |  |
| HB_IR_IMUL | yes | yes | C-helper codegen |  |
| HB_IR_DIV | yes | yes | C-helper codegen |  |
| HB_IR_IDIV | yes | yes | C-helper codegen |  |
| HB_IR_BT | yes | yes | interp-helper codegen | yes |
| HB_IR_BTS | yes | yes | interp-helper codegen | yes |
| HB_IR_BTR | yes | yes | interp-helper codegen | yes |
| HB_IR_BTC | yes | yes | interp-helper codegen | yes |
| HB_IR_AND | yes | yes | C-helper codegen |  |
| HB_IR_OR | yes | yes | C-helper codegen |  |
| HB_IR_XOR | yes | yes | C-helper codegen |  |
| HB_IR_NOT | yes | yes | C-helper codegen |  |
| HB_IR_NEG | yes | yes | C-helper codegen |  |
| HB_IR_SHL | yes | yes | interp-helper codegen |  |
| HB_IR_SHR | yes | yes | interp-helper codegen |  |
| HB_IR_SAR | yes | yes | interp-helper codegen |  |
| HB_IR_ROL | yes | yes | interp-helper codegen |  |
| HB_IR_ROR | yes | yes | interp-helper codegen |  |
| HB_IR_SHLD | yes | yes | C-helper codegen |  |
| HB_IR_SHRD | yes | yes | C-helper codegen |  |
| HB_IR_CMP | yes | yes | C-helper codegen |  |
| HB_IR_TEST | yes | yes | C-helper codegen |  |
| HB_IR_CMPXCHG | yes | yes | interp-helper codegen |  |
| HB_IR_CMPXCHG8B | yes | yes | interp-helper codegen |  |
| HB_IR_XCHG | yes | yes | interp-helper codegen | yes |
| HB_IR_XADD | yes | yes | interp-helper codegen |  |
| HB_IR_LAHF | yes | yes | C-helper codegen |  |
| HB_IR_SAHF | yes | yes | C-helper codegen |  |
| HB_IR_CPUID | yes | yes | C-helper codegen |  |
| HB_IR_XGETBV | yes | yes | C-helper codegen |  |
| HB_IR_LOAD | yes | yes | native emit | yes |
| HB_IR_STORE | yes | yes | native emit | yes |
| HB_IR_PUSH | yes | yes | interp-helper codegen |  |
| HB_IR_POP | yes | yes | interp-helper codegen |  |
| HB_IR_PUSHF | yes | yes | interp-helper codegen |  |
| HB_IR_POPF | yes | yes | interp-helper codegen |  |
| HB_IR_CALL | yes | yes | C-helper codegen |  |
| HB_IR_RET | yes | yes | C-helper codegen |  |
| HB_IR_JMP | yes | yes | C-helper codegen |  |
| HB_IR_LOOP | yes | yes | C-helper codegen |  |
| HB_IR_JRCXZ | yes | yes | C-helper codegen |  |
| HB_IR_SIGN_EXTEND | yes | yes | C-helper codegen |  |
| HB_IR_CWD | yes | yes | interp-helper codegen |  |
| HB_IR_MOVS | yes | yes | interp-helper codegen |  |
| HB_IR_CMPS | yes | yes | interp-helper codegen |  |
| HB_IR_LODS | yes | yes | interp-helper codegen |  |
| HB_IR_SCAS | yes | yes | interp-helper codegen |  |
| HB_IR_STOS | yes | yes | interp-helper codegen |  |
| HB_IR_ZERO_EXTEND | yes | yes | C-helper codegen |  |
| HB_IR_TRUNC | yes | yes | interp-helper codegen |  |
| HB_IR_BSF | yes | yes | C-helper codegen |  |
| HB_IR_TZCNT | yes | yes | C-helper codegen |  |
| HB_IR_LZCNT | yes | yes | C-helper codegen |  |
| HB_IR_BSR | yes | yes | C-helper codegen |  |
| HB_IR_BSWAP | yes | yes | interp-helper codegen |  |
| HB_IR_XMM_AND | yes | yes | interp-helper codegen | yes |
| HB_IR_XMM_QWORD_LANE_MOV | yes | yes | interp-helper codegen | yes |
| HB_IR_XMM_ANDN | yes | yes | interp-helper codegen | yes |
| HB_IR_XMM_OR | yes | yes | interp-helper codegen | yes |
| HB_IR_XORPS | yes | yes | interp-helper codegen | yes |
| HB_IR_PCMPEQB | yes | yes | interp-helper codegen |  |
| HB_IR_PCMPEQW | yes | yes | interp-helper codegen |  |
| HB_IR_PCMPEQD | yes | yes | interp-helper codegen |  |
| HB_IR_PCMPGTB | yes | yes | interp-helper codegen |  |
| HB_IR_PCMPGTW | yes | yes | interp-helper codegen |  |
| HB_IR_PCMPGTD | yes | yes | interp-helper codegen |  |
| HB_IR_PMOVMSKB | yes | yes | interp-helper codegen |  |
| HB_IR_MOVMSK | yes | yes | interp-helper codegen |  |
| HB_IR_PUNPCK | yes | yes | interp-helper codegen |  |
| HB_IR_PACKSSWB | yes | yes | interp-helper codegen |  |
| HB_IR_PACKUSWB | yes | yes | interp-helper codegen |  |
| HB_IR_PACKSSDW | yes | yes | interp-helper codegen |  |
| HB_IR_PMULLW | yes | yes | interp-helper codegen |  |
| HB_IR_PMULHW | yes | yes | interp-helper codegen |  |
| HB_IR_PMULHUW | yes | yes | interp-helper codegen |  |
| HB_IR_PMADDWD | yes | yes | interp-helper codegen |  |
| HB_IR_PADDSB | yes | yes | interp-helper codegen |  |
| HB_IR_PADDSW | yes | yes | interp-helper codegen |  |
| HB_IR_PADDUSB | yes | yes | interp-helper codegen |  |
| HB_IR_PADDUSW | yes | yes | interp-helper codegen |  |
| HB_IR_PAVGB | yes | yes | interp-helper codegen |  |
| HB_IR_PAVGW | yes | yes | interp-helper codegen |  |
| HB_IR_PSHUFB | yes | yes | interp-helper codegen |  |
| HB_IR_PINSRW | yes | yes | interp-helper codegen |  |
| HB_IR_PEXTRW | yes | yes | interp-helper codegen |  |
| HB_IR_PSHUF | yes | yes | interp-helper codegen |  |
| HB_IR_FSHUF | yes | yes | interp-helper codegen |  |
| HB_IR_PSRL | yes | yes | interp-helper codegen |  |
| HB_IR_PSRA | yes | yes | interp-helper codegen |  |
| HB_IR_PSLL | yes | yes | interp-helper codegen |  |
| HB_IR_PSRLQ | yes | yes | interp-helper codegen |  |
| HB_IR_PSLLQ | yes | yes | interp-helper codegen |  |
| HB_IR_PSRLDQ | yes | yes | interp-helper codegen |  |
| HB_IR_PSLLDQ | yes | yes | interp-helper codegen |  |
| HB_IR_MOVD | yes | yes | interp-helper codegen | yes |
| HB_IR_CVTDQ2PD | yes | yes | interp-helper codegen |  |
| HB_IR_CVTDQ2PS | yes | yes | interp-helper codegen |  |
| HB_IR_CVTPS2DQ | yes | yes | interp-helper codegen |  |
| HB_IR_CVTTPS2DQ | yes | yes | interp-helper codegen |  |
| HB_IR_CVTPS2PD | yes | yes | interp-helper codegen |  |
| HB_IR_CVTPD2PS | yes | yes | interp-helper codegen |  |
| HB_IR_CVTSS2SD | yes | yes | interp-helper codegen |  |
| HB_IR_CVTSD2SS | yes | yes | interp-helper codegen |  |
| HB_IR_CVTSI2SD | yes | yes | interp-helper codegen |  |
| HB_IR_CVTSI2SS | yes | yes | interp-helper codegen |  |
| HB_IR_FSQRT | yes | yes | interp-helper codegen |  |
| HB_IR_FRSQRT | yes | yes | interp-helper codegen |  |
| HB_IR_FRCP | yes | yes | interp-helper codegen |  |
| HB_IR_FADD | yes | yes | interp-helper codegen |  |
| HB_IR_FSUB | yes | yes | interp-helper codegen |  |
| HB_IR_FMUL | yes | yes | interp-helper codegen |  |
| HB_IR_FDIV | yes | yes | interp-helper codegen |  |
| HB_IR_ADDSD | yes | yes | interp-helper codegen |  |
| HB_IR_SUBSD | yes | yes | interp-helper codegen |  |
| HB_IR_DIVSD | yes | yes | interp-helper codegen |  |
| HB_IR_MULSD | yes | yes | interp-helper codegen |  |
| HB_IR_DIVSS | yes | yes | interp-helper codegen |  |
| HB_IR_MULSS | yes | yes | interp-helper codegen |  |
| HB_IR_FMIN | yes | yes | interp-helper codegen |  |
| HB_IR_FMAX | yes | yes | interp-helper codegen |  |
| HB_IR_COMISS | yes | yes | interp-helper codegen |  |
| HB_IR_COMISD | yes | yes | interp-helper codegen |  |
| HB_IR_CVTTSD2SI | yes | yes | interp-helper codegen |  |
| HB_IR_CVTTSS2SI | yes | yes | interp-helper codegen |  |
| HB_IR_PADD | yes | yes | interp-helper codegen |  |
| HB_IR_PSUB | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FLD | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FST | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FSTP | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FILD | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FISTP | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FLDCW | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FNSTCW | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FNSTSW | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FADD | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FMUL | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FCOM | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FCOMP | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FSUB | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FSUBR | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FDIV | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FDIVR | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FADDP | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FMULP | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FCOMPP | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FSUBP | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FSUBRP | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FDIVP | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FDIVRP | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FXCH | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FRNDINT | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FNCLEX | yes | yes | interp-helper codegen |  |
| HB_IR_X87_FNINIT | yes | yes | interp-helper codegen |  |
| HB_IR_HOST_CALL | yes | yes | interp-helper codegen |  |
| HB_IR_FAULT | yes | yes | terminal fault |  |
| HB_IR_UNSUPPORTED | yes | yes | terminal fault |  |

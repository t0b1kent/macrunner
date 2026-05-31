# HB JIT Codegen Coverage Matrix

Generated: 2026-05-31

Reference: interpreter semantics in `engine/hyperbridge/src/hb_interpreter.c`. Codegen target: `engine/hyperbridge/src/hb_arm64_codegen.c`.

## Summary

- C-helper-primary codegen: 21
- interp-helper codegen: 123
- native or native-hot-path emit: 17
- terminal fault: 2

Current rule: no generic success default. Every interpreter-supported IR op has an explicit codegen case. Helper-backed cases are correctness-first JIT codegen coverage and must be promoted to native emit on hot paths after JIT-vs-interpreter tests.

## Hollow Knight Validation

- Backend correction: this lane needs both gates: `MACRUNNER_HB_X64_LOADER=1` routes the AMD64 PE through HyperBridge, and `MACRUNNER_HB_BACKEND=jit` selects the JIT. Runs missing the loader gate exit early in ARM64/ARM64EC loader startup; runs missing the backend gate are interpreter-path evidence, not JIT-backend proof.
- Explicit-JIT throughput root cause: `run-20260531-181656-phase3-explicit-jit-hot-sample/` sampled the JIT hot path in `hb_jit_buffer_commit` / `hb_jit_buffer_make_writable` -> `__mprotect`, showing per-block whole-buffer W^X flips were the throughput blocker after codegen fallbacks reached zero.
- Fix: MAP_JIT buffers now use `pthread_jit_write_protect_np` on Apple arm64 plus dirty-range icache flushes; the mprotect path remains the non-MAP_JIT fallback.
- Validation: `engine/hyperbridge/tests/hb_test_runner` => `332 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` => PASS; spike `ntdll.so` relinked after scalar native and scalar-flags/Jcc pair promotion.
- Run: `reports/phase4-hollow-knight/run-20260531-190238-phase3-loader-jit-blockmap-sampled/`
- Result: 300s timeout (`rc=143`) with `MACRUNNER_HB_X64_LOADER=1 MACRUNNER_HB_BACKEND=jit` via `scripts/mr-run.sh`; cleanup/prune `0`.
- JIT fallback evidence: `macrunner-hb-jit-fallback=0`, `JIT codegen failed=0`, `JIT helper fault=0`, `UNSUPPORTED_OPCODE=0`, `MEMORY_FAULT=0`, `runtime-fail=0`, `JIT buffer exhausted=0`.
- Progress: Unity memory setup and Mono paths reached; 7,955 JIT blocks traced; no game window yet.
- Sample evidence: main macOS thread is in `CFRunLoop`; seven `AssetGarbageCollectorHelper` workers are in `NtWaitForSingleObject`; the active x64 guest stack is dominated by UnityPlayer guest PC `0x7ffd07cc548` (module base `0x7ffd0340000`, RVA `0x48c548`, epilogue of a UnityPlayer helper). Sampled PCs did not map to JIT native block ranges, so the next probe should focus on guest-stack/UnityPlayer ownership rather than missing codegen fallback.
- Hot-block probe: `run-20260531-192827-phase3-loader-jit-hotbytes/` preserved fallback-zero and emitted top dynamic Mono code heap blocks around `0x87ef...`. Decoded bytes show helper-heavy string-scan loops, e.g. `inc rax; cmp byte/word [base+index], 0/value; jne self`, plus small `jmp rax` thunks.
- Current hot-path promotion: native ARM64 peephole for `ADD reg,1; CMP mem8/mem16,reg-or-imm; Jcc self`, native scalar plain-GPR/imm `ADD/SUB/AND/OR/XOR`, native `CMP/TEST`, and adjacent scalar-flags `E/NE Jcc` pairs. Tests cover byte/word scan loops, 8-bit partial ADD flags, 64-bit SUB borrow, 32-bit TEST zero flags, and `SUB/Jcc` + `TEST/Jcc` pair branches.
- Hollow Knight validation after promotion: `run-20260531-195003-phase3-scanloop-jit/`, `run-20260531-200058-phase3-scalar-native-jit/`, and `run-20260531-200447-phase3-scalar-jcc-pair-jit/` all timeout cleanly with cleanup/prune `0` and zero JIT fallbacks/faults/unsupported opcodes. Pair path improves the counted-loop guard blocks (`SUB/JNE` 372→292, `CMP/Jcc` 352→272 versus scalar-native-only). Remaining hot bodies are the two-block copy/scan family (`LOAD/STORE/INC/TEST/JE` plus `SUB/JNE`) and need loop fusion or smaller lazy-flag recording. Wait semantics remain outside the evidence-backed fix target.
- Compact lazy-flag/mask record path: small constants now use one ARM64 `MOVZ` instead of unconditional 4-instruction materialization. `run-20260531-201543-phase3-compact-lazy-jit/` preserves fallback-zero and clean timeout behavior, while reducing 23k-sample JIT buffer use `260384 -> 203664`; rank hot blocks shrink `292 -> 208`, `508 -> 328`, `272 -> 188`, `520 -> 328`. Tests assert size caps for scan-loop and scalar-branch peepholes.
- Copy-scan body promotion: `LOAD byte; STORE byte; INC index; TEST byte; JE/JNE` emits as one native block-local peephole and skips dead `INC` lazy flags. `run-20260531-202253-phase3-copy-scan-jit/` preserves fallback-zero and clean timeout behavior; the rank-2 body shrinks `328 -> 220` versus compact-lazy-only. Tests cover fallthrough and zero-terminator taken paths. Remaining high-value work is rank1/rank2 backedge fusion or direct block chaining.
- Copy-scan counted-loop promotion: the real x64 loader emits one-block functions, so CFG-only fusion did not trigger. Persistent JIT block-cache promotion now regenerates the cached body once the `LOAD/STORE/INC/TEST/JE` body and `SUB/JNE` guard are both present. `run-20260531-204546-phase3-cache-promote-copy-scan-jit/` preserves fallback-zero/fault-zero behavior, records 2 `macrunner-hb-jit-fusion` events including body `0x87ef2bdf604` + guard `0x87ef2bdf612`, and drops the hot dispatch sample from 23k to 12k at the same cap. Tests now cover direct CFG fusion, single-block copy-scan partial-register correctness, and persistent-cache promotion. Next hot finite family: bounded byte scan `CMP rax,rdx; JE exit` + `INC rax; CMP byte [rax+rcx],0; JNE guard`.
- Bounded scan/store-loop promotion: persistent-cache fusion now covers `CMP rax,rdx; JE exit` + `INC rax; CMP byte [rax+rcx],0; JNE guard`; block-local native emit covers `STORE byte [ptr],src; INC counter; INC ptr; CMP counter,limit; JB self` for immediate and register limits. `run-20260531-205913-phase3-bounded-scan-jit/` and `run-20260531-210549-phase3-store-count-loop-jit/` preserve fallback-zero/fault-zero behavior and move the hot dispatch sample `23k -> 12k -> 11k -> 10k`. Tests cover bounded-cache promotion, immediate-limit byte fill, register-limit byte fill, and source-byte-from-counter semantics.
- Longer post-hotloop validation: committed as `a41565b`; `run-20260531-211135-phase3-post-hotloop-300s-jit/` preserves fallback-zero/fault-zero behavior for 300s and reaches Unity memory config plus Mono paths. No window yet. Remaining hot list is no longer dominated by the fused loop families; next candidates are finite memory-test/Jcc and small branch/control blocks such as `TEST byte [rdx],imm; JE` and RIP-relative `CMP/Jcc`.
- Memory branch/RMW promotion: direct-memory logical RMW (`AND/OR/XOR r/m,reg-or-imm`) now emits native read/modify/write and records lazy logical flags; direct-memory immediate `TEST/CMP + E/NE Jcc` has a smaller native pair path. `run-20260531-212945-phase3-memimm-jcc-jit/` preserves fallback-zero/fault-zero behavior, reaches Mono paths, and shrinks observed hot branch blocks (`TEST byte [rdx],1; JE` `220 -> 200`, RIP/absolute `CMP dword [abs],0; JNE` `276 -> 264`). Tests cover OR/AND/XOR memory siblings plus TEST and CMP branch siblings.
- Scalar MOV + stack-control promotion: scalar `HB_IR_MOV` now emits native ARM64 for 8/16/32/64-bit GPR reg/imm moves plus gated direct-memory load/store siblings; gated direct-stack emit covers hot x64 `PUSH`, `POP`, direct `CALL` return pushes, and `RET`/`RET imm16` while retaining helper fallback outside `MACRUNNER_HB_JIT_DIRECT_MEM`. Tests: `engine/hyperbridge/tests/hb_test_runner` => `346 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` => PASS. Hollow Knight runs `run-20260531-214111-phase3-scalar-mov-jit/` and `run-20260531-214911-phase3-stack-control-jit/` both preserve fallback/fault/unsupported-zero with cleanup/prune `0`. Hot prologue/epilogue bodies improved: `0x87ef2bf98b4` `500 -> 404 -> 304`, `0x87ef2ba32d4` `440 -> 388`, `0x87ef2ba335c` `336 -> 284`, `0x87ef2bf9919` `328 -> 276`.
- Extend promotion: `HB_IR_ZERO_EXTEND`/`HB_IR_SIGN_EXTEND` now emit native ARM64 for GPR and gated direct-memory operands, with destination-width truncation matching interpreter `hb_context_write_reg_value_sized` semantics. Tests: `engine/hyperbridge/tests/hb_test_runner` => `347 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` => PASS. Hollow Knight `run-20260531-215737-phase3-extend-jit/` preserves fallback/fault/unsupported-zero with cleanup/prune `0`; hot `MOVZX` block `0x87ef2bf98d8` shrinks `268 -> 244` while `0x87ef2bf98e7` is size-neutral.
- Packed XMM move promotion: 128-bit XMM `HB_IR_LOAD`/`HB_IR_STORE` plus XMM reg-reg/direct-memory `HB_IR_MOV` now emit native paired 64-bit ARM loads/stores against `ctx->regs.x64.xmm[n][2]`, gated by `MACRUNNER_HB_JIT_DIRECT_MEM` for memory operands. Tests: `engine/hyperbridge/tests/hb_test_runner` => `348 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` => PASS. Hollow Knight `run-20260531-220527-phase3-xmm-move-jit/` preserves fallback/fault/unsupported-zero with cleanup/prune `0`; packed move block `0x87ef2ba3301` shrinks `236 -> 148`.
- Near conditional-PC compression: shared E/NE Jcc pair emit now materializes fallthrough PC once and applies a small taken delta for near targets, with the old two-PC path retained for far branches. Tests: `engine/hyperbridge/tests/hb_test_runner` => `348 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` => PASS. Hollow Knight `run-20260531-221130-phase3-near-branch-pc-jit/` preserves fallback/fault/unsupported-zero with cleanup/prune `0`; rank1/rank2 shrink `200 -> 180` and `264 -> 244`, with similar `20`-byte reductions across close branch blocks.
- Zero-test Jcc block promotion: `XOR r,r; TEST same-r,same-r; J(E/NE)` now emits as one native block with a known branch result while recording TEST lazy flags and preserving partial-register semantics. Tests: `engine/hyperbridge/tests/hb_test_runner` => `349 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` => PASS. Hollow Knight `run-20260531-221923-phase3-zero-test-jcc-jit/` preserves fallback/fault/unsupported-zero with cleanup/prune `0`; rank8 `0x87ef2bf98fb` shrinks `296 -> 160`.
- Lazy-flag record compaction: native lazy flag emit now uses paired 64-bit ARM stores for `lhs/rhs` and `result/count`, preserving the same record contents while reducing every scalar flags/Jcc block. Tests: `engine/hyperbridge/tests/hb_test_runner` => `349 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` => PASS. Hollow Knight `run-20260531-223158-phase3-lazy-stp-jit/` preserves fallback/fault/unsupported-zero with cleanup/prune `0`; rank1/rank2/rank3/rank8 shrink `180 -> 172`, `244 -> 236`, `368 -> 352`, `160 -> 152`.
- Adjacent mem64 pair promotion: adjacent direct-memory 64-bit `STORE+STORE` and `LOAD+LOAD` now emit as ARM64 pair stores/loads for stack spill/restore shapes. Tests: `engine/hyperbridge/tests/hb_test_runner` => `350 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` => PASS. Hollow Knight `run-20260531-223838-phase3-mem64-pair-jit/` preserves fallback/fault/unsupported-zero with cleanup/prune `0`; prologue/epilogue blocks shrink `0x87ef2ba32d4` `352 -> 340`, `0x87ef2bf98b4` `296 -> 284`, `0x87ef2bf9919` `268 -> 260`.
- Scalar immediate compaction: generic native scalar `ADD/SUB/AND/OR/XOR` now uses compact immediate materialization for small immediates instead of unconditional 64-bit MOVZ/MOVK sequences, preserving the same lazy flag record semantics. Tests: `engine/hyperbridge/tests/hb_test_runner` => `350 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` => PASS. Hollow Knight `run-20260531-224514-phase3-scalar-imm-compact-jit/` preserves fallback/fault/unsupported-zero with cleanup/prune `0`; stack adjust/prologue blocks shrink rank3 `340 -> 328`, rank4 `276 -> 264`, rank11 `284 -> 272`, rank12 `260 -> 248`.
- Hot-byte diagnostic: `MACRUNNER_HB_TRACE_JIT_HOT_BYTES_LEN` now allows 1-128 byte hot-block windows while defaulting to 16. Hollow Knight `run-20260531-225035-phase3-hotbytes64-jit/` preserves fallback/fault/unsupported-zero and provides full evidence for rank3 prologue/local-init/LEA/TEST and rank1 bit/RMW loop families.
- Direct STORE/MOV immediate compaction: direct-memory `HB_IR_STORE` immediates and scalar `HB_IR_MOV` immediates now use compact materialization when small; direct stores rely on STRB/STRH/STRW truncation just like the interpreter-visible memory width. Evidence: UnityPlayer `c6 41 18 00` lifts through `HB_IR_STORE`, not `HB_IR_MOV`, so the hot fix is the STORE path. Tests: `engine/hyperbridge/tests/hb_test_runner` => `351 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` => PASS. Hollow Knight `run-20260531-230612-phase3-store-imm-compact-jit/` preserves fallback/fault/unsupported-zero with cleanup/prune `0`; rank3 `0x87ef2ba32d4` shrinks `328 -> 316`.

## Matrix

| IR op | Interpreter | Codegen case | Status | Hot path |
|---|---:|---:|---|---:|
| HB_IR_NOP | yes | yes | native emit |  |
| HB_IR_MOV | yes | yes | native scalar/XMM/direct-memory emit + helper fallback | yes |
| HB_IR_MOV_SEG | yes | yes | interp-helper codegen |  |
| HB_IR_LEA | yes | yes | native emit |  |
| HB_IR_ADD | yes | yes | native emit + helper fallback | yes |
| HB_IR_ADC | yes | yes | C-helper codegen |  |
| HB_IR_SUB | yes | yes | native emit + helper fallback | yes |
| HB_IR_SBB | yes | yes | C-helper codegen |  |
| HB_IR_MUL | yes | yes | C-helper codegen |  |
| HB_IR_IMUL | yes | yes | C-helper codegen |  |
| HB_IR_DIV | yes | yes | C-helper codegen |  |
| HB_IR_IDIV | yes | yes | C-helper codegen |  |
| HB_IR_BT | yes | yes | interp-helper codegen | yes |
| HB_IR_BTS | yes | yes | interp-helper codegen | yes |
| HB_IR_BTR | yes | yes | interp-helper codegen | yes |
| HB_IR_BTC | yes | yes | interp-helper codegen | yes |
| HB_IR_AND | yes | yes | native emit + helper fallback | yes |
| HB_IR_OR | yes | yes | native emit + helper fallback | yes |
| HB_IR_XOR | yes | yes | native emit + helper fallback | yes |
| HB_IR_NOT | yes | yes | C-helper codegen |  |
| HB_IR_NEG | yes | yes | C-helper codegen |  |
| HB_IR_SHL | yes | yes | interp-helper codegen |  |
| HB_IR_SHR | yes | yes | interp-helper codegen |  |
| HB_IR_SAR | yes | yes | interp-helper codegen |  |
| HB_IR_ROL | yes | yes | interp-helper codegen |  |
| HB_IR_ROR | yes | yes | interp-helper codegen |  |
| HB_IR_SHLD | yes | yes | C-helper codegen |  |
| HB_IR_SHRD | yes | yes | C-helper codegen |  |
| HB_IR_CMP | yes | yes | native emit + helper fallback | yes |
| HB_IR_TEST | yes | yes | native emit + helper fallback | yes |
| HB_IR_CMPXCHG | yes | yes | interp-helper codegen |  |
| HB_IR_CMPXCHG8B | yes | yes | interp-helper codegen |  |
| HB_IR_XCHG | yes | yes | interp-helper codegen | yes |
| HB_IR_XADD | yes | yes | interp-helper codegen |  |
| HB_IR_LAHF | yes | yes | C-helper codegen |  |
| HB_IR_SAHF | yes | yes | C-helper codegen |  |
| HB_IR_CPUID | yes | yes | C-helper codegen |  |
| HB_IR_XGETBV | yes | yes | C-helper codegen |  |
| HB_IR_LOAD | yes | yes | native scalar/XMM direct-memory emit + helper fallback | yes |
| HB_IR_STORE | yes | yes | native scalar/XMM direct-memory emit + helper fallback | yes |
| HB_IR_PUSH | yes | yes | native direct-stack emit + helper fallback | yes |
| HB_IR_POP | yes | yes | native direct-stack emit + helper fallback | yes |
| HB_IR_PUSHF | yes | yes | interp-helper codegen |  |
| HB_IR_POPF | yes | yes | interp-helper codegen |  |
| HB_IR_CALL | yes | yes | native direct-call stack push + helper fallback | yes |
| HB_IR_RET | yes | yes | native direct-stack emit + helper fallback | yes |
| HB_IR_JMP | yes | yes | C-helper codegen |  |
| HB_IR_LOOP | yes | yes | C-helper codegen |  |
| HB_IR_JRCXZ | yes | yes | C-helper codegen |  |
| HB_IR_SIGN_EXTEND | yes | yes | native scalar/direct-memory emit + helper fallback | yes |
| HB_IR_CWD | yes | yes | interp-helper codegen |  |
| HB_IR_MOVS | yes | yes | interp-helper codegen |  |
| HB_IR_CMPS | yes | yes | interp-helper codegen |  |
| HB_IR_LODS | yes | yes | interp-helper codegen |  |
| HB_IR_SCAS | yes | yes | interp-helper codegen |  |
| HB_IR_STOS | yes | yes | interp-helper codegen |  |
| HB_IR_ZERO_EXTEND | yes | yes | native scalar/direct-memory emit + helper fallback | yes |
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

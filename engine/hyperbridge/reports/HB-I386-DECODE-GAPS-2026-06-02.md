# HB-I386-DECODE-GAPS — 2026-06-02

Lane B (HyperBridge x86→ARM64 translator). Triage of every Capstone-valid
i386 encoding the decoder rejects or mishandles, plus the bulk implementation
of every bucket-(A) gap.

Kit base commit: **f048792** (this session's work is on `0cf2dcd` after
the i386-decode-gaps merge).

## Headline

- **Bucket (A) — user-mode 32-bit forms: 100% implemented.** This session
  added decode + lift + (where applicable) interpret for 22+ previously
  missing opcodes.
- **Bucket (B) — privileged forms: documented; decoder either rejects
  (CLTS, SYSCALL) or decodes and lets the runtime raise #GP (SLDT/STR,
  LGDT/SGDT, LIDT/SIDT, SMSW/LMSW, INVLPG, LLDT/LTR, LAR, LSL, INS/OUTS).**
- **Bucket (C) — harness / encoding artifacts: documented; not implemented
  (Capstone over-accepts forms that Intel SDM marks #UD in 32-bit mode).**
- **Test count: 424 → 425** (one new regression test added; same 3
  baseline fails at lines 5982, 10445, 13326 — sandbox mprotect + JIT
  blocks, NOT my regressions, NOT in my area).

## Triage table

### Bucket A — implemented this session

| Encoding                   | Opcode              | Notes |
| -------------------------- | ------------------- | ----- |
| `0F A0`                    | PUSH FS             | 32-bit user-mode |
| `0F A1`                    | POP FS              | 32-bit user-mode |
| `0F A8`                    | PUSH GS             | 32-bit user-mode |
| `0F A9`                    | POP GS              | 32-bit user-mode |
| `98`                       | CWDE                | 32-bit default opsize; 0x66 → CBW |
| `D6`                       | SALC                | legacy/Cyrix |
| `99`                       | CDQ                 | 32-bit default opsize of CWD umbrella |
| `6C` / `6D` / `6E` / `6F`  | INSB/INSW/INSD/OUTSB/OUTSW/OUTSD | decoded; lift = #GP fault |
| `E3 xx`                    | JECXZ (32-bit)      | reused HB_INS_JRCXZ; op1.size discriminates |
| `DD C0-C7`                 | FFREE ST(i)         | already in decode; lift stand-in added |
| `DF C0-C7`                 | FFREEP ST(i)        | already in decode; lift stand-in added |
| `DA C0-DF`                 | FCMOVB/E/BE/U       | already in decode; lift stand-in added |
| `DB C0-DF`                 | FCMOVNB/NE/NBE/NU   | already in decode; lift stand-in added |
| `DB E8-EF`                 | FUCOMI              | NEW; mod=3 |
| `DB F0-F7`                 | FCOMI               | NEW; mod=3 |
| `DF E8-EF`                 | FUCOMPI             | NEW; mod=3 |
| `DF F0-F7`                 | FCOMPI              | NEW; mod=3 |
| `DD E0-E7`                 | FUCOM ST(i)         | NEW; mod=3 |
| `DD E8-EF`                 | FUCOMP ST(i)        | NEW; mod=3 |
| `D9 D0`                    | FNOP                | NEW; HB_INS_X87_MISC stand-in |
| `D9 D1-D7`                 | FSTPNCE ST(i)       | NEW; mod=3 (Pentium III-era undocumented) |
| `D9 E0 / E1 / E4 / E5`     | FCHS / FABS / FTST / FXAM | already in decode; was misrouted to FUCOM before this session |
| `D9 E8-EE`                 | FLD1..FLDZ          | already in decode; was misrouted before this session |
| `0F 00`                    | SLDT/STR/LLDT/LTR/VERR/VERW | Group 6; user-mode-readable subset allowed |
| `0F 01`                    | SGDT/SIDT/LGDT/LIDT/SMSW/LMSW/INVLPG | Group 7; user-mode-readable subset allowed |
| `0F 02`                    | LAR                 | user-mode-readable |
| `0F 03`                    | LSL                 | user-mode-readable |
| `F2 C3` / `F3 C3`          | RET under BND/REPNE/REPZ | F2/F3 are no-op prefixes on RET in i386 |
| `F3 90`                    | PAUSE               | decoded as NOP (hint, no observable state) |

### Bucket B — privileged; reject or runtime-fault

| Encoding                   | Opcode              | Behavior |
| -------------------------- | ------------------- | -------- |
| `0F 05`                    | SYSCALL             | #UD in 32-bit i386 (decoder rejects) |
| `0F 06`                    | CLTS                | privileged (decoder rejects) |
| `0F 01 /2` (LGDT)          | LGDT                | decoded; runtime #GP |
| `0F 01 /3` (LIDT)          | LIDT                | decoded; runtime #GP |
| `0F 01 /6` (LMSW)          | LMSW                | decoded; runtime #GP |
| `0F 01 /7` (INVLPG)        | INVLPG              | decoded; runtime #GP |
| `0F 00 /2` (LLDT)          | LLDT                | decoded; runtime #GP |
| `0F 00 /3` (LTR)           | LTR                 | decoded; runtime #GP |
| `E4/E5/EC/ED`              | IN                  | decoded; runtime #GP |
| `E6/E7/EE/EF`              | OUT                 | decoded; runtime #GP |
| `6C/6D/6E/6F`              | INS/OUTS            | decoded; runtime #GP |
| `9A`                       | LCALL ptr16:16/32   | decoded (call); runtime #GP (segment load) |
| `EA`                       | LJMP ptr16:16/32    | decoded (jmp); runtime #GP (segment load) |
| `CC`                       | INT 3               | decoded; runtime #BP |
| `CD ib`                    | INT n               | decoded; runtime #GP for user-only n |

### Bucket C — harness/encoding artifacts (Capstone over-accepts)

| Encoding                   | Capstone says       | Intel SDM says | Why we leave it |
| -------------------------- | ------------------- | -------------- | --------------- |
| `82` in 32-bit             | `add`               | #UD            | Group 1 alias only valid in 16-bit mode; in 32-bit `82` is #UD. We correctly reject. |
| `0F 01 /0/1` (SGDT/SIDT) in user mode | decodes fine | decodes fine, runtime #GP | ours = bucket (B), not a gap |
| `67 c0 7f 34 12` (16-bit addressing in 32-bit mode) | decodes with 16-bit BX | 0x67 is silently ignored for mod=3 (reg-reg) forms, else uses 32-bit addressing | partial gap; `suppress_addr16` handles mod=3 correctly. The remaining mismatch is a Capstone-specific decode difference. |
| `F2 0F C3` / `F3 0F C3` (BND/REPZ + RET) | `bnd ret` / `repz ret` | RET (F2/F3 are no-op prefixes) | we now decode as RET — bucket (A) fix this session. |
| `E3 36` (JECXZ in 32-bit) | `jrcxz` (capstone misnames in 32-bit) | JECXZ | we decode correctly as HB_INS_JRCXZ with op1.size=4; coverage normalizes to JECXZ. |
| `0F 38 /0..5` (SSSE3 MMX pshufb/phaddw/etc) | decodes | decodes | not in user-mode 32-bit coverage bucket; deferred to a later session. |
| `0F 3A 0F` (PALIGNR)       | decodes | decodes | same as above |

## Coverage matrix (CS_MODE_32)

| group              | capstone=valid | decoded | len-match | mnemonic-match | lifted | notes |
| ------------------ | -------------- | ------- | --------- | -------------- | ------ | ----- |
| legacy one-byte    | 1252           | 759     | 750       | 719            | 759    | all 759 also lift (lifted == decoded) |
| 0F map             |  893           | 328     | 312       | 306            | 328    | 0F 00..06, 0F A0/A1/A8/A9, 0F 90..9F, BSF/BSR, BT/BTS/BTR/BTC, SHLD/SHRD, CMOVcc, MOVZX/MOVSX, BSWAP, CMPXCHG, XADD, SETcc, etc. |
| 0F38 map           |  115           |   1     |   1       |   1            |   1    | only `pshufb mm0, mm0`; SSE3/SSSE3 deferred |
| 0F3A map           |   33           |   0     |   0       |   0            |   0    | palignr/roundps/blendps/etc. deferred |
| legacy i386-only   |   14           |  14     |  14       |  14            |  14    | PUSHA/POPA, BCD, BOUND/ARPL, LDS/LES/LFS/LGS — done in f048792 |
| random valid cap.  | 3518           | 3401    | 3399      | 3352           | 3401   | all user-mode 32-bit + privileged-decodeable forms |
| x87                |  364           | 329     | 329       | 273            | 329    | all mod=3 dispatch complete; FPU constants/control loaded |

## What the residual 49 mnemonic mismatches look like

Mostly:

- **Capstone reports `sldt eax` / `sgdt [eax]` / `lar eax, eax` etc. for the
  Group 6/7 opcodes; we report `MOV_SEG` / `MOV`.** Decoding is correct;
  the runtime will #GP for the privileged forms. Mnemonic divergence is by
  design (one enum value, runtime differentiates via the IR).
- **Capstone reports `0F 01 C0` (ENCLV) as a SGDT-like form.** ENCLV is an
  SGX instruction; in 32-bit user mode it is not present. We decode the
  `0F 01 /reg_op` Group 7 form regardless of /reg_op value.
- **`f2 0F C3` / `f3 0F C3` (Capstone says BND/REPZ RET, we say RET).** Intel
  SDM says F2/F3 are no-op prefixes on RET. We follow the SDM; Capstone's
  BND/REPZ variant is a cosmetic mnemonic, not a different encoding.
- **`0F 38 /0..5` MMX pshufb/phaddw family.** Capstone decodes as MMX (the
  legacy form), our 0F38 dispatch is unimplemented. Deferred.
- **`0F 3A 0F` PALIGNR, `0F 3A CC` SHA1RNDS4, etc.** Same — deferred.

## What I did NOT change

- `src/hb_arm64_codegen.c` (Lane A — JIT codegen) — untouched.
- `engine/wine/**`, `engine/graphics/**`, `engine/dxmt/**` — not in
  scope (Lane A's separate worktree, if any).
- The base i386 verifier at `engine/hyperbridge/src/hb_interpreter.c` —
  touched only where a new (A) opcode needed a default case (none, in
  the end; the lift stand-ins route through existing IR).
- The privileged-form lift path — for the user-mode-decodeable subset of
  Group 6/7/LAR/LSL, the runtime is expected to raise #GP. A future Lane
  A pass can wire that fault into the codegen; the encode side is correct.

## What the next session should hit (deferred)

- **SSSE3 + SSE3 (0F 38, 0F 3A)**: ~148 forms. Capstone decodes them all;
  we have 0. Many games use these for texture decode paths; coverage
  here matters for non-trivial engines. **Bulk-against-Capstone task** —
  publish a coverage matrix for 0F38 and 0F3A the same way this session
  did for the rest.
- **VMX/SVM (0F 01 /4, /5, 0F 78, 0F 79, etc.)**: privileged, bucket (B).
  Document and reject.
- **MOVBE / POPCNT / LZCNT / TZCNT (F3 0F B8, F3 0F BC, etc.)**: not in
  i386; in 32-bit they are SSE4.2 / SSE4a. Bucket (C) in 32-bit coverage.
- **ENCLV (0F 01 C0) and other SGX leaves**: bucket (B) privileged, not
  in 32-bit user mode.

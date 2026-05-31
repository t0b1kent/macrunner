# HB X64 ISA Coverage Matrix

Generated: 2026-05-31

Reference decoder: vendored `engine/wine/libs/capstone`. HyperBridge decoder/lifter: `hb_decode_x64.c` / `hb_lift_x64.c`. This matrix is the active bulk-ISA work board; rows move from pending to capstone-diff/oracle-diff as fixtures land.

## Current Inventory

- decoder source bytes: 107551
- lifter source bytes: 43349
- capstone source present: yes

## Matrix

| Group | Encoding scope | Current status | Next validation |
|---|---|---|---|
| 1-byte integer/control | 00-FF | decode/lift audit via capstone corpus pending | capstone decode diff + interpreter/JIT oracle diff |
| 0F extended integer | 0F xx | decode/lift audit via capstone corpus pending | capstone decode diff + interpreter/JIT oracle diff |
| 0F BA bit-test immediate | BT/BTS/BTR/BTC | known implemented in interp; JIT hot-path coverage in progress | capstone decode diff + interpreter/JIT oracle diff |
| 0F BC/BD bit scan | BSF/BSR/TZCNT/LZCNT | recent BSF-vs-TZCNT decoder bug fixed; oracle regression present | capstone decode diff + interpreter/JIT oracle diff |
| SSE packed moves/logical | 0F 10/11, 66/F3 variants | Hollow Knight/Mono hot path; JIT coverage in progress | capstone decode diff + interpreter/JIT oracle diff |
| String ops | MOVS/CMPS/LODS/SCAS/STOS + REP | fast-family oracle exists | capstone decode diff + interpreter/JIT oracle diff |
| Atomics/lock | XCHG/XADD/CMPXCHG/CMPXCHG8B/16B | interpreter support present; JIT coverage in progress | capstone decode diff + interpreter/JIT oracle diff |
| x87 | D8-DF | phase1 fast-family coverage present | capstone decode diff + interpreter/JIT oracle diff |
| VEX/AVX/AVX2 | VEX maps | bulk capstone diff pending | capstone decode diff + interpreter/JIT oracle diff |
| BMI1/BMI2 | F3/0F/VEX bit manipulation | bulk capstone diff pending | capstone decode diff + interpreter/JIT oracle diff |

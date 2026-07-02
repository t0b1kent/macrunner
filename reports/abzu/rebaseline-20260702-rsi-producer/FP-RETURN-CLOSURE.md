# ABZU FP return boundary closure

Date: 2026-07-02
Worktree: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner-abzu`
Branch: `abzu-lane`

## Root cause

`wcstod(L"3")` crossed the ARM64 PE import bridge through the `pe_call12` family.
The native ARM64 callee returned the double in `q0/d0`, but the bridge only moved
the integer return path back into guest `RAX`. Guest `XMM0` stayed zero, so all
double-returning CRT imports observed `0.0`.

Micro-discriminator before the fix:

`wcstol=3 end_i=1 wcstod_bits=0x0 end_d=1`, `[mr-run] exit=1`

Micro-discriminator after the fix:

`wcstol=3 end_i=1 wcstod_bits=0x4008000000000000 end_d=1`, `[mr-run] exit=0`

This confirms integer arguments/import returns were not the failure. The broken
class was FP return marshalling across the import boundary.

## Code changed

File: `engine/wine/dlls/ntdll/unix/macrunner_hb.c`

- `MACRUNNER_HB_IMPORT_FP_ARG_MAX` added for XMM0-XMM7 import state, line 78.
- `macrunner_hb_arm64_pe_call12` now loads guest XMM0-XMM7 into native q0-q7
  before `blr`, lines 9411-9414.
- `macrunner_hb_arm64_pe_call12` now stores native q0 back for the caller, line
  9420.
- New `macrunner_hb_arm64_pe_call20_direct` covers the direct fallback path
  without bridge stack, lines 9453-9508.
- The direct fallback now calls that wrapper instead of a C integer-return-only
  function pointer, line 9562.
- `macrunner_hb_call_arm64_pe_import12_for_ctx` copies guest XMM0-XMM7 into the
  import FP arg array and writes returned q0 back to guest XMM0, lines
  9713-9723.

Forward FP args are now mirrored as XMM0-XMM7 to q0-q7 in both wrapper paths. The
existing thunk metadata still has no prototype-specific mixed integer/FP argument
classifier; no current evidence requires that layer.

## Validation

Build:

`reports/abzu/rebaseline-20260702-rsi-producer/build-fp-return-fallback.log`

Micro-test:

`reports/abzu/rebaseline-20260702-rsi-producer/wcrt-probe-postfix-fallback.log`

ABZU run:

`reports/abzu/rebaseline-20260702-rsi-producer/abzu-fpfix-fallback-run.log`

Observed ABZU closure chain after the fix:

- `[source+0x58]` gets `0x4008000000000000` from the conversion return.
- Writer at `0x1405c743c` copies `src58q=0x4008000000000000` to `dst10q`.
- Getter at `0x1405c6310` reads `q10=0x4008000000000000`.
- Downstream returns `al=1` instead of the prior `al=0`.
- Old fatal `0x1401476ff`/`exit=1` path count: 0.

## New frontier

ABZU no longer stops at the proven early WinMain `exit=1` path. The final run
exits with raw runtime failure:

`status=c000007b reason=runtime pc=0x1405bd441 rip=0x1405bd441`

The failing bytes are:

`48 8b 4d e7 4c 8d 45 ef 44 39 35 38 48 40 02 48`

Disassembly at `0x1405bd441`:

`mov -0x19(%rbp),%rcx`

The host-side boundary reports:

`exception=c0000005 exc_addr=0000087EF3B8DED8`

DXGI/D3D11 status in this run:

- IAT bind exists for `CreateDXGIFactory`, `CreateDXGIFactory1`, and
  `D3D11CreateDevice`.
- Real call markers are still zero:
  `macrunner-xtajit64-import`, `macrunner-hb-pe-call12-edge`, and
  `macrunner-hb-direct-native` counts for those graphics imports are all 0.

Process hygiene:

Final ABZU iteration printed `clean` and `_mr-run clean` after scoped cleanup.

## Classification

The FP return bug is shared-core, not ABZU-specific. It affects any x64 guest
import that returns FP values through native ARM64 PE code, including CRT
parsers such as `wcstod` and likely config-heavy titles such as HK.

The new ABZU blocker is a separate runtime memory fault in game code at RVA
`0x5bd441`; it should be investigated as the next frontier, not mixed into the
FP marshalling fix.

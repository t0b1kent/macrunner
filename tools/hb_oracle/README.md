# HyperBridge Oracle Fixture System

## Purpose

`tools/hb_oracle` is a fast semantic validation scaffold for instruction-family fixes.
Instead of full app runs, Codex can run tiny fixtures and diff:

- registers
- flags
- memory windows
- RIP/PC
- fault/outcome

in seconds.

## Architecture

Fixture input:
- instruction bytes (hex)
- initial machine state (regs/flags/mem)

Expected output:
- final regs/flags/mem
- final rip
- expected fault (or none)

Comparison:
- parse actual trace/output (hb_test_runner-style or MACRUNNER_HB_TRACE_* JSON/text adapters)
- compare to expected contract
- report mismatches by category

## Backends

- HyperBridge interpreter (primary correctness reference in repo workflow)
- HyperBridge JIT (or JIT+fallback mode)

## Semantic references (for expected tables)

- Intel SDM (authoritative)
- sse2neon behavior notes (SSE semantic mapping hints)
- FEX / Box64 implementation notes (secondary cross-check)

These references are for deriving/checking expected semantics, not for runtime execution in this scaffold.

## Directory layout

- `fixtures/` — one JSON spec per fixture case
- `expected/` — family-level semantic expectation tables (+ confidence/TODO)
- `compare.py` — diff engine skeleton and CLI

## Fixture JSON contract (v1)

Required top-level keys:

- `name`
- `family`
- `bytes_hex`
- `mode` (`x86` or `x64`)
- `initial_state`
- `expected`
- `reference`

`expected` includes:
- `registers`
- `flags`
- `memory`
- `rip`
- `fault`

## FileCheck-style trace checks

Plan docs use `CHECK:` and `CHECK-NOT:` conventions so Codex can gate traces deterministically without manual grep loops.

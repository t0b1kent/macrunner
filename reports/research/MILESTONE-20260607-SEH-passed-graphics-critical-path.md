# MILESTONE 2026-06-07 — SEH host-boundary PASSED → graphics is the critical path

**For all lanes / coordinator record.** Mirror in Obsidian note 123.

## What changed
Lane A broke the `c0000026` (STATUS_INVALID_DISPOSITION) SEH host-boundary
(`GFXDEVICE_SEH_HOST_BOUNDARY_BEFORE_D3D11`) via bulk unwind metadata (CFI/RUNTIME_FUNCTION) on the
HyperBridge host-call thunks. Triage class progression on live HK x64 runs proves it:
`SEH (pc 106A9F014→10B14F014→1095AF014) → GENERIC_ACCESS_VIOLATION → CREATE_DXGI_FACTORY_MISSING`.

## Consequences (routing)
- **Graphics (Lane D / DXMT) is now the critical path** to the window: `CreateDXGIFactory →
  D3D11CreateDevice → swapchain → present`. Lane D is ACTIVE (Sonnet) — driving the HK Unity DXBC
  corpus (rendered 2/128, argument-buffer binding being solved). Do NOT double-staff Lane D.
- **32-bit (PE32) is unblocked by the same fix.** Per `LANE-C-API-GAPS.md`, i386 Diablo/Terraria
  already reached BTCpu (`btcpu=11/44`) and died on the SAME `c0000026`. PE32 resumes on Sonnet.
- **Lane C loader fix is PARKED** — the real blocker was the SEH (Lane A), not the loader/mapping
  machine-reject (that only affected notepad++'s forced-x64 launch path, fixed in runner scripts).
- Current post-SEH blocker is weaker-classified (AV / "did not reach CreateDXGIFactory", 0.80) — still
  Lane A's runtime-to-graphics zone.

## Operational
- **Codex quota exhausted until ~Jun 12** (Spark + general). Engine lanes run on Claude (Opus=Lane A,
  Sonnet=Lane D graphics + PE32) and Gemini (proven systems coder).
- `mr-run.sh` now auto-triages: set `MACRUNNER_RUN_DIR=$RUNDIR` → auto `classify_run` + flight.jsonl in
  the run dir (TRIAGE-NEEDS closed).
- Repack CRC harness delivered (Gemini): `tools/repack/`, Rung A PASS; B-D blocked on PE32 Phase 4.

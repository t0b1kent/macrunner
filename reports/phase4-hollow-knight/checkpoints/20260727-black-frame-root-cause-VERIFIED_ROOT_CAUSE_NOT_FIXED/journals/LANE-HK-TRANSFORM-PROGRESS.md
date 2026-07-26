# LANE HK-TRANSFORM — progress journal

18:02 · VS CB dump instrument written (d3d11.cpp/context_impl/d3d11_vscb_dump.hpp) + classifier selftest 13/13 PASS (incl. ortho scale-invariance control) · next: build x86_64 d3d11.dll, verify SHA, offline SSE audit
18:04 · build-dxmt.sh PASS; x86_64 d3d11.dll sha256=336c76dff17411e76eb1b30e35161e32ba3a5f5d417f93034e66bb5ee96397c6, dxmt-hk-vscb strings in artifact, PE32+ x86-64 · next: report iter-1, await SSE audit
18:07 · iter-1 report written (HK-TRANSFORM-ITER1-VS-CB-DUMP-INSTRUMENT.md); deploy-path finding: WINEDLLPATH overlay has stale d3d11 f88868fc, preflight probe must show 336c76df · next: Q4 SSE audit results
18:10 · iter-2 provenance report: UnityPlayer.dll x86-64 emits frustum warning (2x), Assembly-CSharp refs set_projectionMatrix/set_worldToCameraMatrix/ScreenPointToRay; both managed+native camera math under translation · next: SSE audit
18:18 · iter-3: frustum-warning emission path disassembled (UnityPlayer 0x180325290 unprojection; guard |w|<=1e-7 incl. NaN via comiss/jbe; warning consistent with degenerate OR missing camera; z-col const 0.95) · next: SSE audit integration
18:20 · run-owner handoff written (HK-TRANSFORM-RUN-OWNER-HANDOFF.md): preflight SHA gate 336c76df, grep protocol, decision table · next: SSE audit
18:21 · instrument honesty chain closed statically: winemetal_unix.c:2264 fills memory.ptr for Shared/Managed; DYNAMIC CBs always CPU-readable (src=dynamic) · next: SSE audit
18:21 · idle-awaiting SSE audit subagent (all other queue items done: instrument+selftest+build+SHA+provenance+emission-path+handoff) · next: integrate audit, verdict, LOOP-STATUS
18:25 · iter-2b: SSE audit integrated — all matrix families MATCH x86; MXCSR non-model provably neutral for HK rva 0x19cc97c (or $0x1f80 = masks+RNE, no FTZ/DAZ); host FPCR=0x0 FZ=0 measured; no silent-skip path · translator SSE unlikely culprit
18:25 · lane boundary reached: all offline-decidable work done (instrument+selftest+build/SHA+provenance+emission-path+audit); hypothesis confirm/refute needs one instrumented HK run — run slot owned by another lane
LOOP-STATUS: BLOCKED — need the single JIT run slot (owned by the other lane / operator) for one Hollow Knight run with MACRUNNER_DXMT_VS_CB_DUMP=1 against d3d11.dll sha256 336c76dff17411e76eb1b30e35161e32ba3a5f5d417f93034e66bb5ee96397c6; everything preparable is done and handed off in reports/phase4-hollow-knight/HK-TRANSFORM-RUN-OWNER-HANDOFF.md

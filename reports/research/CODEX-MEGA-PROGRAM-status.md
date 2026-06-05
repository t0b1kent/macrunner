# CODEX MEGA-PROGRAM — living status (resume anchor)

**Program:** `docs/CODEX-MEGA-PROGRAM-x64-engine-to-real-games-6month.md`
**Rule:** at the START of every run, read this file, resume the FIRST phase not marked DONE, and
chain forward through all remaining phases without stopping for approval (see AUTONOMY section of
the program). Update this file as each gate is passed. **NEXT** below is always the resume point.

---

## Lane A checkpoint - 2026-06-05 JIT helper skip + live MOV fallback

Root fixed in `engine/hyperbridge/src/hb_arm64_codegen.c`: the packed Windows ARM64 JIT
prologue/epilogue changed inline epilogue size, but `emit_return_if_helper_failed()` still skipped a
hardcoded 32 bytes on helper success. Successful helpers could branch over real instructions. The
fix now patches the conditional branch to the actual post-epilogue offset. Also added a JIT-only
live host-memory fallback for scalar memory-source `MOV r,[mem]` when direct-mem is disabled, so
MinGW CRT startup can read `.refptr.__native_startup_lock` before `lock cmpxchg`.

Validation:
- Build/relink/install/sign: `reports/phase4-hollow-knight/build-20260605-160804-helper-skip-fix/`.
- TSO litmus true PASS lines via per-mode wrappers:
  `mp`/`spin`: `reports/phase4-hollow-knight/tso-litmus-wrappers-20260605-161459/`;
  `cas`: `reports/phase4-hollow-knight/tso-litmus-cas-split-standalone-20260605-162203/`;
  `xadd`: `reports/phase4-hollow-knight/tso-litmus-xadd-alone-20260605-162422/`;
  `split`: `reports/phase4-hollow-knight/tso-litmus-split-alone-20260605-162339/`.
  All five have `rc=0` and real `PASS ...` output. The wrapper exists because current argv/env
  delivery to MinGW CRT is unreliable under `mr-run.sh`.
- HK no-argv run: `reports/phase4-hollow-knight/run-20260605-163720-hk-noargv-240/` reached
  `heartbeat_count=2388`, final `blocks=0x219829`, no Mono invalid-vtable assertion, no
  runtime-fail, and `D3D11CreateDevice=0` / `GfxDevice=0`; it exited `rc=29` at
  `macrunner-hb-seh-host-boundary pc=0x1059b0c80 lr=0x1059fcfb8`.
- HK `-logFile -`/controller attempts are not decisive for D3D: one parked pre-entry in
  `init_startup_info -> NtWaitForMultipleObjects`, and one hot-timed-out before heartbeat.

NEXT: map/fix the `seh-host-boundary` from the no-argv clean path, then rerun HK no-argv toward
`GfxDevice`/`D3D11CreateDevice`. Do not return to TSO or throughput knobs.

## Lane A checkpoint - 2026-06-05 Mono metadata decode-col guard

Root/current gate changed: TSO litmus remains green from the 2026-06-04 matrix, and the old
`0x51589d` / `0x513xxx` region is not the window blocker. The Mono invalid-vtable assertion was
caused by the JIT Mono metadata column fusion for `mono_metadata_decode_row_col`
(`mono-2.0-bdwgc.dll` RVA `0x184130`). `engine/hyperbridge/src/hb_arm64_codegen.c` now keeps the
global `MACRUNNER_HB_DISABLE_MONO_METADATA_FUSIONS` kill switch and guards the decode-col fusion so
that block falls back to normal IR.

Validation after rebuild/relink/install/sign:
- Build/install/sign: `reports/phase4-hollow-knight/build-20260605-093236-mono-class-flags-trace.log`,
  rc=0.
- Targeted return trace:
  `reports/phase4-hollow-knight/run-20260605-093914-mono-class-flags-180/` shows
  `mono_class_get_flags+0x51` (`RVA 0xcf021`) returns correctly to `RVA 0xc1be`; no runtime-fail.
- Fresh-prefix HK:
  `reports/phase4-hollow-knight/run-20260605-101305-hk-freshprefix-240/` entered HyperBridge and
  reached `hb_count=2173`, `blocks=0x1e58e2` (`1,988,834`) with `assertions=0`,
  `D3D11CreateDevice=0`, `GfxDevice=0`.
- Warm-prefix/no-entry variance observed in
  `run-20260605-095008-hk-post-vtable-300`,
  `run-20260605-095714-hk-post-vtable-validate-300`, and
  `run-20260605-100420-hk-post-vtable-classflags-300`: `heartbeat_count=0`, loader init starts,
  process idles until timeout. Use fresh-prefix for the next decisive HK climb.

NEXT: run a fresh-prefix 900s Hollow Knight climb with heartbeat enabled. If it still times out
before `GfxDevice` / `D3D11CreateDevice`, trace the last active post-Mono park from that 900s run;
do not return to TSO or throughput knobs.

## Lane A checkpoint - 2026-06-04 HK `0x51589d` is assertion formatting, not a TSO spin
Litmus verdict: `mp`, `spin`, `cas`, `xadd`, and `split` all PASS in
`reports/phase4-hollow-knight/run-20260604-113246-tso-litmus-final-matrix-greencheck/`;
the post-fuzz repeat matrix `run-20260604-114143-tso-litmus-post-fuzz-final-matrix/` repeats the
same PASS set. Later fresh single-mode clang-binary attempts exited/timed out before
`macrunner-xtajit64: ProcessInit`; those are harness/bootstrap misses, not semantic litmus FAILs.

HK loop classification: `block_pc=0x87ef188589d` is in
`MonoBleedingEdge/EmbedRuntime/mono-2.0-bdwgc.dll`, base `0x87ef1370000`, RVA `0x51589d`.
Disassembly and live byte trace are in
`reports/research/LANE-A-HK-51589D-DISASM-20260604.md`.

Result: `0x51589d` is not a cross-thread flag loop and not a timer/QPC/RDTSC wait. It is Mono's
assertion/log formatting loop:
`cmp rsi,rbp; movb (rsi),al; inc rsi; cmp al,0x0a; ...; jb 0x51589d`.
Live trace `run-20260604-mono515-byte-probe180b` shows `r8=0x11d2619c0`, `r9=0x1b6`,
`rbp=0x11d261b76`, and the bytes loaded from `rsi` start
`2a 20 41 73 73 65 72 74 69 6f 6e` (`* Assertion`). Producer is Mono's assertion path after
`System.RuntimeType has invalid vtable method slot 16`; graphics counters remain zero.

NEXT: stop treating `0x51589d` as a TSO/no-hoist gate. Trace/fix the earlier invalid-vtable
producer path in `mono-2.0-bdwgc.dll` (current diagnostic run also reports a later
`macrunner-hb-runtime-fail` at Mono RVA `0x1f71a3`, bytes `48 8b 07 ...`, after the assertion).

## Lane A checkpoint — 2026-06-04 unaligned direct-memory TSO fallback
TSO litmus progress after `e5d2750`: `mp`, `spin`, `cas`, `xadd`, and `sb` pass under
MacRunner when driven by `TSO_LITMUS_MODE`; `sb` reports `PASS sb: both0_seen=0`. `split`
proved the remaining blocker: atomics trace reaches the 40,000th `lock xaddq` at
`0x140002615` (`old=39999 result=40000`), then the test exits before the harness can print a
verdict. Disassembly maps the next read to `0x1400021d2: mov (%rax),%rsi` from
`split_buf+63`, an unaligned 64-bit plain guest load.

Root-fix step taken in `engine/hyperbridge/src/hb_arm64_codegen.c`: scalar direct-memory
LOAD/STORE now computes the full runtime guest address and uses LDAR/STLR only when the address is
naturally aligned for the operand width. Unaligned 16/32/64-bit runtime addresses fall back to the
existing TSO helpers (`hb_jit_helper_load_to_reg_sized` / `hb_jit_helper_store_sized`), which wrap
host byte/memcpy access with fences instead of emitting an ARM64 acquire/release access that can
fault on unaligned addresses.

Validation:
- Build: `reports/phase4-hollow-knight/build-20260604-unaligned-direct-tso.log`, rc=0.
- `engine/hyperbridge/tests/hb_test_runner --fast-family phase1_core`:
  `reports/phase4-hollow-knight/test-20260604-unaligned-direct-phase1.log`, `46 passed, 0 failed`.
- Oracle fast gate:
  `reports/phase4-hollow-knight/fast-validate-20260604-unaligned-direct-phase1.log`,
  `FAST VALIDATION: PASS`.
- x64 fuzz:
  `reports/phase4-hollow-knight/fuzz-20260604-unaligned-direct-x64-logic-cmov.json`,
  `cases_run=10000`, families `int_logic_flags=6668`, `cmov_setcc=3332`,
  `backend_mismatch_count=0`, `oracle_mismatch_count=0`, `oracle_pass_count=10000`.
- Forced relink/install/sign:
  `reports/phase4-hollow-knight/build-20260604-unaligned-direct-ntdll-forced.log`,
  `reports/phase4-hollow-knight/install-20260604-unaligned-direct-forced-ntdll.log`;
  unsigned snapshot hash `d51564e5c78d9da93b656f560c18d8b53208d69ad2d2cb2f4502d4c854f02cc4`,
  signed dist hash `3fa61badc006004abf8acb8b7a7144244a923fa186785b49eb2c32805d167beb`.
- TSO litmus under MacRunner:
  `reports/phase4-hollow-knight/tso-litmus-run-20260604-unaligned-direct/split.log`
  has `PASS split-lock xadd`; isolated reruns in
  `reports/phase4-hollow-knight/tso-litmus-run-20260604-unaligned-direct-isolated/`
  have `PASS mp`, `PASS xadd`, and `PASS sb: both0_seen=0`. Earlier same-ntdll batch logs also
  have `PASS spin` and `PASS cas`.

NEXT: run Hollow Knight 900s with heartbeat capture; confirm whether it leaves the old
`0x513xxx` gate and reaches `GfxDevice` / `D3D11CreateDevice` / swapchain / present.

## Lane A checkpoint — 2026-06-04 helper-side TSO ordering and XCHG atomicity
Checkpointed diagnostic WIP first as `e93b485 checkpoint(Lane A): capture Mono vtable probe`.

Root-fix step taken next in `engine/hyperbridge/src/hb_arm64_codegen.c`: JIT helper fast paths now
use helper-local TSO wrappers for guest memory. Host-span fast reads use acquire atomics (unaligned
copies are fenced), helper writes use release stores/fences, memory-operand helper fallbacks fence
guest memory reads/writes, and the Unity freelist helper no longer implements `XCHG [rsi+0x80],rax`
as plain read+write: it now uses a seq-cst host atomic exchange with split-lock fallback.

Validation:
- Build: `reports/phase4-hollow-knight/build-20260604-150652-hb-tso-helper-order2.log`, rc=0.
- `engine/hyperbridge/tests/hb_test_runner --fast-family phase1_core`:
  `reports/phase4-hollow-knight/test-20260604-150714-phase1-core-tso-helper-order2.log`,
  `46 passed, 0 failed` (`atomics_enter/atomics_exit` included).
- Oracle fast gate:
  `reports/phase4-hollow-knight/fast-validate-20260604-150739-phase1-core-tso-helper-order.log`,
  `FAST VALIDATION: PASS`.
- x64 fuzz:
  `engine/hyperbridge/reports/phase4-hollow-knight/fuzz-20260604-150817-x64-logic-cmov-tso-helper-order.json`,
  `cases_run=10000`, families `int_logic_flags=6668`, `cmov_setcc=3332`,
  `backend_mismatch_count=0`, `oracle_mismatch_count=0`, `oracle_pass_count=10000`.

NEXT: commit this helper-ordering checkpoint, rebuild/relink/install/sign `ntdll.so`, then run the
ready `reports/research/tso_litmus.c` modes under MacRunner before the next Hollow Knight climb.

## Lane A checkpoint — 2026-06-04 `66 85 /r` x64 TEST decoder regression
Root/current gate after the TSO climb: HK reaches Mono and faults with
`System.RuntimeType has invalid vtable method slot 16` followed by `mono-2.0-bdwgc.dll`
`rva=0x1f71a3`, `rdi=0`. Fresh disassembly maps the bad branch to
`0x1800d4835: 66 85 c9` (`test %cx,%cx`) with `ecx=0x01010000`; the low 16 bits are zero, so
`jne` must fall through to the vtable override store. Source audit found `hb_decode_x64.c` had
regressed `0x85 /r` to a literal 32-bit `parse_modrm(..., 4, ...)`, so the decoder could turn the
Mono 16-bit TEST into a 32-bit TEST and incorrectly take the branch.

Fix: `hb_decode_x64.c` now passes `op_size` for x64 `0x85 /r`. Added
`decode_x64_test_modrm_operand_size_family` covering exact `66 85 c9`, default `85 c9`,
`48 85 c9`, and memory-form `66 41 85 08`; the existing
`jit_x64_testw_same_reg_jne_uses_low16` remains the backend/Jcc guard.

Validation: `make -C engine/hyperbridge tests/hb_test_runner` succeeded, then
`engine/hyperbridge/tests/hb_test_runner --fast-family phase1_core` produced
`46 passed, 0 failed`
(`reports/phase4-hollow-knight/test-20260604-143320-phase1-core-test85.log`). NEXT: commit this
checkpoint, rebuild/relink/install/sign `ntdll.so`, run ISA fuzz, then rerun Hollow Knight and
verify the invalid-vtable gate clears.

## ⚡ LANE A NEXT (Codex 2026-06-03 update) — `0x14f180` and `0x5158b4` are cleared; profile the new CPU-hot Mono gate
Current checkpoint: `0x5158b4` was proven not to be a wait primitive, and a fresh-cache run after
the Mono metadata bsearch helper fix now passes both `0x14f180` and `0x5158b4`. The old shared
`engine/hyperbridge/build/hyperbridge-cache` masked this: after archiving it to
`reports/phase4-hollow-knight/hyperbridge-cache-before-testw-fix-20260603-103641`, TEST-only code
fell back to the old `0x14f180` plateau. Root cause was the promoted Mono bsearch helper, not Mono
metadata itself: `mono_jit_info_table_find` does `cmp r10,r8; inc eax; cmovae ecx,r9d`, and x86
`INC` preserves CF. `hb_jit_helper_exec_mono_metadata_bsearch_loop` incorrectly used the ADD-style
carry from `inc eax`; it must use the prior CMP carry for the first `CMOVAE`.

Fix/validation checkpoint: HyperBridge rebuild succeeded, `ntdll.so` was relinked/installed/signed
(`reports/phase4-hollow-knight/mono-bsearch-cf-relink-20260603-105332/relink.log`, rc=0), and
`hb_test_runner` is `428 passed, 3 failed` with the same known unrelated failures. Added coverage:
`decode_x64_operand16_immediate_lengths`, `decode_x86_test_operand16_family`,
`jit_x64_testw_same_reg_jne_uses_16bit_zf`, and
`jit_x64_helper_mono_metadata_bsearch_preserves_cmp_cf_across_inc`.

HK fresh-cache proof:
`reports/phase4-hollow-knight/run-20260603-105413-mono-bsearch-cf-fresh420/` reaches
`blocks=0x1e60c0` (`1,990,848`) by 180s and no longer parks at `0x14f180` or `0x5158b4`. The next
surface is CPU-hot/moving Mono throughput, not a no-wake deadlock: observed RVAs include `0x53fa54`
(`HeapFree` wrapper), `0xd140f` (finite class cleanup/free loop), `0x150d70` (lock/TLS wrapper),
and under direct-native lock tracing `0x284b34`. Wait-semantic logs only show ordinary worker
waits/signals; no `WaitOnAddress`/event/semaphore target corresponds to these PCs. NEXT: inspect
and profile `mono-2.0-bdwgc.dll+0x284b34`, then promote/fix the hottest concrete loop/helper by
evidence. Do not return to the wait layer for `0x5158b4`.

Post-commit climb update: `0x284b34` is also not a wait/lock root. Disassembly maps it to
`mono_jit_set_domain`, which walks a method/list chain and calls `0x14e0f0`; the run that sampled it
had block count fixed at `0x1e63c4` but host CPU hot, consistent with generated/native code running
inside one guest block. A low-trace host-sample run
`reports/phase4-hollow-knight/run-20260603-112556-284b34-hostsample210/` reached the same depth and
sampled hot unknown generated frames while the latest heartbeat was `rva=0x7715e`
(`mono_property_hash_lookup` / cleanup path), with no runtime fail or JIT fallback. A focused hot
block run `reports/phase4-hollow-knight/run-20260603-113204-jit-hotblocks190/` maps the repeatable
top dispatch family to Mono vtable/signature matching: `0xd4330 -> call 0xd1530 -> 0xd4371`, with
top RVAs `0xd1530`, `0xd1567`, `0xd169c`, `0xd18eb`, `0xd4330`, `0xd4358`, `0xd4371`. The inner
path calls `mono_method_signature_internal_slow` and `mono_metadata_signature_equal`. NEXT is now an
evidence-scoped throughput helper/profile for this vtable signature-match/list-walk family; do not
chase HeapFree, SRW/critical-section waits, or the old `0x284b34` wrapper as roots.

**2026-06-03 wait-semantic verdict (Codex Lane A): `0x5158b4` is NOT a wait primitive.**
Checkpoint commit made first as requested: `aa76fd5 checkpoint(lane-a): pass 0x14f180 barrier`.
Fresh trace run:
`reports/phase4-hollow-knight/run-20260603-100029-wait-semantic-5158b4-900/`
(`MACRUNNER_HB_TRACE_WAIT_SEMANTIC=1`, heartbeat, wait budget `200000`, `run_rc=143`,
`clean_rc=0`). Samples: 60s `blocks=0xba7b5 rva=0x184130` with HK at ~98% CPU; 300/600/900s
freeze exactly at `blocks=0x1a8465 rva=0x5158b4`, CPU 0.0. Wait counters are unchanged from
300s through 900s: `addr_wait_before=0`, `addr_wait_after=0`, `addr_wake=0`,
`wait_before=25`, `wait_after=18`, `event_signal=10`, `event_create=6`.

Disassembly maps `block_pc=0x87ef18c58b4` to `mono-2.0-bdwgc.dll` RVA `0x5158b4`, instruction
`mov %al,(%rbx)` inside a byte-copy/assertion formatting loop, not `WaitOnAddress`,
`WaitForSingleObject`, or any kernel wait thunk. The log then prints two Mono assertions:
`mini-runtime.c:4657` and `threads.c:723`, both `Type System.RuntimeType has invalid vtable method
slot 16 with method System.Reflection.MemberInfo:get_Name()`, followed by
`macrunner-hb-runtime-fail` in `mono-2.0-bdwgc.dll` at `rva=0x1f71a3`, `reason=JIT helper fault`,
`bytes=48 8b 07 ...`, `rdi=0`.

Unmatched waits are only seven `AssetGarbageCollectorHelper` worker semaphores
(`0x44,0x50,0x5c,0x68,0x74,0x80,0x8c`) from caller `0x87efcad7c92`; host sample
`sample-hoststack-after300.txt` shows those helper threads parked in
`macrunner_hb_try_kernel32_handle_semantic -> NtWaitForSingleObject -> server_wait`. These are idle
workers after the main Mono init path has faulted, not the thread at `0x5158b4`, and no
`SetEvent`/`ReleaseSemaphore` should target `0x5158b4` because there is no wait object there.
NEXT remains the existing root: Mono init does not reach the worker/render signal condition because
the RuntimeType vtable is already wrong. Prior branch proof points at Lane B-owned 16-bit
`TEST`/Jcc semantics (`testw %cx,%cx; jne` with `ecx=0x01010000`, low 16 bits zero) causing the
override store at `0xd4aa9` not to fire. Do not chase the wait layer for this gate.

**2026-06-03 operator-corrected Lane A checkpoint: old `0x14f180`/`~0x9456f` gate is cleared; new
gate is a hard park at `rva=0x5158b4`.** Fresh 900s samples from
`reports/phase4-hollow-knight/run-20260603-livelock-settle900-clean-head/samples.tsv` climb
`0xe1c53` at 60s to `0x1a841b` at 300s, then remain exactly `0x1a841b` at 600s and 900s with
process CPU sampled at `0.0%`. This is not the old Mono bsearch livelock and not slow throughput;
it is a deeper park/deadlock before graphics (`D3D11CreateDevice=0`, `GfxDevice=0`). NEXT: run HK
with `MACRUNNER_HB_TRACE_WAIT_SEMANTIC=1` plus heartbeat and identify the exact wait primitive
behind the last guest PC `rva=0x5158b4`: handle/event/WaitOnAddress address, whether any wake/signal
targets it, and which guest worker/render condition should signal it if no wake appears.

**2026-06-03 fresh 900s verdict (Codex Lane A): old `~0x9456f` plateau is cleared; current gate is
Mono RuntimeType vtable mismatch caused by 16-bit `TEST`/Jcc semantics.** Disk guard passed
(`137 GB >= 30 GB`). The stashed Lane A codegen WIP was restored only for validation, but it is NOT
build-green (`hb_test_runner`: `425 passed, 3 failed`), so it was restored to stash state and not
committed. Clean-source HyperBridge/`ntdll.so` rebuilt and the spike `ntdll.so` was reinstalled and
codesigned (`reports/phase4-hollow-knight/laneA-clean-source-rebuild-20260603-051152.log`, rc=0).

Requested 900s HK run:
`reports/phase4-hollow-knight/run-20260603-livelock-settle900-clean-head/`.
Samples: 60s `blocks=0xe1c53 rva=0x192049`; 300s `blocks=0x1a841b rva=0x5158b4`;
600s `blocks=0x1a841b rva=0x5158b4`; 900s `blocks=0x1a841b rva=0x5158b4`.
`D3D11CreateDevice=0`, `GfxDevice=0`, `CreateSwapChain=0`, `Present=0`; process CPU sampled `0.0%`
after the assertion. So this run definitively does NOT plateau at the old `~600k` bsearch count; it
passes that rung and then stops at the newer Mono assertion gate:
`Type System.RuntimeType has invalid vtable method slot 16 with method System.Reflection.MemberInfo:get_Name()`.

Loop/condition verdict: not a lost wake and not another-thread signaling. Prior exact branch proof
(`run-20260603-vtable-btjb-chain-interp-branchbudget-240`) shows Mono compares
`RuntimeType.System:get_Name` against inherited abstract `MemberInfo.System.Reflection:get_Name`.
Names, param counts, and return type match, but the override store at `0xd4aa9` never fires. The
engine falls through the `BT bit22` check correctly (`cf=0` for
`candidate_sig_flags ^ target_sig_flags = 0x01010000`), then wrongly takes the `jne` at `0xd4838`
after `testw %cx,%cx`. Since `ecx=0x01010000`, `cx == 0`, so `JNE` must fall through. The "value"
being re-tested is the low 16 bits of the signature flag xor; nobody should change it. The real gate
is wrong 16-bit `TEST` flag/condition semantics in the Lane B-owned decode/lift/interpreter path,
not more Mono bsearch/vtable helper optimization.

**2026-06-03 Lane A STOP checkpoint: long-run verdict is livelock, CMOVAE clears it, next root is
Lane B-owned 16-bit TEST semantics.** Decisive HK long run
`reports/phase4-hollow-knight/run-20260602-livelock-decisive-1800b/samples.tsv`:
60s `blocks=0x45a23 rva=0x53faec`; 300s `blocks=0x93eef rva=0x14f180`;
600s `blocks=0x93eef rva=0x14f180`; 900s `blocks=0x93eef rva=0x14f180`. So the old
`~0x94k` block count was a true fixed livelock, not throughput. The uncommitted Lane A CMOVAE
correction in `hb_arm64_codegen.c` moved HK past that plateau to `blocks=0x1a80c3`, then exposed
Mono's `RuntimeType` invalid-vtable assertion before graphics (`D3D11CreateDevice=0`,
`GfxDevice=0`).

Follow-up probes narrowed the new blocker. `RuntimeType.System:get_Name` and
`MemberInfo.System.Reflection:get_Name` have matching names, param counts, and identical string
return type pointers, but `mono_class_setup_vtable_general` advances through `0xd49ac` and never
stores at `0xd4aa9`, leaving inherited abstract slot 16. Broad forced interpreter ranges for
type-equality/vtable code did not fix it. Disassembly plus branch traces show the exact pair
falls through name compare, `BT bit22` (`cf=0`), and param-count checks, then incorrectly takes
`0xd4838` (`testw %cx,%cx; jne`) with `ecx=0x01010000`; low 16 bits are zero, so `JNE` should
fall through. This is evidence for wrong 16-bit `TEST` operand-size/flags semantics in the
decode/lift/interpreter-owned path, not a JIT-only or Mono-data issue. Lane A must not edit
`hb_decode_x64.c`, `hb_lift_x64.c`, or `hb_interpreter.c`; hand this exact root to Lane B.
Key proof run: `reports/phase4-hollow-knight/run-20260603-vtable-btjb-chain-interp-branchbudget-240/`.

**21:12 checkpoint (Codex Lane A restart): context-fix HK verdict = not exercised; no graphics
progress.** Re-read `CLAUDE.md` + `reports/research/LANE-A-MEGA-MISSION.md`. Reviewed dirty
`engine/hyperbridge/src/hb_arm64_codegen.c` plus dependent memory-region cache changes: the
`CMOVAE` bsearch semantic change is plausibly correct, but focused `make -C engine/hyperbridge
test` was not green (`424 passed, 3 failed`), so the WIP code was stashed as
`stash@{0}: Lane A WIP mono metadata codegen fast path` instead of committed. Rebuilt committed
HEAD `9892ba9` HyperBridge/`ntdll.so`; full ARM64EC build still has unrelated `appwiz.cpl`
`__alloca/___chkstk_ms` linker failures. Local relink required re-signing
`engine/wine/dist-arm64ec-spike` or Wine was SIGKILLed before HB startup.

Fresh HK run evidence: valid run is
`reports/phase4-hollow-knight/run-20260602-post-getcontext-head240-signed-abs/`, launched from the
extracted game dir so `Hollow Knight_Data` is adjacent. It timed out with `mr-run exit=143`,
`heartbeat=686`, `wait_semantic=73`, `D3D11CreateDevice=0`, `GfxDevice=0`, `CreateSwapChain=0`,
`Present=0`, `runtime_fail=0`, `jit_fallback=0`, and no `Killed: 9`. Final heartbeat:
`blocks=0x94025 steps=0x38c036 block_pc=0x87ef14ff180 rva=0x14f180`, still the known Mono metadata
hot region and not past the prior `0x9456f/0x96ae0` plateau. Critical context verdict:
`nt-get-context guest-x64=0` and `get-context import=0`, so HK did not exercise the new real guest
`GetThreadContext` path; no sane RIP/RSP/XMM sample can be claimed from this run. Wait traces are
normal `WaitForSingleObject*` calls/returns; the only `suspend` text is `CreateThread(... flags=0x4
suspended=1)` worker creation, not `SuspendThread`, so this is not a proven Mono GC stop-the-world
pending-suspend park. NEXT: do not chase GC `pending_suspends` unless `SuspendThread`/`NtGetContextThread`
starts appearing; continue bulk native promotion / profiling around the `rva=0x14f180` Mono metadata
chain, and keep a small suspend/GetContext trace enabled as a guard.

**10:56 checkpoint (Codex): starting the LANE A NEXT long-run experiment.** Plan: run Hollow
Knight 3x90s to warm the persistent translation cache with safe direct paths enabled
(`DIRECT_STACK=1`, `DIRECT_SCALAR_SCAN=1`, global `DIRECT_MEM=0` because the Unity comparator
fault at `rdx=0x10` is still proven unsafe), then run one 600s warm-cache profile. Report
heartbeat blocks/wallclock, cache hit-rate, and whether real `D3D11CreateDevice`/`GfxDevice`
appears or the heartbeat plateaus.

**11:19 checkpoint (Codex): long warm-cache result initially looked like a fixed plateau.**
Run root: `reports/phase4-hollow-knight/laneA-long-20260602/`. Warmups with safe direct
paths (`DIRECT_STACK=1`, `DIRECT_SCALAR_SCAN=1`, global `DIRECT_MEM=0`) reached the same
`600000` hot-block heartbeat in ~108-110s. Cache hit-rate improved from warm1
`9577/(9577+15236)=38.60%` to warm2/warm3 `~40.86%`; cache grew to ~2.9MB. The final
`long600-warm` run (`timeout=600`, wall `624s`, `run.rc=143`) still stopped at
`heartbeat_last=600000` (`~961.5 blocks/s` over wall), had cache `hits=10141`,
`misses=14678`, `stores=0`, hit-rate `40.86%`, and no real `D3D11CreateDevice` or
`GfxDevice` markers. `macrunner-hb-runtime-fail=0`, `macrunner-hb-jit-fallback=0`,
`JIT codegen failed=0`. Follow-up below checks whether this is a true wait/sync gate or
just the JIT hot-block aggregate flattening while Mono still burns CPU.

**11:47 checkpoint (Codex): follow-up verdict = throughput in Mono metadata, not a wait gate.**
`run-20260602-gate-wait180-small` and `run-20260602-heartbeat-filter240` kept safe direct
paths (`DIRECT_STACK=1`, `DIRECT_SCALAR_SCAN=1`) and global `DIRECT_MEM=0`. The 180s
wait probe had `wait_semantic_count=73`; waits release normally, ending with
`ReleaseSemaphore(handle=0xa4) status=0`, and HK stayed `~99-100%` CPU through timeout.
The filtered 240s heartbeat run avoided raw-log prune and shows real execution still moving:
`heartbeat_last blocks=0x96ae0 steps=0x3982f2 block_pc=0x87ef14ff180 rva=0x14f180`.
That PC is the known `mono-metadata-bsearch` helper/fusion region, not a parked worker.
Still no `D3D11CreateDevice`, `GfxDevice`, or `CreateSwapChain`; runtime/JIT fallback/codegen
fail counts remain zero. Correct NEXT: keep AOT cache on, keep safe direct paths on, do not
flip global `DIRECT_MEM` until the Unity comparator fault class is proven safe, and continue
bulk native-promotion around Mono metadata bsearch/decode-row/rowptr/string hot paths until
Mono init reaches graphics creation.

**12:08 checkpoint (Codex): Mono metadata bsearch helper tightened; correctness green, no app-level
speed win yet.** In `hb_arm64_codegen.c`, `hb_jit_helper_exec_mono_metadata_bsearch_loop()` now
uses one host-span read for the contiguous metadata node fields (`node+16` qword and `node+28`
dword) and caches the metadata pointer-table host span for in-span table entries, while preserving
the old ordered guest-read fallback for boundary/fault cases. Validation: `make` + `hb_test_runner`
-> `419 passed, 0 failed`; `hb_fuzz_diff.py --cases 10000 --batch 1024 --families
shift_rotate_flags` -> backend/oracle mismatches `0`; spike `ntdll.so` was forcibly relinked against
the new `libhyperbridge.a`, installed, and codesigned. HK probes:
`run-20260602-mono-bsearch-nodefast-heartbeat240` and
`run-20260602-mono-bsearch-tablefast-heartbeat240` stayed clean (`runtime_fail=0`,
`jit_fallback=0`, `JIT codegen failed=0`) but still had `D3D11CreateDevice=0`, `GfxDevice=0`,
`CreateSwapChain=0`. Final table-span run ended at `heartbeat_last blocks=0x9456f`
(`607599` decimal) / `steps=0x38c21d`, still at `rva=0x14f180` with HK `~99-100%` CPU and waits
releasing normally. This helper-only micro-opt is correctness-safe but not sufficient; NEXT should
move to a broader native promotion of the adjacent Mono metadata call chain or direct-memory safety
proofing, not more isolated reads inside this helper. Parallel ISA coverage refreshed:
`x64_isa_coverage.py --random 1000` passed with capstone-valid decode misses empty; see
`reports/research/HB-X64-ISA-COVERAGE-matrix.md`.

## ⚡ LANE A SPEED — turn ON the unused accelerators (operator 2026-06-02) — biggest lever
Post-gate the main thread is throughput-bound in Mono managed-init (heartbeat ~705K blocks and
climbing, no D3D11CreateDevice yet). Warming one hot loop per run is too slow. An audit found TWO
big accelerators that EXIST but are NOT engaged in the Hollow Knight runs:
1. **AOT / persistent translation cache is NOT used in HK runs.** `hb_aot_cache.c` exists and
   `engine/hyperbridge/build/hyperbridge-cache/translation-cache.bin` is present but EMPTY (16 bytes);
   `scripts/mr-run.sh` sets no AOT/translation-cache env, and the HK trace shows zero cache hit/miss
   markers. So every run re-JITs the entire Mono init from scratch — millions of managed-code
   instructions recompiled each launch. **TURN IT ON:** wire the persistent translation cache into
   the spike/HK run path so a first run warms it and subsequent runs start warm (cache-hit the Mono
   init). This is the single biggest startup speedup. Verify with cache hit counts climbing run-over-
   run and the warm-start heartbeat reaching far more blocks in the same wall-clock.
2. **`MACRUNNER_HB_JIT_DIRECT_MEM=0`** (off). JIT memory ops go through helper calls instead of
   direct loads/stores. Enable it for the proven-safe paths (you already trust DIRECT_STACK=1) and
   measure — direct memory is a large per-access win on the hot Mono loops.
3. **Bulk-warm instead of one-loop-per-run:** do ONE profiling run, collect ALL hot blocks at once
   (you already log `jit-hot-block` ranks), and native-promote them in a batch — don't discover one
   loop per launch.
NOTE: there is NO public "map of hot JIT paths" — hot paths are unique to this app+translator. The
levers are architectural (AOT cache, block chaining, direct-mem, bulk promotion), and #1+#2 above
are currently OFF. Engage them first; that is the fastest route to finishing Mono init → main →
GfxDevice → window. Keep correctness (the shift/SETcc JIT flag fix below) intact.

**06:55 checkpoint (Codex): persistent translation cache is now wired into the Hollow Knight spike
path; direct-mem global enable is unsafe.** Runtime changes: `hb_jit_runtime_run()` now opens the
persistent translation cache from `MACRUNNER_HB_TRANSLATION_CACHE_ROOT`, keys blocks by guest bytes +
mode/backend/direct-mem/direct-stack flags, loads helper-free native blobs into the current executable
JIT buffer, stores helper-free compiled blobs back to disk, and prints cache hit/miss/store summaries.
`hb_aot_cache.c` now keeps the cache file indexed in memory per process instead of rereading/scanning
`translation-cache.bin` on every lookup/store. `scripts/mr-run.sh` wires the spike path to
`engine/hyperbridge/build/hyperbridge-cache` with cache tracing on by default.

Validation: `make && ./tests/hb_test_runner` -> `419 passed, 0 failed`;
`hb_fuzz_diff.py --cases 10000 --batch 1024 --families shift_rotate_flags` ->
`backend_mismatch_count=0`, `oracle_mismatch_count=0`; targeted ntdll relink/install succeeded.
Cold/warm HK measurement with direct-mem OFF:
- `run-20260602-cache-index-cold90`: clean (`runtime_fail=0`, `jit_fallback=0`), cache summary
  `hits=1731 misses=23058 stores=5572 store_skips=17486`, hot-block total reached `600000` by 90s,
  cache stats PASS with `8599` entries / `1323268` bytes.
- `run-20260602-cache-index-warm90`: clean, cache summary
  `hits=6874 misses=17945 stores=429 store_skips=17516`, hot-block total again `600000` by 90s,
  cache stats PASS with `8601` entries / `1323536` bytes.

Direct-mem measurement: global `MACRUNNER_HB_JIT_DIRECT_MEM=1` is still not safe. Cold run
`run-20260602-cache-cold90` reproduced the known UnityPlayer RVA `0x649910` failure:
`out=MEMORY_FAULT`, `rdx=0x10`, no JIT fallback/codegen failure. The runner default is therefore
kept at `MACRUNNER_HB_JIT_DIRECT_MEM=0`; direct-mem must be narrowed to proven-safe ranges/forms
before becoming default.

**07:55 checkpoint (Codex): two safe accelerator batches landed, but Mono init is still
throughput-bound.** Added `MACRUNNER_HB_JIT_DIRECT_SCALAR_SCAN=1` as a narrow direct-memory
surface for existing helper-free scalar scan loops; global `MACRUNNER_HB_JIT_DIRECT_MEM` remains
OFF after the Unity `0x649910` fault. Validation stayed clean:
`make && ./tests/hb_test_runner` -> `419 passed, 0 failed`; `hb_fuzz_diff.py --cases 10000
--batch 1024 --families shift_rotate_flags` -> backend/oracle mismatches `0`.

Measurement:
- Baseline indexed-cache warm run was `run-20260602-cache-index-warm90`: cache `hits=6874`,
  `misses=17945`, `stores=429`, `store_skips=17516`, heartbeat `600000` blocks/90s.
- Scalar-scan warm profile `run-20260602-scalar-scan-profile90`: cache `hits=7303`,
  `misses=17516`, `stores=0`, `store_skips=17516`, heartbeat still `600000` blocks/90s.
  The previous Mono string-scan hot rank disappeared, so the path works, but it is not enough.
- Helper-stub cache v9 canonicalization landed for conservative single-block helper stubs
  (`x1=current block`, `x23=allowlisted helper`, one `blr x23`; generic per-instruction helpers
  still skipped). Warm run `run-20260602-helpercache-warm90`: cache `hits=7235`, `misses=17554`,
  `stores=449`, `store_skips=17105`, heartbeat still `600000` blocks/90s. Clean run:
  `runtime_fail=0`, `jit_fallback=0`, `JIT codegen failed=0`, `D3D11CreateDevice=0`,
  `GfxDevice=0`.

**08:30 correction (Codex): do not chase the `0x4e3440/0x4e3727` pair as a new family.** A targeted
IR capture (`run-20260602-ir-mono-4e3440`) proved Mono RVA `0x4e3440` is a range guard
(`LOAD r14,[r13]; CMP r14,rdx; JB 0x4e3727`) and the existing runtime already promotes the pair:
`macrunner-hb-jit-fusion: kind=bounded-byte-scan guard=0x87ef1893727 body=0x87ef1893440`.
A speculative linked-list promoter was removed after a no-fire profile; validation after removal:
`make && ./tests/hb_test_runner` -> `419 passed, 0 failed`;
`hb_fuzz_diff.py --cases 10000 --batch 1024 --families shift_rotate_flags` -> backend/oracle
mismatches `0`; targeted ntdll relink/install succeeded.

**08:45 checkpoint (Codex): cache warm-start is real but not yet enough; do not add more Mono
loop warmers blindly.** Same-build warm run `run-20260602-current-warm2-90` was clean
(`runtime_fail=0`, `jit_fallback=0`, `JIT codegen failed=0`) with cache summary `hits=7235`,
`misses=17578`, `stores=449`, `store_skips=17129`; heartbeat stayed `600000` blocks/90s and no
real `D3D11CreateDevice`/`GfxDevice` appeared. A second targeted IR capture
(`run-20260602-ir-mono-6d920`) proved Mono `0x6d920/0x6d926` is also already handled by the
existing bounded-byte-scan fusion (`guard=0x87ef141d920`, `body=0x87ef141d926`). The next issue is
why these promoted/fused blocks still dominate hot ranks after cache replay.

NEXT: investigate fused-loop persistence/accounting before writing any new Mono loop promoter. Current
top real work is Unity `0x649910` (rank #1; global direct-mem faulted there with `rdx=0x10`) plus the
Unity sort/comparator `0x283xxx` family. Focus on helper-free/native implementations or precise safe
direct-memory guards for those families, then rerun a warm 90s profile and check real
`D3D11CreateDevice`/`GfxDevice`. Keep global direct-mem OFF; only enable direct memory under specific
proven-safe emitters.

**09:45 checkpoint (Codex): persistent cache now covers the x1-instruction helper family and warm
replays many more Mono-init stubs, but execution is still helper-bound.** Runtime promotion now runs
after persistent-cache hits too, so already-known bounded scan fusions are rebuilt on warm start; the
old hot `0x4e3440/0x6d920` bounded-scan pair disappeared from the warm top ranks. Cache canonicalizer
format `v11` now supports single-instruction helper stubs (`x1=&block->instrs[i]`, `x23=allowlisted
helper`) by storing an instruction-index sentinel and patching it back to the current block on load.
Validation stayed clean: `make && ./tests/hb_test_runner` -> `419 passed, 0 failed`;
`hb_fuzz_diff.py --cases 10000 --batch 1024 --families shift_rotate_flags` -> backend/oracle
mismatches `0`; targeted ntdll relink/install succeeded.

Measurement:
- v9 warm (`run-20260602-current-warm2-90`): cache `hits=7235`, `misses=17578`, `stores=449`,
  `store_skips=17129`, heartbeat `600000` blocks/90s.
- v11 cold fill (`run-20260602-cache-v11-cold90`): cache `hits=2351`, `misses=22456`,
  `stores=7785`, `store_skips=14670`, heartbeat `600000` blocks/90s.
- v11 warm (`run-20260602-cache-v11-warm90`): cache `hits=9575`, `misses=15232`, `stores=562`,
  `store_skips=14670`, heartbeat `600000` blocks/90s. Clean run: `runtime_fail=0`,
  `jit_fallback=0`, `JIT codegen failed=0`, `D3D11CreateDevice=0`, `GfxDevice=0`.

NEXT: persistent startup cache is materially better, but it did not move the 90s heartbeat yet. Work is
now inside the helper-collapsed hot blocks: Unity pointer comparator `0x649910` / sort `0x282860`,
Mono metadata helpers `0x183a10/0x183e00/0x184130`, and Mono string helpers `0x59870/0x59830`.
Do not enable raw global direct-mem: Unity `0x649910` already proved invalid pointers are possible.
Next safe accelerator is host-span fast paths inside the C helpers (validated memory range once, then
loop over host bytes) or a guarded native path that falls back before any unsafe load.

**10:20 checkpoint (Codex): helper read fast paths are safe but not a visible throughput lever yet.**
Added validated host-span fast reads inside the Unity `u32_ptr_compare` helper and Mono string/metadata
helpers. The fast path only uses direct host bytes after `hb_memory_find_region` proves a readable
host-backed span; invalid/unbacked pointers still fall back to existing `hb_memory_read_*` /
`hb_jit_read_guest_u64_result` semantics. Validation stayed clean (`419 passed, 0 failed`; 10k
`shift_rotate_flags` backend/oracle mismatches `0`; ntdll relink/install succeeded). Measurements:
`run-20260602-fastreads-refill90` clean refill cache `hits=2345`, `misses=22444`, `stores=7785`,
`store_skips=14658`; `run-20260602-fastreads-warm90` clean warm cache `hits=9569`, `misses=15220`,
`stores=562`, `store_skips=14658`. Heartbeat still `600000` blocks/90s, no
`D3D11CreateDevice`/`GfxDevice`.

NEXT: take a finer 50K-interval heartbeat to see whether the 200K snapshots hide a small speed delta.
If still flat, stop optimizing helper read internals and move to the Unity comparator/sort control-flow
shape: `0x649910` remains rank #1, while `0x282860/0x282876` repeatedly call it. A guarded native
comparator path must check pointer validity before loads and fall back to the C helper for invalid
pointers (`rdx=0x10` was observed under global direct-mem).

**10:35 checkpoint (Codex): finer heartbeat confirms no hidden speed delta.**
`run-20260602-fastreads-warm50k90` was clean with cache `hits=10139`, `misses=14674`, `stores=0`,
`store_skips=14674`; 50K heartbeat ticks ended at `600000` (`250000..600000` tail), so the 200K
interval was not hiding a near-`800000` run. Still no `D3D11CreateDevice`/`GfxDevice`.

NEXT: stop helper-read micro-optimizing. Target the Unity sort/comparator control-flow family in bulk:
`0x282860/0x282876` around the indirect comparator call and `0x649910` comparator itself. Keep raw
global direct-mem OFF. Any native comparator/sort path must guard pointer validity before loading and
fall back to the C helper on invalid/unbacked pointers.

## 🐞 LANE A — JIT CORRECTNESS BUG found by Lane B fuzzer (operator 2026-06-01, do alongside gate)
Lane B's new differential fuzzer (`engine/hyperbridge/tests/hb_fuzz_diff.py`, merged) ran 1M cases:
interpreter semantics are CLEAN (exact-oracle mismatches=0), but there are **43,478 interpreter-vs-
JIT mismatches, ALL in the `shift_rotate_flags` family**, minimized to `SETNE r8b` (`41 0f 95 c0`).
That means the **JIT codegen (`hb_arm64_codegen.c`, Lane A's file) computes wrong FLAGS** for
shift/rotate and/or SETcc — a silent correctness bug that would corrupt games unpredictably. This
is higher-severity than missing speed: fix it. Repro: `cd engine/hyperbridge && python3
tests/hb_fuzz_diff.py --cases 200000 --batch 4096` and look at `shift_rotate_flags` backend
mismatches; or run the single case `41 0f 95 c0` through interp vs JIT and diff EFLAGS. Fix the
flag materialization in the JIT shift/rotate/SETcc path so JIT == interpreter, then re-run the
fuzzer to confirm `backend mismatches=0`. (Lane B owns the fuzzer + interpreter; the JIT FIX is
yours.)

**20:53 checkpoint (Codex): JIT correctness bug fixed.** Root cause was not interpreter flags:
`SETcc r/m8` fast JIT helper wrote the whole destination GPR. For `SETNE r8b`, interpreter preserved
upper bits (`0x2aa9b3c6758e4400`) while JIT zeroed all of `r8`. `hb_arm64_codegen.c`
`hb_jit_helper_exec_setcc_lazy()` now writes `HB_SIZE_8` via `hb_context_write_reg_value_sized()`.
Validation:
- `cd engine/hyperbridge && python3 tests/hb_fuzz_diff.py --cases 200000 --batch 4096`: rc=0,
  backend mismatches=0, oracle mismatches=0.
- `cd engine/hyperbridge && python3 tests/hb_fuzz_diff.py --cases 1000000 --batch 4096`: rc=0,
  backend mismatches=0, oracle mismatches=0, oracle checked/pass=608,698/608,698.
- `cd engine/hyperbridge && make && ./tests/hb_test_runner`: rc=0, 416 passed, 0 failed.
**21:50 checkpoint (Codex): graphics gate moved earlier to prefix/bootstrap setup.** Added local
`GetFileInformationByHandle` for synthetic local file handles and a local
`SystemTimeToTzSpecificLocalTime` / `TzSpecificLocalTimeToSystemTime` semantic family in
`macrunner_hb.c`; targeted ntdll rebuild/install succeeded. Added direct ARM64 JIT for register
`BT/BTS/BTR/BTC` after `kernelbase!UrlIsOpaqueA` looked suspicious; validation:
`bt_family` 10k backend=0/oracle=0 and `shift_rotate_flags` 10k backend=0/oracle=0.
However the current DXMT no-log gate run still does not reach Unity/D3D: fresh throwaway prefixes
spend the full timeout in auto `wineboot.exe --init` /
`rundll32.exe setupapi,InstallHinfSection PreInstall ... wine.inf`
(`run-20260601-bit-test-jit-nolog240`, `D3D11CreateDevice=0`, `GfxDevice=0`, heartbeat only
`rva=0x4f960`). A focused JIT trace (`run-20260601-setupapi-hotblock-ir90`) proves that hot block
compiles (`macrunner-hb-jit-fallback=0`, `match=1`); it is a prologue/load/test/jcc block, not the
BT instruction. NEXT: diagnose/fix the prefix bootstrap gate at the root: either make `mr-run.sh` /
`sync-prefix-from-dist.sh` seed an already-initialized disposable prefix, or find the missing
Win32/setupapi semantic that makes `wine.inf` PreInstall never complete. Do not return to Unity wait
diagnosis until this fresh-prefix setup path is cleared; do not treat loop warming as the window path.

**22:08 checkpoint (Codex): fresh-prefix setup gate cleared in the runner.** `scripts/mr-run.sh`
now seeds DXMT throwaway prefixes from the newest validated `artifacts/warm-prefix/*` registry
template before `sync-prefix-from-dist.sh`, and stamps the disposable `.update-timestamp` to the
current dist `wine.inf` mtime on the default skip-wineboot path. Validation run
`run-20260601-warm-prefix-gate90`: `wineboot=0`, `setupapi,InstallHinfSection=0`, no
`rundll32.exe`/`wineboot.exe` spin in 15/45/75s process samples, cleanup rc=0. The app is now back
inside Hollow Knight/Unity (`last heartbeat rva=0x5544c1`) but still has `D3D11CreateDevice=0`,
`GfxDevice=0`, `CreateSwapChain=0`, `jit_fallback=0`. NEXT: resume the real Unity gate diagnosis
from the post-prefix state: identify the wait/worker object around the current Unity RVAs (including
the prior `0xcba8b2` zero-timeout poll and worker parks) and fix the missing sync semantic at root;
do not warm loops as the path to graphics.

**22:15 checkpoint (Codex): post-prefix wait trace reclassifies the current blocker as CPU progress,
not a live wait gate.** `run-20260601-post-prefix-waittrace240` still has `D3D11CreateDevice=0` /
`GfxDevice=0`, but the wait evidence is healthy: `0xcba8b2` is still
`WaitForSingleObject(GetCurrentProcess(), 0)` returning `WAIT_TIMEOUT`; `0x577f44` startup waits are
released by matching `ReleaseSemaphore` calls from RVA `0x14d1f30`; `0x577c92` workers park on idle
semaphores. Process samples at 60/120/180/240s show the Hollow Knight process active at
~99-100% CPU, not parked; hot heartbeats are `0x649910`, `0x283d25/0x283876`, `0x19d439d`, etc.,
with `jit_fallback=0`. NEXT: run a clean 900s post-prefix speed-vs-gate check (low trace) to verify
whether progress eventually reaches real `D3D11CreateDevice`; if it stays CPU-bound with moving
heartbeats, resume evidence-targeted JIT/codegen hot-family work, not sync patching.

**22:33 checkpoint (Codex): post-prefix 900s verdict is CPU/JIT-native, not wait-gate.** Verdict file:
`reports/research/HB-GRAPHICS-speed-vs-gate-verdict-20260601-post-prefix.md`. Run
`run-20260601-post-prefix-speed-vs-gate900` stayed clean (`wineboot=0`,
`setupapi,InstallHinfSection=0`, `jit_fallback=0`, `JIT codegen failed=0`) and still had
`D3D11CreateDevice=0` / `GfxDevice=0`. Heartbeats froze after the 60s snapshot at Unity RVA
`0x5544b8` while the real `Hollow Knight.exe` process stayed ~99-100% CPU through 900s; live macOS
sample mapped the active PC to anonymous executable VM (`0x87fff960000-0x87fffa60000`, sampled around
`0x87fff9ce678`). NEXT: run a focused JIT hot-block/native-address trace to map that anonymous PC
back to the guest block, then fix or specialize that finite hot block family. Do not patch
wait/event semantics without new wait evidence.

**22:55 checkpoint (Codex): anonymous CPU loop mapped to Wine ntdll timerqueue assert.** Focused
native-address runs mapped sampled PC `0x87fff9ce678` outside the HB block cache; live disassembly
showed an ARM64 fatal/assert loop. Decoding the ADRP string operands identified
`"../dlls/ntdll/threadpool.c"`, line `569`, assertion `"t->destroy"` in `queue_remove_timer()`.
This is not a Unity wait object and not an HB JIT fallback. NEXT: instrument the old timer queue path
(`RtlCreateTimer*`/`RtlUpdateTimer`/`RtlDeleteTimer*`, `queue_add/move/destroy/remove`) to find how a
timer reaches `queue_remove_timer()` without the destroy invariant, then fix the root cross-arch
timer/callback/sync semantic. Do not relax the assert as a workaround.

**23:54 checkpoint (Codex): timerqueue assert hypothesis is not current-reproducible.** Rebuilt with
the old timerqueue probes active in `threadpool.c`, then ran `run-20260601-timerqueue-trace120` and
`run-20260601-timerqueue-trace300`; both still had `D3D11CreateDevice=0` / `GfxDevice=0`, but
`macrunner-hb-timerqueue=0`, `threadpool.c=0`, and `t->destroy=0`. Fresh live samples
`run-20260601-current-generated-pc120b` and `run-20260601-current-assert-wide100` repeat the same
anonymous ntdll-generated CPU loop at `0x87fff9d0e4c`, plus lldb catches a native ARM64 PE
`EXC_BAD_ACCESS address=0x60` at `ldr x8, [x18,#0x60]` (`0x87fff9bb040`). Source already has an
Apple `x18`/TEB self-heal in `unix/signal_arm64.c`, so NEXT is evidence-first: determine whether
that lldb x18 stop is the real unhandled root or only debugger interception of a recoverable signal,
then map/fix the ntdll-generated fatal loop. Do not patch sync/timer semantics without a firing trace.

## LANE SYNC POINT (2026-06-01, operator) — both lanes reconciled into HEAD `6a2b855`
Both lanes were stopped, all Lane B ISA work (batches 06-01..06-05) merged into main, kit refreshed
to this base. Build rc=0, runner 395/0, fast-family PASS. Now both resume from this clean base:
- **Lane A (this main mac):** GRAPHICS / DXMT (below). JIT codegen is yours. Do NOT edit
  `hb_decode_x64.c` / `hb_lift_x64.c` / `hb_interpreter.c` vector semantics — Lane B owns those.
- **Lane B (Air/kit):** finishing ISA semantics (full AVX2 packed, 0F3A AES/string, MMX, EVEX) in
  `_air-bulk-isa-kit/` per its AGENTS.md. Returns bundles via disk; operator reconciles (~daily).

## ⚠️ NEXT (operator-directed 2026-06-01 14:40, resolved 15:00) — GATE, not speed
The required speed-vs-gate experiment is complete:
`reports/phase4-hollow-knight/run-20260601-hk-dxmt-speed-vs-gate900/` and
`reports/research/HB-GRAPHICS-speed-vs-gate-verdict-20260601.md`.

Verdict: the WINDOW blocker is a GATE, not throughput. In the 900s Hollow Knight DXMT run, real
`D3D11CreateDevice=0`, `GfxDevice=0`, `CreateSwapChain=0`, and progress plateaued from
the first 60s sample through timeout at the same heartbeat (`blocks=56205`, `steps=1e7b0e`,
`rva=0x4e7e60`, trace bytes unchanged except the timeout line). Wait callers map to Unity
RVA `0xcba8b2` zero-timeout poll plus worker parks at `0x577c92/0x577f44`; CPU sample showed
Hollow Knight/wineserver/services at `0.0%` (parked, not slow-translating).

**Clarification on the JIT loop-warming (operator 2026-06-01):** the JIT-warming work is NOT
wasted — every promoted hot block is a permanent engine speedup that ALL games on this engine
reuse, and full JIT coverage will be needed anyway for playable FPS once the window opens. The
point is only about PRIORITY/SEQUENCING: warming loops cannot, by itself, OPEN the window, because
the main thread is parked on a wait that never signals (no amount of speed helps a sleeping thread).
So:
- **Priority #1 = fix the GATE** (this is the only thing that makes `GfxDevice` appear).
- **JIT-warming continues as valid BACKGROUND work** — keep the promotions you've made, and you
  MAY keep promoting genuinely-hot blocks opportunistically; just don't treat loop-warming as the
  path to the window. Don't expect the window from it; expect FPS from it (later).

NEXT action: diagnose the Unity main-thread gate at RVA `0xcba8b2`. Find the handle/event/timer
or init condition it polls, correlate with the worker waits at `0x577c92/0x577f44`, and fix the
missing semantic/thread state at the root. THAT is what makes the window appear — not more loop
optimization.

**17:40 checkpoint (Codex):** re-anchored the wait graph and confirmed the earlier focused trace:
`0xcba8b2` is `WaitForSingleObject(GetCurrentProcess(), 0)` and correctly returns
`WAIT_TIMEOUT`; `0x577f44` startup semaphores signal; `0x577c92` workers are parked on idle work
semaphores. Static IAT mapping shows the nearby Unity wake sites (`0xcba981`/siblings) are
`api-ms-win-core-synch-l1-2-0.dll!WakeByAddressSingle`, so the address-wait family is a real
sync-semantic gap candidate. Unix ntdll now has a local `WaitOnAddress`/`WakeByAddress*` semantic
mirroring Wine's `RtlWaitOnAddress` keyed waiter queues instead of delegating back through the
forwarded PE/API-set target; targeted `ntdll.so` rebuild/install succeeded. Validation runs
`run-20260601-sync-address-local-wait180` and `run-20260601-sync-address-prewait120` still have
real `D3D11CreateDevice=0`, but currently time out earlier than the prior Unity wait traces:
XTAJIT ProcessInit/ThreadInit completes, then no `BeginSimulation`, no wait-semantic line, and no
heartbeat. NEXT: diagnose this pre-entry loader/startup park first (with XTAJIT/IAT traces and
thread wait sampling), then rerun the full Hollow Knight DXMT gate check for real `D3D11CreateDevice`.
Do not resume loop warming for this gate.

**15:30 checkpoint (Codex):** focused wait/thread trace shows `0xcba8b2` returns the expected
`WAIT_TIMEOUT` for `WaitForSingleObject(GetCurrentProcess(), 0)` and `0x577c92` is the idle work
semaphore wait for seven `AssetGarbageCollectorHelper` workers; `0x577f44` startup handshakes are
signaled. A root sync gap was found and patched in HyperBridge: `CRITICAL_SECTION` imports no
longer fake owner/recursion state and now use interlocked acquisition plus real NT semaphore wakeups
in the Unix ntdll semantic path. Targeted `ntdll.so` rebuild/install succeeded. NEXT: run a clean
full Hollow Knight DXMT gate check (no sampling wrapper) and, if `D3D11CreateDevice` is still zero,
continue from producer/main-thread state rather than loop warming.

**16:25 checkpoint (Codex):** the wait gate diagnosis is resolved, and a second root bug was fixed.
The `0xcba8b2` process-handle poll is not the missing signal, `0x577f44` startup semaphores signal,
and `0x577c92` is the worker idle-work semaphore wait. Post-critical-section runs exposed the real
pre-D3D killer: ARM64 PE calls executed on the HyperBridge stack while TEB stack bounds were merged
across Wine's native stack and the bridge stack, creating a fake contiguous range with an unmapped
gap. Unity/Mono stack probing then faulted just below the bridge stack low
(`run-20260601-154911-hk-dxmt-bridge-stack-faultfix360`, `fault=bridge_low-0x280`). Fix:
`macrunner_hb_call_arm64_pe_import12` now publishes only the active bridge stack in
`Tib.StackLimit`/`Tib.StackBase` and `DeallocationStack` during native PE calls, and
`macrunner_hb_run_x64` publishes/restores bridge `DeallocationStack`; ARM64 signal handling also
routes bridge-stack faults to `virtual_handle_fault` with a bridge-stack pointer. Targeted
`ntdll.so` rebuild/install: `build-20260601-160858-ntdll-bridge-stack-teb-range.log` rc=0.
Validation: `run-20260601-160925-hk-dxmt-bridge-teb-range360` timed out cleanly with
`bus_low_stack=0`, `virtual_nested=0`, `runtime_fail=0`, `MEMORY_FAULT=0`, JIT fallback/codegen
failures `0`, and `scripts/mr-clean.sh --prune` clean. Real `D3D11CreateDevice=0`, `GfxDevice=0`,
`CreateSwapChain=0`; no main open wait remains in this trace. NEXT: graphics still has no device
marker; continue gate diagnosis from the clean pre-D3D state after the final native import burst
(`VirtualQuery`/`VirtualAlloc`/`DuplicateHandle`) and Mono page-table loop at Mono RVA `0x4e7e98`.
Do not resume generic loop warming unless the operator reclassifies this as throughput again.

**16:33 checkpoint (Codex):** focused memory-import trace
`run-20260601-162254-hk-dxmt-memory-import360` preserved the clean stack/fault state
(`bus_low_stack=0`, `virtual_nested=0`, `runtime_fail=0`, `MEMORY_FAULT=0`, JIT fallback `0`) and
still had `D3D11CreateDevice=0`. The final memory calls feeding the Mono loop are normal-sized
alloc/query calls, not an obvious bad-range gate: repeated `VirtualAlloc(..., size=0x40000,
MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE)` chunks at `0x11fb80000`/`0x11fbc0000`/`0x11fc00000` and
`0x13b300000..0x13b380000`, plus `VirtualQuery(stack=0x11624d000, size=0x30) -> 0x30`. Last
heartbeat remains Mono RVA `0x4e7e98` in the page-table fill loop with no open main wait. NEXT:
the original wait/fault gate is fixed; current evidence again looks like pre-D3D CPU progress in
Mono memory bookkeeping, but generic loop warming is still held by the latest operator instruction.
If allowed to reclassify, resume targeted JIT/throughput on the finite hot Mono/Unity loops; if not,
add narrower semantic probes around Mono memory bookkeeping ownership and graphics-init transition.

**Latest checkpoint (Codex): Mono assembly-load gate cleared; resume from post-mscorlib Unity init.**
Two root cross-arch file/runtime bugs were fixed in Unix ntdll. First, ARM64X kernelbase `.hexpthk`
stubs that look like x64 bytes now redirect SIGILL to the real ARM64 target; validation
`run-20260601-arm64x-hexpthk-route120` had `.hexpthk` redirects and no invalid-disposition loop.
Second, synthetic HyperBridge local file handles now implement the mapping/info family needed by
Mono (`GetFileInformationByHandleEx`, `CreateFileMappingW`, `MapViewOfFile`, `UnmapViewOfFile`,
mapping `CloseHandle`). This fixes the `mscorlib.dll` load path at root: original game data only
(`Hollow Knight_Data/Managed/mscorlib.dll`, no diagnostic `Managed/mono/*` copy) maps successfully
through `CreateFileMappingW`/`MapViewOfFile`. Validation:
`run-20260601-mapview-original-data180` partial low-trace run (tool timeout, then scoped
`mr-clean.sh --prune`) shows `mscorlib_missing=0`, `invalid_cil=0`, `mapped=1`, `sig=0`,
`jit_fallback=0`, and still `D3D11CreateDevice=0` / `GfxDevice=0` in the captured window. NEXT:
run a clean low-trace Hollow Knight DXMT gate check from this post-mscorlib state; if D3D11 is still
absent, continue evidence-first on the next Mono/Unity initialization gate after the assembly-map
success, not generic loop warming.

**Current checkpoint (Codex, 2026-06-02 03:11 +10): JIT correctness and code-cache fatal are
cleared; graphics still has no device marker.** The Lane B fuzzer bug was fixed in
`hb_arm64_codegen.c`: `SETcc r/m8` now writes only the byte destination via
`hb_context_write_reg_value_sized(..., HB_SIZE_8)`, preserving upper register bits for cases like
`SETNE r8b`. Fresh validation after the Mono helper changes:
`cd engine/hyperbridge && python3 tests/hb_fuzz_diff.py --cases 10000 --batch 1024 --families
shift_rotate_flags` -> `backend_mismatch_count=0`, `oracle_mismatch_count=0`,
`oracle_pass_count=10000/10000`.

The post-mscorlib `JIT buffer exhausted` fatal is also converted to a fail-open interpreter fallback:
the block cache is expanded to 65,536 entries, occupancy is tracked, and full code cache / block table
conditions no longer surface as `OUT_OF_MEMORY`. `run-20260601-jit-cache-failopen-dxmt360` had
`runtime_fail=0`, `jit_buffer_exhausted=0`, `jit_code_cache_full=0`, `jit_fallback=0`, but still
`D3D11CreateDevice=0` / `GfxDevice=0`. A targeted Mono metadata bsearch helper for
`mono-2.0-bdwgc.dll` RVA `0x14f180` is now linked and active; the first semantic bug in its CMOV flag
handling was repaired, and `run-20260601-mono-bsearch-helper-flagsfix-hot180` finished with
`runtime_fail=0`, `mono_assert=0`, `mono_bsearch_fusion=2`, `top_14f180=0`, and still no D3D/Gfx
marker.

NEXT: continue evidence-first from the clean post-helper state. The current hot set moved to
`mono-2.0-bdwgc.dll` around guest PCs `0x87ef1533eeb`, `0x87ef1533ed5`, `0x87ef1533ec0` plus
UnityPlayer `0x87efcba9910`. Dump IR for the top Mono hot family, decide whether it is a finite
helper/codegen coverage gap or a real semantic gate, and rerun Hollow Knight DXMT checking for a real
`D3D11CreateDevice`. Keep direct memory disabled: `run-20260601-directmem-probe360` faulted at
UnityPlayer `0x649910`.

**03:52 checkpoint (Codex): two Mono metadata decode helpers added and validated.** Evidence showed
the `0x183e*` hot cluster is Mono `metadata.c` row decode, not a wait or graphics API gate. Added
JIT helpers in `hb_arm64_codegen.c` for the valid fast paths:
`mono-metadata-decode-row` (`0x183eb4` loop entry) and `mono-metadata-decode-col`
(`0x184130` function entry, including return-address pop). Validation:
`make && ./tests/hb_test_runner` -> 419/0; `hb_fuzz_diff.py --cases 10000 --batch 1024 --families
shift_rotate_flags` -> backend/oracle mismatches 0; forced `ntdll.so` relink/install has both helper
strings. App runs `run-20260602-mono-decode-row-helper-hot180` and
`run-20260602-mono-decode-col-helper-hot180` showed the fusions active with `runtime_fail=0`,
`jit_fallback=0`, `mono_assert=0`. Clean low-trace graphics check
`run-20260602-post-metadata-helpers-lowtrace360` still has real `D3D11CreateDevice=0` /
`GfxDevice=0`, with Hollow Knight CPU-active and final heartbeats shifted to Mono RVAs
`0x19213d`, `0x1922e0`, `0x4c9002`, and bsearch `0x14f180`. NEXT: map the new active Mono
`0x192xxx` family with IR/disassembly, then decide whether it is another finite metadata helper or a
real post-Mono semantic gate before any graphics-device transition.

**04:40 checkpoint (Codex): JIT flag correctness revalidated and the Mono `0x192xxx` helper landed.**
The Lane B fuzzer bug remains fixed on the current tree after the later Mono helper edits:
`python3 tests/hb_fuzz_diff.py --cases 200000 --batch 4096 --families shift_rotate_flags` completed
with `backend_mismatch_count=0`, `oracle_mismatch_count=0`, and `oracle_pass_count=200000/200000`.
Root cause remains the `SETcc r/m8` byte-destination write in `hb_arm64_codegen.c`; no interpreter or
Lane B decoder/lifter files were touched.

The `0x192xxx` Mono hot path was mapped to a finite metadata coded-index search helper. Added
`mono-metadata-coded-index-search` in `hb_arm64_codegen.c`, sharing the validated metadata column decode
logic and falling back before mutation outside the proven fast path. Validation: `make &&
./tests/hb_test_runner` -> `419 passed, 0 failed`; targeted `ntdll.so` rebuild/install contains
`mono-metadata-coded-index-search=1`, `mono-metadata-decode-col=1`, and
`mono-metadata-decode-row=1`. App run `run-20260602-mono-coded-index-helper-hot180` shows
`coded_index_fusion=1`, `decode_col_fusion=1`, `decode_row_fusion=1`, `bsearch_fusion=2`,
`hot_192240=0`, `hot_1922e0=0`, `hot_19213d=0`, and no runtime/JIT/Mono asserts. Clean DXMT check
`run-20260602-post-coded-index-lowtrace360` still has `D3D11CreateDevice=0`, `GfxDevice=0`,
`CreateSwapChain=0`, `runtime_fail=0`, `jit_fallback=0`, `mono_assert=0`, and Hollow Knight remains
CPU-active in Mono/Unity initialization.

NEXT: do not revisit the `0x192xxx` family unless it reappears. Map the new post-coded-index active
tail in `mono-2.0-bdwgc.dll` (including RVAs `0x183e00`/`0x183e59`/`0x183ac6`/`0x18d5d0` and
`0x53fa59`/`0x545bcb`/`0x545bd0`) and classify it as either another finite helper/codegen gap or the
actual post-Mono semantic gate before the first real `D3D11CreateDevice`.

**04:57 checkpoint (Codex): Mono checked row-decode entry helper added.** Focused IR probe
`run-20260602-mono-183e00-ir90` mapped `mono-2.0-bdwgc.dll` RVA `0x183e00` to the checked entry
wrapper for the same metadata row decode family: bounds/column-count asserts, then the existing
`0x183eb4` decode loop. Added `mono-metadata-decode-row-entry` in `hb_arm64_codegen.c` so valid calls
run as one whole-function helper and invalid/assert paths fall back before mutation. Validation:
`make && ./tests/hb_test_runner` -> `419 passed, 0 failed`; post-helper
`hb_fuzz_diff.py --cases 10000 --batch 1024 --families shift_rotate_flags` ->
`backend_mismatch_count=0`, `oracle_mismatch_count=0`; forced `ntdll.so` relink/install contains
`mono-metadata-decode-row-entry`. App run `run-20260602-mono-row-entry-helper-hot180` shows
`row_entry_fusion=1`, `coded_index_fusion=1`, `decode_col_fusion=1`, `bsearch_fusion=2`,
`runtime_fail=0`, `jit_fallback=0`, `mono_assert=0`, and still no real `D3D11CreateDevice`/`GfxDevice`.

NEXT: continue from the clean post-row-entry state. The active tail is now UnityPlayer compare block
around RVA `0x649910` plus Mono RVAs around `0x183a10`/`0x183ac6`, `0x1409870` string/hash loops, and
final-heartbeat Mono RVAs `0x150d95`/`0x58cb6`/`0x27ffb6`/`0x280028`/`0x4ca3d0`. Map those in bulk
against static disassembly + targeted IR; only add another helper if the fast path is finite and can be
mirrored exactly. Keep direct-memory optimization disabled for UnityPlayer `0x649910` until the earlier
faulting directmem path has a root-cause fix.

**05:19 checkpoint (Codex): post-row-entry hot-tail helper batch added and validated.** Static
disassembly plus targeted IR runs mapped three finite hot helpers: UnityPlayer RVA `0x649910`
`u32` pointer compare (`run-20260602-unity-cmp-649910-ir80`), Mono metadata row-pointer entry
RVA `0x183a10` (`run-20260602-mono-rowptr-183a10-ir80`), and Mono string hash at module RVA
`0x59870` (`run-20260602-mono-hash-59870-ir80`; the earlier `0x1409870` label was the guest address,
not module RVA). Added JIT helpers in `hb_arm64_codegen.c`: `unity-u32-ptr-compare`,
`mono-metadata-rowptr-entry`, and `mono-string-hash`. These do not enable the generic direct-memory
optimization that previously faulted at Unity `0x649910`; they use checked C helpers and preserve
byte-write / flag / return semantics.

Validation: `make && ./tests/hb_test_runner` -> `419 passed, 0 failed`;
`hb_fuzz_diff.py --cases 10000 --batch 1024 --families shift_rotate_flags` ->
`backend_mismatch_count=0`, `oracle_mismatch_count=0`; forced `ntdll.so` relink/install contains all
three new helper strings. App run `run-20260602-hot-tail-helpers-hot180` shows
`unity_cmp_fusion=1`, `mono_hash_fusion=1`, `rowptr_fusion=1`, existing Mono metadata fusions active,
and `runtime_fail=0`, `jit_fallback=0`, `mono_assert=0`. Clean low-trace
`run-20260602-post-hot-tail-helpers-lowtrace360` still has `D3D11CreateDevice=0`, `GfxDevice=0`,
`CreateSwapChain=0`, `runtime_fail=0`, `jit_fallback=0`, `mono_assert=0`; process samples keep
Hollow Knight at ~99-100% CPU, so this remains CPU progress before graphics init, not a wait park.

NEXT: continue mapping the new low-trace tail in bulk: Mono RVAs `0x19213d`/`0x192141`,
`0x18e2c0`, `0xb7c19`, `0x4ec1a9`, `0x7185e`, `0x183e00`, `0x3622ad`, `0x150d8a`,
`0x672c6`, and `0x53fadd`/`0x53faa3`, plus the non-Mono `0x87fff65b328` generated/runtime block.
Classify with static disassembly + targeted IR first; only add helpers for exact finite fast paths, and
keep watching for the first real `D3D11CreateDevice` marker after each batch.

**05:47 checkpoint (Codex): Mono string equality helper added and validated.** Static disassembly plus
targeted IR (`run-20260602-mono-streq-59830-ir80`) mapped Mono RVA `0x59830` to the finite byte-string
equality fast path: pointer-equal return 1, otherwise compare `[rcx]` with `[rcx + (rdx - rcx)]` until
mismatch or shared terminator. Added the exact JIT helper `mono-string-equal` in `hb_arm64_codegen.c`,
preserving RCX/RDX/R8/RAX and the post-branch flags for pointer-equal, mismatch, and terminator-equal
returns.

Validation: `cd engine/hyperbridge && make && ./tests/hb_test_runner` -> `419 passed, 0 failed`;
`python3 tests/hb_fuzz_diff.py --cases 10000 --batch 1024 --families shift_rotate_flags` ->
`backend_mismatch_count=0`, `oracle_mismatch_count=0`; forced `ntdll.so` relink/install
`build-20260602-mono-string-equal-ntdll` contains `unity-u32-ptr-compare`, `mono-string-hash`,
`mono-string-equal`, and `mono-metadata-rowptr-entry`. App run
`run-20260602-mono-string-equal-hot150` shows `mono_string_equal_fusion=1`, `mono_string_hash_fusion=1`,
`unity_cmp_fusion=1`, `rowptr_fusion=1`, and still `runtime_fail=0`, `jit_fallback=0`,
`mono_assert=0`; real `D3D11CreateDevice=0`, `GfxDevice=0`, `CreateSwapChain=0`.

NEXT: continue from the post-string-equality clean state. The remaining hot tail is now dominated by
UnityPlayer blocks around guest `0x87efc7e3876`/`0x87efc7e3860`, `0x87efc7e3d6c`/`0x87efc7e3d2d`,
and `0x87efc81b5db`/`0x87efc81b5d2`/`0x87efc81b5c0` (likely Unity RVAs `0x283876`/`0x283860`,
`0x283d6c`/`0x283d2d`, and `0x2bb5db`/`0x2bb5d2`/`0x2bb5c0` if the Unity base remains
`0x87efc560000`). Map those in bulk with static disassembly plus targeted IR before adding any helper.
Keep checking for the first real `D3D11CreateDevice`/`GfxDevice` marker after each batch; do not edit
`hb_decode_x64.c`, `hb_lift_x64.c`, or interpreter vector semantics.

---

## (background) GRAPHICS / DXMT — Unity attach to first frame
**DXMT staging is active and Hollow Knight now gets past `winemetal.dll`, `dxgi.dll`, and
`d3d11.dll` process attach.** JIT perf remains fallback-zero and is the speed foundation, but
PRIMARY GOAL now = clear UnityPlayer attach, reach Unity `GfxDevice` / DX11 device creation, then
drive DXMT to swapchain → on-screen window → present → first frame. Full brief:
`docs/CODEX-LANE-A-graphics-dxmt-to-window-brief.md`.
1. FIRST diagnose why graphics doesn't start (does Unity even reach d3d11/dxgi CreateDevice, or die
   earlier in Mono/CPU init?) → `reports/research/HB-GRAPHICS-bringup-diagnosis-20260601.md`.
2. THEN drive DXMT: device→swapchain→on-screen window→present→first frame; use `engine/dxmt/tests`
   fixtures where possible. Graphics lane is yours (`engine/dxmt`, `engine/graphics`, `engine/vkd3d`).
3. BOUNDARY: decoder/lifter (`hb_decode_x64.c`, `hb_lift_x64.c`) belong to Lane B (the Air) now —
   **do NOT edit them.** JIT codegen stays yours.

**Diagnosis checkpoint (2026-06-01 07:05 local):**
DXMT staging is functional: `run-20260601-064714-hk-dxmt-long120` has
`macrunner-hb-dxmt-unixlib-bridge=1`, DXGI/d3d11/winemetal attach success, and UnityPlayer attach
success with JIT fallback/codegen/helper-fault counters all zero. The finite semantic-import pass
removed the earlier `c000007b`/raw x64 kernel32 fallthroughs by wiring kernel32 stdio, codepage,
time/perf, environment, bulk kernel32/kernelbase semantic-table parity, kernel32 heap,
process/thread id/tickcount, and local-export `GetProcAddress`. Graphics is still not reached:
`GfxDevice=0`, `D3D11CreateDevice=0`, and swapchain `0` after a clean 120s run; wait tracing in
`run-20260601-065130-hk-dxmt-waittrace60` found no failed waits or obvious deadlock. Artificial
block-limit evidence in `run-20260601-065324-hk-dxmt-blocklimit60` stops inside a UnityPlayer
CFG/XFG indirect-call loop at `UnityPlayer.dll` RVA `0x19d439d` (`movabs r10, <xfg hash>; call
[rip+0x2dd83]` through helper table entry `0x1819e89f0`). NEXT: keep graphics primary, but treat
UnityPlayer throughput before `GfxDevice` as the current graphics bring-up blocker; profile/promote
that hot indirect-call/dispatch path or otherwise prove a Unity init gate. Continue runs through
`scripts/mr-run.sh`, prune with `scripts/mr-clean.sh --prune`, and do not edit Lane B-owned
`hb_decode_x64.c` / `hb_lift_x64.c`.

**Current checkpoint (2026-06-01 16:58 local):**
PE ntdll remains the active x64 main-thread route and the crash/fault class is clear. JIT safe-default
work since the 10:53 checkpoint added exact helper-backed promotions for two-block scan loops, Unity
byte-compare loops, helper-heavy CRT compare blocks, terminal-`Jcc` self-loops up to 16 IR ops, the
Mono null-qword table scan, direct-stack CALL/RET/PUSH/POP independent of unsafe direct memory, and
the Unity int32 comparator at UnityPlayer RVA `0x649910`. The comparator promotion now has a
near-cache fallback for CFG fragments without predecessor metadata, fires in Hollow Knight, and uses a
single exact helper for the full `load/cmp/jne -> setb/setl/ret` family instead of generic IR-block
dispatch. `scripts/mr-run.sh` now defaults `MACRUNNER_HB_JIT_DIRECT_STACK=1` for
`dist-arm64ec-spike` while keeping `MACRUNNER_HB_JIT_DIRECT_MEM=0`. Validation:
`test-20260601-1648-hb-i32-comparator-exact.log` = `395 passed, 0 failed`; staged `ntdll.so`
build/copy `build-20260601-1649-ntdll-i32-comparator-exact.log` = `0`.
Preserved low-trace proof `run-20260601-1418-hk-dxmt-lowtrace300` timed out cleanly:
`c0000005=0`, `c000007b=0`, `MEMORY_FAULT=0`, `JIT codegen failed=0`, `JIT helper fault=0`,
`macrunner-hb-jit-fallback=0`; DXMT bridge is loaded, but graphics still is not reached
(`GfxDevice=0`, `D3D11CreateDevice=0`, `CreateSwapChain=0`, `Present=0`), so this is not just trace
overhead. Wait-caller probe `run-20260601-1504-hk-dxmt-waitcaller75` maps the worker parks to
UnityPlayer RVA `0x577c92/0x577f44` and the zero-timeout main poll to UnityPlayer RVA `0xcba8b2`;
no Wine/DXMT device call is failing yet. Latest preserved run
`run-20260601-1651-hk-dxmt-i32-comparator-exact90` remains fallback/fault-zero and pre-GfxDevice,
with active fusions `bounded-byte-scan=456`, `copy-scan-counted=167`, `self-loop=96`,
`byte-compare-loop=19`, `i32-less-tiebreaker=1`, `null-qword-scan=1`. Hot work still remains around
Unity comparator/string/sort/XFG loops (`0x649910`, `0x6c44xx`, `0x2838xx`, `0x19d4xxx`). NEXT: keep
graphics primary, but diagnose/promote the next finite Unity hot family, especially the XFG indirect
call loop around `0x19d439d` or the sort partition loops around `0x2838xx`, until `GfxDevice`
appears; do not edit Lane-B-owned `hb_decode_x64.c` / `hb_lift_x64.c`.

**Current checkpoint (2026-06-01 18:28 local):**
Runner preloader root cause found and fixed in `scripts/mr-run.sh`: the ARM64EC Wine app child was
parking in macOS `_dyld_start` inside the temporary preloader copy; `WINELOADERNOEXEC=1` restores
Wine/HyperBridge bootstrap and is now the default for `dist-arm64ec-spike`. JIT work added safe
absolute/RIP-relative 64-bit indirect branch target loads for `CALL/JMP [abs]` without enabling broad
`MACRUNNER_HB_JIT_DIRECT_MEM`; validation `test-20260601-1725-hb-abs-branch-target.log` =
`395 passed, 0 failed`, staged build `build-20260601-1727-ntdll-abs-branch-target.log` = `0`.
Hollow Knight proof after the runner fix:
`run-20260601-1820-hk-dxmt-noexec-abs-branch-target240` timed out cleanly with DXMT loaded
(`macrunner-hb-dxmt-unixlib-bridge=1`, `winemetal.dll=40`, `UnityPlayer.dll=48`) and zero
`macrunner-hb-jit-fallback`, JIT codegen/helper fault, memory fault, or unsupported opcode. Renderer
markers remain absent: `GfxDevice=0`, `D3D11CreateDevice=0`, `CreateSwapChain=0`, `Present=0`.
Follow-up IR probe `run-20260601-1828-hk-dxmt-ir-sort-283876180` shows `0x283876` is already covered
by the bounded-byte-scan fusion, so NEXT is the remaining finite Unity hot families: the string
binary-search loop at `0x6c4451/0x6c4483/0x6c449e`, sort/callback loop heads around
`0x283d31/0x283d52/0x283d91`, and residual XFG `0x19d439d`. Continue graphics-primary; switch to
DXMT device/swapchain code only when a real `GfxDevice`/D3D11 create marker appears.

---

## (prior focus, now secondary) Phase 3 Hollow Knight — bulk JIT codegen coverage

**Resume checkpoint (2026-06-01 02:46 local, base HEAD `a697b4c`):** Hollow Knight remains loader-gated explicit-JIT
fallback/fault/unsupported-zero after scalar `MOV`, stack-control, extend, packed XMM move, near
conditional-PC, zero-test Jcc, lazy-flag record, adjacent mem64 pair, arithmetic/logical small
immediate, direct `STORE`/`MOV` immediate compaction, and direct-memory `TEST/CMP imm + Jcc` immediate
mask compaction, base+small-displacement direct-memory offset folding, and adjacent mem64 pair
offset folding, MSVC stack-spill/push/sub prologue fusion, and same-base `MOV`+`LEA` pairing.
The rank3 local-init triple `STORE imm; MOV same-base; LEA same-base` now also pairs, and adjacent
direct-memory scalar/XMM `LOAD+STORE` same-register pairs now keep loaded values live for the
following store.
Parallel Lane B
ISA merge is included in HEAD history; merged `hb_test_runner` baseline at this checkpoint is
`377 passed, 0 failed`, fast validation PASS. Current blocker is still throughput/no-window in hot
Mono/Unity code. Latest run `run-20260601-004957-phase3-rank3-prologue-init-test-jit/` preserves
fallback/fault/unsupported-zero with cleanup/prune `0`; the full rank3
`STORE/STORE/PUSH/SUB; STORE0/MOV/LEA; TEST/Jcc` block now fuses and shrinks `264 -> 200`. Previous
lazy zero-register run `run-20260601-004401-phase3-lazy-xzr-zero-jit/` shrank rank1 `156 -> 148`,
rank2 `212 -> 204`, rank3 `280 -> 264`, rank4 `256 -> 248`, rank6 `152 -> 144`, rank7 `168 -> 160`,
rank8 `144 -> 136`, rank9 `152 -> 144`, rank10 `204 -> 196`, rank11 `248 -> 240`, and rank12
`244 -> 236`. Latest run `run-20260601-005649-phase3-cmp-mem-zero-jcc-jit/` preserves
fallback/fault/unsupported-zero with cleanup/prune `0`; rank2 `CMP direct-memory, 0` feeding `Jcc`
now branches directly from the loaded value while preserving exact lazy CMP flags, shrinking
`0x87ef2ba32f8` `204 -> 172`. Latest run
`run-20260601-010437-phase3-testimm-flags-jcc-jit/` preserves fallback/fault/unsupported-zero with
cleanup/prune `0`; rank1 `TEST direct-memory, imm + Jcc` now branches from the ARM64 `ANDS` Z flag
while preserving exact lazy TEST flags, shrinking `0x87ef2bf915f` `148 -> 144`. Latest run
`run-20260601-011102-phase3-zero-tail-subs-jit/` preserves fallback/fault/unsupported-zero with
cleanup/prune `0`; the rank1 zero-arm tail now uses XZR/WZR zero stores and branches from `SUBS`,
shrinking `0x87ef2bf9182` `204 -> 196`. Latest run
`run-20260601-011514-phase3-rmw-resultonly-jit/` preserves fallback/fault/unsupported-zero with
cleanup/prune `0`; direct-memory logical RMW now drops dead lhs/rhs scratch preservation for
result-only lazy flags, but the rank1 OR arms stayed below the hot-entry cutoff so there is no
top-12 movement to claim. Latest run `run-20260601-013402-phase3-epilogue-ret-unset-jit/` preserves
fallback/fault/unsupported-zero with cleanup/prune `0`; MSVC epilogue restore/return blocks now
fuse for real lifted `MOV reg,[rsp+disp]` restores and unset no-imm `RET`, shrinking rank4
`0x87ef2ba335c` `248 -> 108` and rank12 `0x87ef2bf9919` `236 -> 100`. Latest run
`run-20260601-015918-phase3-indirect-branch-jit/` preserves fallback/fault/unsupported-zero with
cleanup/prune `0`; `HB_IR_JMP`/`HB_IR_CALL` now have native 64-bit register/direct-memory indirect
operand codegen with interpreter-order null-target guards. Hot `jmp rax` thunks shrink `136 -> 112`;
RIP-memory `jmp [rip+disp]` thunks are now helper-free (`136 -> 148` bytes because the null guard is
inline). Latest run `run-20260601-020700-phase3-memreg-test-jcc-jit/` preserves fallback/fault/
unsupported-zero with cleanup/prune `0`; direct-memory `TEST/CMP` with register RHS now shares the
branch-pair path, and the post-call boolean block `0x87ef2bf98d8` shrinks `196 -> 184`. Latest run
`run-20260601-022903-phase3-setcc-sequence-jit/` preserves fallback/fault/unsupported-zero with
cleanup/prune `0`; `HB_IR_SETcc` now has native E/NE scalar-flags materialization for adjacent and
one-`MOV`-intervened sequences, with register and direct-memory byte destinations covered. No
top-12 movement is claimed because the observed `SETE` fallthrough is behind a branch boundary in
this sample. NEXT: continue the bulk JIT-codegen coverage lane from executed 64-byte hot evidence,
favoring full post-call boolean tail fusion plus branch/fallthrough CFG fusion so the condition
materialization blocks behind hot branches compile as one native path. Keep
`reports/research/HB-JIT-CODEGEN-COVERAGE-matrix.md` and
`reports/research/HB-X64-ISA-COVERAGE-matrix.md` current, validate JIT-vs-interpreter/oracle per family,
run Hollow Knight with `scripts/mr-run.sh`, and prune with `scripts/mr-clean.sh --prune`.

**Latest batch after this checkpoint:** direct-memory `HB_IR_ADD/SUB` RMW now emits native
load/modify/store for `dst == src1`, covering lifted memory `INC/DEC` shapes. Validation:
`hb_test_runner` `376 passed, 0 failed`, fast validation PASS, and Hollow Knight
`run-20260601-023806-phase3-arith-rmw-jit` preserved fallback/fault/unsupported/runtime zero with
cleanup/prune `0`. Targeted `0x87ef2bda428` is helper-free but grows `184 -> 244` because ADD/SUB
must retain full lazy flag operands; keep the next pass focused on branch/fallthrough CFG fusion and
lazy-record compaction rather than claiming a size win for arithmetic RMW.

**Follow-up batch:** `run-20260601-024322-phase3-arith-rmw-deadflags-jit` preserves fallback/fault/
unsupported/runtime zero with cleanup/prune `0`; the direct-memory `ADD/SUB` RMW followed by
`TEST same-reg; E/NE Jcc` now omits the dead arithmetic lazy record because TEST overwrites flags.
Targeted `0x87ef2bda428` shrinks `244 -> 176` and is now smaller than the original helper-backed
`184`. NEXT remains branch/fallthrough CFG fusion and lazy-record compaction on the 64-byte hot
evidence, while keeping the bulk ISA matrix live.

**Follow-up batch:** `run-20260601-025552-phase3-lazy-pack-jit2` preserves fallback/fault/
unsupported/runtime zero with cleanup/prune `0`; lazy-flag metadata now uses packed ARM64 stores for
the fixed `hb_lazy_flags_t` header and mask layout while preserving old `x20`-`x23` live-value
behavior via `x16/x17` scratch registers. This is a bulk native-codegen compaction for every native
flag-producing IR path: hot blocks shrink rank1 `144 -> 132`, rank2 `172 -> 160`, rank3 `200 -> 188`,
rank6 `144 -> 132`, rank7 `160 -> 144`, rank8 `136 -> 124`, rank9 `144 -> 132`, rank10 `184 -> 172`,
and rank11 `240 -> 224`. NEXT: continue the bulk JIT-codegen coverage lane from executed 64-byte hot
evidence, prioritizing branch/fallthrough CFG fusion and helper-backed IR-family native promotion
only with JIT-vs-interpreter/oracle coverage; keep the parallel bulk ISA matrix live.

**Follow-up batch:** `run-20260601-030521-phase3-indirect-guard-jit` preserves fallback/fault/
unsupported/runtime zero with cleanup/prune `0`; native indirect `HB_IR_JMP/CALL` register and
direct-memory targets now use a shared null-target guard that sets `last_result=HB_ERR_EXEC_FAULT`
and skips to the normal block epilogue, instead of embedding a duplicate fault epilogue in every
branch thunk. Regression coverage keeps the null `CALL` no-push ordering and tightens native code-size
gates for the full indirect branch family (`jmp reg`, `call reg`, `jmp mem`, `call mem`). The final
90s steady-state top-12 is unchanged because these thunks are startup-hot rather than Mono-loop hot;
NEXT remains CFG/fallthrough fusion for the branch-split Mono blocks plus native promotion of
helper-backed IR families only with oracle parity coverage.

**Follow-up batch:** `run-20260601-031147-phase3-bswap-jit` preserves fallback/fault/
unsupported/runtime zero with cleanup/prune `0`; `HB_IR_BSWAP` now has native ARM64 `REV` codegen for
32-bit and 64-bit GPR destinations with no flag side effects, and helper fallback retained for
invalid widths/operands. Regression `jit_x64_native_bswap_family` covers the 32-bit zero-extending
and 64-bit register siblings and confirms flags remain unchanged. `hb_test_runner` is now
`378 passed, 0 failed`, fast validation PASS. The final 90s Mono top-12 is unchanged because BSWAP is
not in the sampled hot loop; NEXT remains branch/fallthrough CFG fusion plus native promotion of the
remaining finite helper-backed IR families against interpreter/oracle parity.

**Follow-up batch:** `run-20260601-032018-phase3-bitscan-jit` preserves fallback/fault/
unsupported/runtime zero with cleanup/prune `0`; `HB_IR_BSF/TZCNT/LZCNT/BSR` now have native ARM64
`RBIT/CLZ/CSEL` codegen for 8/16/32/64-bit GPR destinations with register, immediate, and
direct-memory sources. The path clears pending lazy flags and matches interpreter concrete ZF/CF
behavior, including BSF/BSR zero-source destination preservation. Regression
`jit_x64_native_bit_scan_family` covers nonzero, zero, 16-bit, 32-bit, 64-bit, and direct-memory
siblings. `hb_test_runner` is now `379 passed, 0 failed`, fast validation PASS. The final 90s Mono
top-12 is unchanged because bit-scan is not in the sampled hot loop; NEXT remains branch/fallthrough
CFG fusion plus native promotion of remaining finite helper-backed IR families against
interpreter/oracle parity.

**Follow-up batch:** `run-20260601-032811-phase3-cwd-jit` preserves fallback/fault/
unsupported/runtime zero with cleanup/prune `0`; `HB_IR_CWD` now has native ARM64 CWD/CDQ/CQO
codegen for 16/32/64-bit high-half sign extension. The CDQ path explicitly masks the native all-ones
value to `0xffffffff` before the generic 32-bit GPR store so the x64 `EDX` zero-extension semantics
match the interpreter. Regression `jit_x64_native_cwd_family` compares JIT vs interpreter for
positive/negative 16-bit, 32-bit, and 64-bit siblings and verifies flags/lazy flags remain unchanged.
`hb_test_runner` is now `380 passed, 0 failed`, fast validation PASS. The final 90s Mono top-12 is
unchanged because this finite helper-backed family is not in the sampled hot loop; NEXT remains
branch/fallthrough CFG fusion plus native promotion of remaining finite helper-backed IR families
against interpreter/oracle parity while keeping the bulk ISA matrix live.

**Follow-up batch:** `run-20260601-033442-phase3-xmmlogic-jit` preserves fallback/fault/
unsupported/runtime zero with cleanup/prune `0`; hot-marked `HB_IR_XMM_AND/XMM_ANDN/XMM_OR/XORPS`
now emit native two-lane ARM64 bit operations for XMM register operands plus gated direct-memory
128-bit operands, while preserving no flag/lazy side effects. Regression
`jit_x64_native_xmm_logic_family` compares JIT vs interpreter for AND, ANDN, OR, and XORPS with
register, src1-memory, and src2-memory siblings. `hb_test_runner` is now `381 passed, 0 failed`,
fast validation PASS. The final 90s Mono top-12 is unchanged because this family is not in the
sampled loop; NEXT remains branch/fallthrough CFG fusion plus native promotion of remaining finite
helper-backed IR families against interpreter/oracle parity while keeping the bulk ISA matrix live.

**Tier-1 game present (GOG, DRM-free):** Hollow Knight 1.5.12620 (64-bit) at
`/Users/timurtoby/Documents/MacRunner/Main/game-hollow.knight-(89718)/setup_hollow_knight_1.5.12620_(64bit)_(89718).exe`
(624M GOG InnoSetup installer + `Bonus/`). It's a sibling of the repo (outside `MacRunner/`) — use
the absolute path; do NOT copy 600M into the repo.
- **Extraction done:** `reports/phase4-hollow-knight/innoextract-20260530-200352.log` extracted to
  `…/game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight.exe` (Unity engine:
  `UnityPlayer.dll`, `Hollow Knight_Data`, MonoBleedingEdge).
- **2026-05-30/31 bring-up progress:** `scripts/mr-run.sh` now has macOS `timeout` fallback. Hollow
  Knight reaches and returns successfully from `UnityPlayer.dll` x64 `PROCESS_ATTACH` under
  HyperBridge and continues past Unity allocator startup. Cleared blockers: API-set target resolution/module map, dynamic guest
  `GetProcAddress`, ARM64X `.hexpthk` wrapping, localization/fibers/winrt API-set fallbacks,
  heap/TLS/FLS/LastError, startup/stdout/file type/command line/NLS/LCMap/environment strings,
  dynamic kernel semantic thunks, local heap ownership tracking, critical-section semantics,
  time/SystemTime conversions, and UnityPlayer SIMD families (`sqrt`, `rsqrt/rcp`, packed
  `mul/div`, `shufps/shufpd`, XMM high/low qword lane moves). Additional 2026-05-31 blockers cleared:
  TEB stack bounds for x64 `__chkstk`, SList, virtual memory, event/error/debug families,
  WinRT init, `CommandLineToArgvW`, file attributes, local file handles, `GetNativeSystemInfo`,
  `GlobalMemoryStatusEx`, x64 `CMPXCHG8B/CMPXCHG16B` (`0F C7 /1`, trigger `f0 48 0f c7 4e 40`),
  chunked x64 code fetch, SRW-lock and condition-variable host-boundary leaks, native fallback
  for ordinary `Heap*` imports, and the bit-scan family decoder bug where bare `0F BC` (BSF) was
  incorrectly treated as `F3 0F BC` (TZCNT). That BSF/TZCNT fix cleared the Unity small-allocator
  sentinel-bucket crash at `UnityPlayer.dll` RVAs `0x2afe1b`/`0x2b0069`; HyperBridge tests and
  `tools/hb_oracle/fast_validate_family.sh phase1_core` pass after the fix.
- **Current blocker (2026-05-31 18:35 local):** Phase 3 is still no-window/no-menu. Important
  evidence correction: this lane needs both gates: `MACRUNNER_HB_X64_LOADER=1` to route the AMD64
  PE through HyperBridge and `MACRUNNER_HB_BACKEND=jit` to select the JIT. Runs missing the loader
  gate exit early in ARM64/ARM64EC loader startup (`load_ntdll_functions` reports missing
  ARM64EC-only exports); runs missing the backend gate are interpreter-path evidence, not proof of
  the JIT backend. Loader-gated explicit-JIT probes showed zero codegen fallbacks but severe
  startup throughput loss from per-block whole-buffer W^X flips
  (`hb_jit_buffer_commit`/`hb_jit_buffer_make_writable` -> `__mprotect`). The current fix uses the
  macOS MAP_JIT thread write-protect API (`pthread_jit_write_protect_np`) plus dirty-range icache
  flushing, preserving the fallback safety net while removing the mprotect hot path. Cleared since
  `run-20260531-092812-cotaskmem-fix-phase3-probe/`: `CreateDirectoryW`, advapi/EventProvider ETW
  no-op semantics, current-process `VirtualAlloc/VirtualProtect/VirtualFree` replay into imported
  x64 contexts, xtajit64 Unix-side memory notifications, live VM write fallback for writable guest
  regions backed by RX Mach pages, `mr-run.sh` internal `config/env.sh` sourcing, JIT scalar load
  width/register semantics, FS/GS/RIP-relative scalar memory operand resolution, indirect
  `CALL/JMP` operand targets, persistent per-run JIT runtime fallback-to-interpreter handling, and
  explicit JIT codegen cases for every interpreter-supported `HB_IR_*` op (correctness-first helper
  route where native emit is not yet promoted), plus the MAP_JIT W^X fix. HyperBridge tests:
  `325 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` PASS after the
  JIT fixes and spike `ntdll.so` relink. Latest loader-gated explicit-JIT evidence:
  `reports/phase4-hollow-knight/run-20260531-190238-phase3-loader-jit-blockmap-sampled/` ran 300s
  with `MACRUNNER_HB_X64_LOADER=1 MACRUNNER_HB_BACKEND=jit`, reached Unity memory setup and Mono
  paths, traced 7,955 JIT blocks, and had zero `macrunner-hb-jit-fallback`, `JIT codegen failed`,
  `JIT helper fault`, `JIT buffer exhausted`, `MEMORY_FAULT`, `UNSUPPORTED_OPCODE`, or
  `runtime-fail`. Samples show the main macOS thread in `CFRunLoop`, seven
  `AssetGarbageCollectorHelper` workers in `NtWaitForSingleObject`, and the active x64 guest stack
  dominated by UnityPlayer guest PC `0x7ffd07cc548` (module base `0x7ffd0340000`, RVA `0x48c548`,
  epilogue of a UnityPlayer helper). A follow-up hot-block probe
  (`run-20260531-192827-phase3-loader-jit-hotbytes/`) kept fallback-zero and identified the actual
  helper-heavy throughput pattern: dynamic Mono code heap blocks around `0x87ef...` running
  byte/word string-scan loops such as `inc rax; cmp byte/word [base+index], 0/value; jne self`.
  No game window yet. Do not patch wait semantics speculatively:
  `run-20260531-175000-phase3-wait-resume-trace/` showed suspended workers resume successfully and
  then idle on companion waits. Current codegen pass is promoting the hot scalar loop family
  (`ADD/CMP-or-TEST/Jcc`, byte/word memory compare, self-branch) away from helper-heavy codegen
  while preserving loader-gated explicit-JIT fallback-zero.
- **Phase 3 gate still NOT passed:** no main menu, input, audio, or rendered frame yet; latest
  screenshots are desktop-only with no game window.
- **Run hygiene:** `scripts/mr-run.sh` for runs, `scripts/mr-clean.sh --prune` after each batch.

### ★ TOP PRIORITY (operator-directed 2026-05-31) — BULK JIT CODEGEN COVERAGE
This is the fastest route to the Hollow Knight window: it's throughput-bound on JIT fallbacks.
The IR-op set is finite (163 ops in `engine/hyperbridge/include/hb_ir.h`); the interpreter already
implements ALL of them; JIT codegen covers only ~65 → the rest fall back to the slow interpreter.
**Do it in bulk:** for every interpreter-supported IR op, add the ARM64 codegen in
`engine/hyperbridge/src/hb_arm64_codegen.c`, diff JIT-vs-interpreter/oracle per op, drive JIT
fallbacks on the Hollow Knight hot path to zero. Deliverable:
`reports/research/HB-JIT-CODEGEN-COVERAGE-matrix.md`. See "★ PRIORITY INSERT #2 — BULK JIT CODEGEN
COVERAGE" in the program doc.

Status 2026-05-31 19:30: first bulk pass published in
`reports/research/HB-JIT-CODEGEN-COVERAGE-matrix.md`; loader-gated explicit-JIT Hollow Knight run
(`run-20260531-192827-phase3-loader-jit-hotbytes/`) shows fallback-zero after the MAP_JIT W^X fix
and identifies hot dynamic Mono string-scan loops. Promote helper-backed scalar `ADD/CMP/Jcc`
loop blocks to native emit first; do not regress the
`MACRUNNER_HB_X64_LOADER=1 MACRUNNER_HB_BACKEND=jit` fallback-zero gate.

Status 2026-05-31 19:48: native ARM64 peephole for the hot scalar scan-loop family
(`ADD reg,1; CMP mem8/mem16,reg-or-imm; Jcc self`) is under validation. Unit coverage now has byte
and word scan-loop JIT-vs-runtime checks; `engine/hyperbridge/tests/hb_test_runner` => `327 passed,
0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` => PASS. Next: rebuild/relink the
spike `ntdll.so`, run loader-gated Hollow Knight, and compare hot-block/fallback counters.

Status 2026-05-31 20:08: scalar native JIT family expanded beyond the first scan-loop peephole:
plain-GPR/imm `ADD/SUB/AND/OR/XOR`, `CMP/TEST`, and adjacent `E/NE Jcc` pairs now emit native
ARM64 and record lazy flags without condition-helper fallback. Coverage:
`engine/hyperbridge/tests/hb_test_runner` => `332 passed, 0 failed`;
`tools/hb_oracle/fast_validate_family.sh phase1_core` => PASS; spike `ntdll.so` relinked. Hollow
Knight probes:
`run-20260531-195003-phase3-scanloop-jit/`,
`run-20260531-200058-phase3-scalar-native-jit/`, and
`run-20260531-200447-phase3-scalar-jcc-pair-jit/` all timeout cleanly with cleanup/prune `0` and
zero `macrunner-hb-jit-fallback`, `JIT codegen failed`, `JIT helper fault`, `UNSUPPORTED_OPCODE`,
`MEMORY_FAULT`, `runtime-fail`, or `JIT buffer exhausted`. Pair path improves the top counted-loop
guard blocks (`SUB/JNE` 372→292, `CMP/Jcc` 352→272 versus scalar-native-only) but remaining hot
copy/scan bodies are still large; next target is loop fusion or a smaller lazy-flag record path for
the `LOAD/STORE/INC/TEST/JE` + `SUB/JNE` two-block copy-loop family.

Status 2026-05-31 20:19: compact immediate emission added for JIT mask/lazy-flag recording so hot
native scalar blocks no longer materialize every small constant with four ARM64 instructions.
Coverage remains `engine/hyperbridge/tests/hb_test_runner` => `332 passed, 0 failed` plus
`tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; tests now assert code-size caps for the
scan-loop and scalar-branch peepholes. Spike `ntdll.so` relinked. Hollow Knight
`run-20260531-201543-phase3-compact-lazy-jit/` timed out cleanly (`MR_RUN_RC=143`) with
cleanup/prune `0` and zero JIT fallback/fault/unsupported counters. At the same 23k hot-block
sample, JIT buffer use improved `260384 -> 203664`; top hot blocks shrank: rank1 `292 -> 208`,
rank2 `508 -> 328`, rank3 `272 -> 188`, rank4 `520 -> 328`. Next evidence-backed target remains
real loop fusion for the two-block byte copy/scan family (`LOAD/STORE/INC/TEST/JE` plus `SUB/JNE`).

Status 2026-05-31 20:26: block-local native copy-scan body peephole added for
`LOAD byte; STORE byte; INC index; TEST byte; JE/JNE`, skipping dead `INC` lazy-flag recording that
is immediately overwritten by `TEST`. Coverage: `engine/hyperbridge/tests/hb_test_runner` =>
`334 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike
`ntdll.so` relinked. Hollow Knight `run-20260531-202253-phase3-copy-scan-jit/` timed out cleanly
(`MR_RUN_RC=143`) with cleanup/prune `0` and zero JIT fallback/fault/unsupported counters. Rank-2
copy-scan body shrank `328 -> 220` versus compact-lazy-only at the same 23k hot-block sample. JIT
buffer used is roughly flat (`203664 -> 203552`) because this optimized one hot compiled body, not
block count; remaining throughput work should fuse the rank1 `SUB/JNE` guard with the rank2 body or
add direct block chaining for the backedge.

Status 2026-05-31 20:45: persistent JIT block-cache promotion now fuses the real one-block-at-a-time
x64 loader shape for `LOAD/STORE/INC/TEST/JE` body + `SUB/JNE` count guard. The first CFG-only
attempt validated locally but did not trigger in Hollow Knight because `hb_lift_func_x64` still emits
single-block functions; the cache promotion regenerates the body cache entry once the guard and body
are both present. Correctness fix included: the copy-scan peephole now writes the loaded `AL/AX` back
to the guest register file. Coverage: `engine/hyperbridge/tests/hb_test_runner` => `337 passed, 0
failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so` relinked.
Hollow Knight `run-20260531-204546-phase3-cache-promote-copy-scan-jit/` timed out cleanly
(`MR_RUN_RC=143`) with cleanup/prune `0`, zero JIT fallback/fault/unsupported counters, and 2
`macrunner-hb-jit-fusion` events including body `0x87ef2bdf604` + guard `0x87ef2bdf612`. Hot-block
dispatch sample dropped from 23k to 12k at the same cap; the old rank1/rank2 copy loop disappeared.
NEXT: apply the same two-block promotion to the new top bounded byte-scan family:
`CMP rax,rdx; JE exit` guard plus `INC rax; CMP byte [rax+rcx],0; JNE guard` body.

Status 2026-05-31 21:05: bounded byte-scan and store/count-loop hot families promoted. Added
persistent-cache two-block fusion for `CMP rax,rdx; JE exit` + `INC rax; CMP byte [rax+rcx],0; JNE
guard`, plus block-local native emit for `STORE byte [ptr],src; INC counter; INC ptr; CMP
counter,limit; JB self` (both immediate and register limits, including source byte = counter low
byte). Coverage: `engine/hyperbridge/tests/hb_test_runner` => `340 passed, 0 failed`;
`tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so` relinked. Hollow
Knight `run-20260531-205913-phase3-bounded-scan-jit/` and
`run-20260531-210549-phase3-store-count-loop-jit/` both timed out cleanly (`MR_RUN_RC=143`) with
cleanup/prune `0`, zero JIT fallback/fault/unsupported counters, and 4 fusion events. Hot dispatch
sample moved `23k -> 12k -> 11k -> 10k`; top copy/bounded/store loops are no longer dominant. NEXT:
inspect the remaining top branchy memory-test/prologue blocks (`0x87ef2bf915f`,
`0x87ef2ba32f8`, `0x87ef2ba32d4`) and promote only evidence-backed finite families.

Status 2026-05-31 21:16: committed hot-loop promotion batch at `a41565b` and ran longer
post-hotloop validation: `run-20260531-211135-phase3-post-hotloop-300s-jit/` (`MR_RUN_RC=143`,
cleanup/prune `0`). Fallback/fault counters remain zero; log reaches Unity memory config and Mono
paths (`Hollow Knight_Data/Managed`, `MonoBleedingEdge/etc`) but still no game window. With lower
hot-block tracing overhead the sample remains around 10k dispatches; current top is branchy
memory-test/control code (`TEST byte [rdx],imm; JE`, RIP-relative `CMP/Jcc`, and nearby prologue /
epilogue blocks), so continue with evidence-backed native promotion for finite memory-test/Jcc and
small branch/control families rather than widening speculative patches.

Status 2026-05-31 21:29: direct-memory logical RMW and memory-immediate branch pairs promoted.
Native JIT now covers `AND/OR/XOR r/m,reg-or-imm` direct-memory read/modify/write and the hot
`TEST/CMP direct-mem,imm; E/NE Jcc` pair. Coverage:
`engine/hyperbridge/tests/hb_test_runner` => `343 passed, 0 failed`;
`tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so` relinked. Hollow
Knight `run-20260531-212945-phase3-memimm-jcc-jit/` timed out cleanly (`MR_RUN_RC=143`) with
cleanup/prune `0`, zero JIT fallback/fault/unsupported counters, and Mono paths reached. Hot block
sizes improved for the evidenced memory branch family: `TEST byte [rdx],1; JE` `220 -> 200`, and
RIP/absolute `CMP dword [abs],0; JNE` `276 -> 264`; total dispatch remains around 10k. NEXT:
continue from the still-hot branch/prologue bodies (`0x87ef2ba32d4`, `0x87ef2bf98b4`) and only
promote finite families that reduce helper calls or dispatch count without growing the common path.

Status 2026-05-31 21:55: scalar `MOV` and stack-control hot families promoted. Native JIT now covers
8/16/32/64-bit scalar `HB_IR_MOV` for GPR reg/imm plus gated direct-memory load/store siblings, and
gated direct-stack `PUSH`, `POP`, direct `CALL` return pushes, and `RET`/`RET imm16` while retaining
helper fallback outside `MACRUNNER_HB_JIT_DIRECT_MEM`. Coverage:
`engine/hyperbridge/tests/hb_test_runner` => `346 passed, 0 failed`;
`tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so` relinked. Hollow Knight
`run-20260531-214111-phase3-scalar-mov-jit/` and
`run-20260531-214911-phase3-stack-control-jit/` timed out cleanly (`MR_RUN_RC=143`) with cleanup/prune
`0`, zero JIT fallback/fault/unsupported counters, and Mono paths reached. Hot prologue/epilogue
native bodies shrank: `0x87ef2bf98b4` `500 -> 404 -> 304`, `0x87ef2ba32d4` `440 -> 388`,
`0x87ef2ba335c` `336 -> 284`, `0x87ef2bf9919` `328 -> 276`. NEXT: native
`HB_IR_ZERO_EXTEND`/`HB_IR_SIGN_EXTEND` for hot `MOVZX` blocks (`0f b6 d3`, `0f b7 04 51`), then
rerun the same fallback-zero Hollow Knight gate.

Status 2026-05-31 22:01: `HB_IR_ZERO_EXTEND`/`HB_IR_SIGN_EXTEND` promoted for GPR and gated
direct-memory operands. A first test run caught and fixed a native sign-extend bug where 32-bit
destinations wrote a full 64-bit negative value instead of truncating through destination width like
`hb_context_write_reg_value_sized`. Coverage: `engine/hyperbridge/tests/hb_test_runner` =>
`347 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so`
relinked. Hollow Knight `run-20260531-215737-phase3-extend-jit/` timed out cleanly (`MR_RUN_RC=143`)
with cleanup/prune `0`, zero JIT fallback/fault/unsupported counters, and Mono paths reached. Hot
`MOVZX` block `0x87ef2bf98d8` shrank `268 -> 244`; `0x87ef2bf98e7` remained size-neutral. NEXT:
inspect packed XMM move/direct-memory feasibility for `0x87ef2ba3301` or fuse the rank1
memory-test/RMW loop; do not broaden scalar extend without new evidence.

Status 2026-05-31 22:09: packed XMM move hot family promoted. Native JIT now covers 128-bit XMM
`HB_IR_LOAD`/`HB_IR_STORE` plus XMM reg-reg/direct-memory `HB_IR_MOV` using paired 64-bit ARM loads and
stores against `ctx->regs.x64.xmm[n][2]`; direct memory remains gated by `MACRUNNER_HB_JIT_DIRECT_MEM`.
Coverage: `engine/hyperbridge/tests/hb_test_runner` => `348 passed, 0 failed`;
`tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so` relinked. Hollow Knight
`run-20260531-220527-phase3-xmm-move-jit/` timed out cleanly (`MR_RUN_RC=143`) with cleanup/prune `0`,
zero JIT fallback/fault/unsupported counters, and Mono paths reached. Hot packed move block
`0x87ef2ba3301` shrank `236 -> 148`. NEXT: rank1 memory-test/RMW loop fusion or smaller branch/control
code for `0x87ef2bf915f`; keep fallback-zero gate.

Status 2026-05-31 22:17: near conditional-PC emit compressed for shared E/NE Jcc pair paths. For close
targets, codegen now materializes fallthrough PC once and applies a small taken delta; far branches keep
the old two-PC materialization. Coverage: `engine/hyperbridge/tests/hb_test_runner` =>
`348 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so`
relinked. Hollow Knight `run-20260531-221130-phase3-near-branch-pc-jit/` timed out cleanly
(`MR_RUN_RC=143`) with cleanup/prune `0`, zero JIT fallback/fault/unsupported counters, and Mono paths
reached. Hot branch blocks shrank broadly: rank1 `200 -> 180`, rank2 `264 -> 244`, rank3 `388 -> 368`,
rank6 `196 -> 176`, rank7 `208 -> 188`, rank8 `316 -> 296`, rank9 `208 -> 188`, rank10 `244 -> 224`.
NEXT: inspect rank8/rank3 for remaining helper or fusion opportunities; continue preserving
fallback-zero gate.

Status 2026-05-31 22:23: zero-test Jcc block promoted for the hot rank8 family. Native JIT now fuses
`XOR r,r; TEST same-r,same-r; J(E/NE)` into one block with known branch result, while still recording
TEST lazy flags and preserving partial-register writes. Coverage: `engine/hyperbridge/tests/hb_test_runner`
=> `349 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so`
relinked. Hollow Knight `run-20260531-221923-phase3-zero-test-jcc-jit/` timed out cleanly
(`MR_RUN_RC=143`) with cleanup/prune `0`, zero JIT fallback/fault/unsupported counters, and Mono paths
reached. Hot rank8 `0x87ef2bf98fb` shrank `296 -> 160`. NEXT: inspect rank3 prologue/local-store block
`0x87ef2ba32d4` or rank1 memory-test/RMW loop fusion.

Status 2026-05-31 22:36: lazy-flag record store compaction added for all native scalar flag producers.
Codegen now uses paired 64-bit ARM stores for `lhs/rhs` and `result/count`, preserving the same
`hb_lazy_flags_t` contents while shrinking every scalar flags/Jcc block. Coverage:
`engine/hyperbridge/tests/hb_test_runner` => `349 passed, 0 failed`;
`tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so` relinked. Hollow Knight
`run-20260531-223158-phase3-lazy-stp-jit/` timed out cleanly (`MR_RUN_RC=143`) with cleanup/prune `0`,
zero JIT fallback/fault/unsupported counters, and Mono paths reached. Hot block sizes improved again:
rank1 `180 -> 172`, rank2 `244 -> 236`, rank3 `368 -> 352`, rank8 `160 -> 152`, rank10 `224 -> 216`.
NEXT: inspect rank3 full prologue/local-store shape or rank1 RMW fusion; continue preserving
fallback-zero gate.

Status 2026-05-31 22:42: adjacent 64-bit direct-memory spill/restore pairs promoted. Native JIT now
combines adjacent `STORE+STORE` and `LOAD+LOAD` direct-memory pairs into ARM64 `STP`/`LDP`, targeting
stack spill/restore shapes in the remaining hot prologue/epilogue blocks. Coverage:
`engine/hyperbridge/tests/hb_test_runner` => `350 passed, 0 failed`;
`tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so` relinked. Hollow Knight
`run-20260531-223838-phase3-mem64-pair-jit/` timed out cleanly (`MR_RUN_RC=143`) with cleanup/prune
`0`, zero JIT fallback/fault/unsupported counters, and Mono paths reached. Hot prologue/epilogue blocks
shrunk: rank3 `0x87ef2ba32d4` `352 -> 340`, rank11 `0x87ef2bf98b4` `296 -> 284`, rank12
`0x87ef2bf9919` `268 -> 260`. NEXT: rank1 memory-test/RMW loop fusion or add full-block diagnostic
visibility for rank3 before further specialization.

Status 2026-05-31 22:49: generic native scalar small-immediate materialization compacted. The
`ADD/SUB/AND/OR/XOR` native path now uses compact immediate emission for small immediates instead of
unconditional 64-bit MOVZ/MOVK sequences, preserving the same lazy flag record semantics. Coverage:
`engine/hyperbridge/tests/hb_test_runner` => `350 passed, 0 failed`;
`tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so` relinked. Hollow Knight
`run-20260531-224514-phase3-scalar-imm-compact-jit/` timed out cleanly (`MR_RUN_RC=143`) with
cleanup/prune `0`, zero JIT fallback/fault/unsupported counters, and Mono paths reached. Hot
prologue/epilogue blocks shrank again: rank3 `340 -> 328`, rank4 `276 -> 264`, rank11 `284 -> 272`,
rank12 `260 -> 248`. NEXT: rank1 memory-test/RMW loop fusion or full-block diagnostics for rank3.

Status 2026-05-31 22:54: hot-block byte trace length made configurable with
`MACRUNNER_HB_TRACE_JIT_HOT_BYTES_LEN` (default 16, cap 128) and validated in
`run-20260531-225035-phase3-hotbytes64-jit/`. Coverage: `engine/hyperbridge/tests/hb_test_runner` =>
`350 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so`
relinked. Hollow Knight timed out cleanly (`MR_RUN_RC=143`) with cleanup/prune `0`, zero JIT
fallback/fault/unsupported counters, and Mono paths reached. New 64-byte evidence: rank3
`0x87ef2ba32d4` = stack spills, `push rdi`, `sub rsp,0x20`, `mov byte [rcx+0x18],0`, `mov rdi,rcx`,
`lea rsi,[rcx+8]`, `test rdx,rdx`, `je`; rank1 `0x87ef2bf915f` = repeated `TEST byte [rdx],bit; JE;
OR byte [rax+rbx+0x18],mask; MOV/load; ...` bit/RMW loop. NEXT: optimize rank3 `LEA`/local-init/test
path or rank1 bit/RMW fusion.

### ★ BULK ISA COVERAGE — MOVED TO LANE B (MacBook Air M1, separate machine) 2026-05-31
**This main-mac Codex (Lane A) no longer does bulk-ISA — it's on the Air now.** Lane A stays on
JIT perf / Hollow Knight window. To avoid a cross-machine merge collision, FILE-LEVEL split:
- **Lane A (this mac) MUST NOT edit** the decoder/lifter: `hb_decode_x64.c`, `hb_lift_x64.c`,
  `hb_decode_x86.c`, `hb_lift_x86.c` — those are Lane B's files on the Air.
- **Lane B (Air) owns** decode/lift + ISA matrix; it MUST NOT touch `hb_arm64_codegen.c`/`hb_jit*`
  (Lane A's files). Kit lives at `_air-bulk-isa-kit/` (see README-AIR-LANE-B.md).
- Lane B returns a patch via external disk; operator (Claude) reconciles it into main. If Lane A
  needs a decoder change for JIT work, flag it for the operator instead of editing the decoder.

---

## Phase ledger

### Phase 0 — close out residual fault — ✅ DONE (2026-05-30, verified)
- Fix: TLS-aware normalization of x64 callback targets in
  `engine/wine/dlls/ntdll/unix/macrunner_hb.c` (`macrunner_hb_dispatch_x64_callback`) +
  `signal_arm64.c` fallback. Reads `AddressOfCallBacks` from the AMD64 TLS dir, snaps near-padding
  PCs (0x14000150f → 0x140001510) to the real callback entry.
- Evidence (operator-verified): `reports/arm64ec-phase0-run-hello_x64-20260530-143933.log` →
  `hello from windows pe`, `run_exit=0`, `cleanup_exit=0`, ZERO runtime-fail/MEMORY_FAULT/c000007b;
  `stdout_stderr_x64` same. Trace: original=0x14000150f → target=0x140001510.

### Phase 1 — x86_64 ISA completeness & correctness — ✅ DONE (2026-05-30, verified)
Integer ISA + flags correctness (diff vs golden oracle), SSE→SSE4.2 + **AVX/AVX2**, x87 edge
cases, atomics + ARM weak-memory mapping (x86 TSO → ARM64 barriers), SEH across EC boundary.
Gate: expanded torture/fuzz suite passes vs golden oracle; 3+ non-trivial x64 console programs
(threads+SSE+exceptions) run correct.
- Evidence: `reports/phase1-gate-summary-20260530-155351.txt`:
  `hb_runner_rc=0`, `phase1_core_rc=0`,
  `phase1_sse_x64 rc=0`, `phase1_threads_x64 rc=0`, `phase1_exception_x64 rc=0`.
- Fixes in this pass: E0-E3 LOOP/JRCXZ family across decode/lift/interp/JIT/tests; x64 import
  semantics for thread creation/wait/handle APIs; x64 vectored exception semantic dispatch for
  `AddVectoredExceptionHandler`/`RaiseException`/`RemoveVectoredExceptionHandler`.

### Phase 2 — interpreter → JIT + translation cache — ✅ DONE (2026-05-30, verified)
- Gate evidence: `reports/HYPERBRIDGE-PHASE2-BENCH.md` from `scripts/bench-hyperbridge.sh`:
  cached-block JIT `29.07x` over interpreter (`960000` ops), per-block compile
  `on-demand`, block chaining `pc-target loop in hb_jit_runtime_run`.
- AOT/cache evidence: `reports/HYPERBRIDGE-TRANSLATION-CACHE.md`:
  `cold_cache_status=miss`, `warm_cache_status=hit`, `json_entries=1`.
- Verification: `reports/phase2-verify-hyperbridge-20260530-160301.log`:
  `Pass: 17`, `Errors: 0`, `HyperBridge verification PASSED`.

### Phase 3 — runtime/Win32 surface (input/audio/DXMT integration) — ⚠ FORMAL GATE IN PROGRESS (2026-05-30)
- Fixes in this pass:
  `ntdll/unix/macrunner_hb.c` x64 import semantics for `LoadLibraryA/W`,
  `LoadLibraryExA/W`, `FreeLibrary`, `GetProcAddress`, `Get/SetEnvironmentVariableA/W`;
  gated synthetic D3D mock modules for `d3d11.dll`/`d3d12.dll`/`dxgi.dll`;
  x64 UCRT `__stdio_common_vfprintf` semantic for guest va_list rendering.
  `ntdll/loader.c` semantic import stub list updated for dynamic loader/env APIs.
- Input/audio evidence: `reports/phase3-runtime/phase3-runtime-suite-20260530-170959-summary.jsonl`:
  keyboard rc=0 marker `input ok`; raw input rc=0 marker `input ok`;
  XInput marker `xinput probe ok`; waveOut marker `audio enum ok`.
- D3D evidence:
  `reports/phase3-runtime/direct-d3d11-vfprintf-final-20260530-170803-summary.json`
  (`rc=0`, marker `d3d11 triangle ok`, `trace_lines=11`);
  replay `artifacts/phase3/direct-d3d11-vfprintf-final-20260530-170803/replay/replay.json`
  (`status=PASS`, `present_count=1`, `non_background_pixels=1152`, `unsupported_calls=0`,
  PPM `d3d-trace.ppm`).
- Regression evidence:
  `reports/phase3-runtime/post-phase3-regression-20260530-171203.log`:
  `305 passed, 0 failed`, `FAST VALIDATION: PASS`;
  `reports/phase3-runtime/post-phase3-phase0-20260530-171220-summary.jsonl`:
  `hello_x64` rc=0, `stdout_stderr_x64` rc=0.
- Formal gate active on Hollow Knight x64. Current evidence includes successful `UnityPlayer.dll`
  x64 `PROCESS_ATTACH`, Unity memory configuration, Mono path/config startup, D3D module loads,
  cleared Mono code-heap memory writes, and a hardened JIT route through scalar memory and indirect
  branch families, all interpreter-supported `HB_IR_*` ops having explicit codegen cases, and the
  MAP_JIT W^X fix for explicit-JIT throughput. HyperBridge tests: `325 passed, 0 failed`;
  `tools/hb_oracle/fast_validate_family.sh phase1_core` PASS after the W^X fix and spike
  `ntdll.so` relink. Latest explicit-JIT evidence:
  `reports/phase4-hollow-knight/run-20260531-190238-phase3-loader-jit-blockmap-sampled/` ran 300s
  with `MACRUNNER_HB_X64_LOADER=1 MACRUNNER_HB_BACKEND=jit`, reached Unity memory setup and Mono
  paths, traced 7,955 JIT blocks, and had zero
  `macrunner-hb-jit-fallback`, `JIT codegen failed`, `JIT helper fault`, `JIT buffer exhausted`,
  `MEMORY_FAULT`, `UNSUPPORTED_OPCODE`, or `runtime-fail`. No game window appeared and all window
  captures remain absent; next evidence pass is UnityPlayer/Mono post-bootstrap CPU ownership while
  preserving loader-gated explicit-JIT fallback-zero. Gate remains open until main menu + input +
  audio + rendered frame are captured under the spike build.

### Phase 4 — green-list bring-up ladder — ⏳ PENDING after Phase 3 Hollow Knight gate
Gate requires ≥5 Tier-1 games playable start→gameplay for ≥30 min each. Start after Hollow Knight
passes the Phase 3 menu/input/audio/rendered-frame gate.

### Phase 5 — hardening / fuzz / regression / CI — ⬜ partial continuous checks passed; full gate pending
Available post-Phase3 regression checks are green (see Phase 3 regression evidence). Formal gate
still requires green CI across the full game/regression suite after Phase 4 assets exist.

---

## Milestone protection (do not regress)
Plain x86_64 PE runs end-to-end (run_exit=0) via ARM64EC. Archived:
`archives/milestone-arm64ec-x64-e2e-20260530.tar.gz` (local + external MacRunner-ARCHIVES,
sha ccb34a25…). If any phase risks regressing this, STOP and escalate.

---

## Lane A checkpoint — 2026-06-03 TESTW gate cleared, still climbing

- Root cause fixed: x64 decoder opcode `0x85 /r` ignored operand-size override and decoded
  `66 85 c9` (`test %cx,%cx`) as 32-bit TEST. In Hollow Knight Mono, `ecx=0x01010000`
  must produce ZF=1 for the low 16 bits; the old decode let `jne` take the wrong path and
  produced the invalid `RuntimeType` vtable slot-16 signature.
- Code fix: `engine/hyperbridge/src/hb_decode_x64.c` now passes `op_size` to `parse_modrm`
  for `0x85 /r`. Regression coverage added in `engine/hyperbridge/tests/hb_test_runner.c`
  for `66 85`, default `85`, `48 85`, `66 F7 /0`, and `F6 /0`, plus a JIT branch test proving
  `66 85 c9; jne` uses low16 while the 32-bit sibling still branches.
- Loader gate kept moving: `engine/wine/dlls/ntdll/unix/loader.c` treats
  `RtlpFreezeTimeBias` and `RtlpQueryProcessDebugInformationRemote` as optional wow64 ntdll
  exports; current PE ntdll lacks `RtlpFreezeTimeBias`.
- Validation:
  `make -C engine/hyperbridge` PASS. `engine/hyperbridge/tests/hb_test_runner` reached
  `443 passed, 2 failed`; the two failures are pre-existing (`out.blocks_executed == 8` and
  PE mprotect/expected-2-got-1), and the new TEST tests passed.
- App proof: `reports/phase4-hollow-knight/run-20260603-173400-testw85-clean420/` ran 420s
  with heartbeat + wait-semantic trace. Result timed out by wrapper (`rc=143`) while still
  progressing: final heartbeat `label=thread blocks=0x1e5c2d steps=0xbc36cf rva=0xd5234`.
  No `RuntimeType`, invalid vtable, assertion, or runtime-fail appeared. Stdout reached Unity
  memory configuration plus Mono path/config. This is past the old 1.737M barrier and is not
  a hard park in this run.
- Diagnostic note: a transient sample at 35s showed `EnterCriticalSection` inside
  `macrunner_hb_try_kernel32_handle_semantic`, but env-gated critical-section trace later showed
  zero `enter-wait` events and sustained heartbeat progress. Treat that as a transient wait,
  not current root cause.

NEXT: run a longer clean settle (900s+) with heartbeat + wait-semantic and late samples. If it
parks, diagnose the final stable `rva`/wait target. If it keeps moving, shift to throughput
acceleration and/or graphics/D3D arrival probes. Current STOP conditions are unchanged: main menu
with rendered frame/input/audio, or a reproducible hard park/runtime-fail with exact signal target.

---

## Lane A checkpoint — 2026-06-03 worker wait verdict, not root signal bug

- Checkpoint commits now protecting this climb:
  - `3efbb1a fix(Lane A): honor x64 TEST operand-size override`
  - `9ddea5d fix(Lane A): tolerate optional wow64 ntdll exports`
  - `5460247 diag(Lane A): trace HyperBridge critical-section waits`
  - `58c9566 diag(Lane A): trace semaphore create and outer wake callers`
- Wait trace evidence:
  - `reports/phase4-hollow-knight/run-20260603-183506-semaphore-outer240/`
  - `reports/phase4-hollow-knight/run-20260603-184240-semaphore-outer520/`
- The parked Unity worker threads wait in `WaitForSingleObjectEx(INFINITE)` at
  `UnityPlayer+0x577c92` on work semaphores `0x44/0x50/0x5c/0x68/0x74/0x80/0x8c`,
  created by `CreateSemaphoreExW` caller `UnityPlayer+0x577e9d`.
- No `WakeByAddress*`/`WaitOnAddress` path appears (`address-wake=0`, `address-wait=0`).
  No `ReleaseSemaphore` ever targets those work semaphores. The only repeated semaphore releases
  target the sibling ready semaphores `0x48/0x54/0x60/0x6c/0x78/0x84/0x90`, from worker startup
  (`outer=UnityPlayer+0x577c77`), plus unrelated handle `0xa4`.
- Who should signal: Unity producer paths should store callback/data into the worker object
  (`+0x68/+0x70`) and release the work semaphore at `+0x58`; static callsites identified are
  `UnityPlayer+0x578176`, `+0x579415`, `+0x5795f5`, and shutdown `+0x577ff9`. These did not
  execute in the traced runs.
- Verdict: the worker wait is downstream. The engine has not reached the Unity producer/scheduler
  signal, so adding a wake to the worker semaphore would be a symptom patch.

## Lane A checkpoint — 2026-06-03 current deeper gate = Mono generated-code CPU burn

- Direct-stack/cache controls:
  - `run-20260603-190251-no-direct-stack300b/`: with `MACRUNNER_HB_JIT_DIRECT_STACK=0` and
    `MACRUNNER_HB_TRANSLATION_CACHE=0`, HK passed the earlier `mono+0x150ec2` epilogue gate and
    reached `blocks=0x1e6206`.
  - `run-20260603-195215-direct-stack-fresh260/`: with direct-stack on and fresh cache, no
    RuntimeType/assert/D3D/Gfx failure, but heartbeat flattened around `blocks=0x1e5ffb`.
- Hot-block profiling:
  - `run-20260603-201243-jithot260/` shows top HyperBridge JIT guest blocks in
    `mono_class_get_flags`, especially `mono+0xd1530` and `mono+0xd1567`, ~33k hits.
  - `run-20260603-201906-jithot-sample300/` sampled the CPU-heavy thread at
    `0x87fff975c78` in `<unknown binary>`.
  - `run-20260603-203214-sample-vmmap300/` mapped that PC to anonymous executable memory:
    `VM_ALLOCATE 87fff960000-87fffa60000 [1024K] r-x/rwx`.
- LLDB memory read was attempted in `run-20260603-203830-hotpc-bytes300/`, but macOS denied
  attach. The sample/vmmap evidence is still enough to classify the active CPU PC as Mono-generated
  executable code heap, not Wine wait, Unity wait, or PE image code.

NEXT: trace/probe the Mono generated-code execution path, not worker semaphores. The immediate
question is whether `0x87fff960000-0x87fffa60000` contains guest x64 managed code being executed
directly, or an intended host/native code heap spinning in runtime logic. Instrument executable
`VirtualAlloc`/`VirtualProtect` creation and/or add an in-process byte dump for sampled hot PCs.

## Lane A checkpoint — 2026-06-03 executable-region probes added

- Added diagnostic envs:
  - `MACRUNNER_HB_TRACE_EXEC_VIRTUAL=1` in `macrunner_hb.c` logs executable
    `VirtualAlloc`/`VirtualProtect` and x64 syscall `NtAllocateVirtualMemory`/`NtProtectVirtualMemory`
    transitions with guest caller/outer where available.
  - `MACRUNNER_HB_TRACE_JIT_NATIVE_RANGE_START/END` and
    `MACRUNNER_HB_TRACE_JIT_GUEST_RANGE_START/END` in `hb_runtime.c` force detailed JIT block IR
    dumps for selected host-native or guest-address ranges.
- Build proof: `reports/phase4-hollow-knight/build-20260603-210028-exec-virtual-syscall-trace/`
  and `reports/phase4-hollow-knight/build-20260603-212706-jit-guest-range/` both rebuilt and
  reinstalled `ntdll.so` successfully.
- Probe results:
  - `run-20260603-205254-exec-virtual300/` and
    `run-20260603-210105-exec-virtual-syscall300/` saw only two KERNEL32 executable allocs
    (`PAGE_EXECUTE_READWRITE`, 64 KiB each), and no `0x87fff...` alloc/protect via KERNEL32 or
    x64 syscall dispatch.
  - `run-20260603-210656-virtual-region300/` produced 9491 virtual sync/record lines but none for
    `0x87fff...`; that sampled region is not created through the normal HB guest-memory sync path.
  - `run-20260603-212039-jit-native-range300/` found `0x87fff...` as guest managed-code addresses
    in JIT logs, while native emitted ranges were `0x111...`; therefore the sample's `0x87fff975c78`
    should be treated as guest/managed PC, not host-native code.
  - `run-20260603-212744-jit-guest-range-975c78/` did not match the narrow range because that run's
    live sample showed parked ntdll wait frames instead of the unknown-code CPU thread.

NEXT: run one combined settle/probe with heartbeat + wait-semantic + guest-range trace around the
sampled managed-code heap, and take a late sample. If the sample shows unknown `0x87fff...`, expand
guest range to the sampled 4 KiB page and rerun immediately; if it shows ntdll wait, use the existing
semaphore verdict and continue toward producer/scheduler reachability rather than worker wake hacks.

## Lane A checkpoint — 2026-06-04 TSO root-fix backend patch

- Heartbeat capture checkpoint landed as `3c85aa8`: x64 dispatch now emits an immediate
  `phase=start` heartbeat when `MACRUNNER_HB_TRACE_HEARTBEAT=1`; proof run
  `run-20260604-082244-heartbeat-proof-tso-mp/` still had `heartbeat_count=0`, which now means the
  dispatcher was not entered, not that the sampler is blind.
- Backend patch in progress:
  - direct scalar guest loads/stores now emit `LDAR[B/H/W/X]` / `STLR[B/H/W/X]`;
  - direct stack POP/RET/PUSH and scalar scan loop reloads are acquire/release hardened;
  - JIT `CMPXCHG/CMPXCHG8B/XCHG/XADD` route through a full-barrier atomic helper;
  - aligned 8/16/32/64-bit memory RMW uses host `__atomic` on the resolved guest backing pointer;
  - unaligned/cacheline-crossing/128-bit forms fall back through a split-lock gate and interpreter op.
- Validation artifacts:
  - HyperBridge build green: `build-20260604-083823-lane-a-tso-codegen-x22offset.log`
  - `tools/hb_oracle/fast_validate_family.sh phase1_core`: PASS
  - `hb_fuzz_diff.py --families xchg_cmpxchg --cases 20000 --arch x64`: 0 backend/oracle mismatches
  - JIT backend diff, 5000 xchg/cmpxchg cases: 0 backend/oracle mismatches
- Full `make -C engine/hyperbridge test` remains red with 442 passed / 5 failed:
  decoder NOP family, existing block-count expectation, PE mprotect errno 13, and two direct-mem
  code-size thresholds. The TSO spin test is not among the failures.

NEXT: commit this checkpoint, relink/install/codesign `ntdll.so`, build/run `tso_litmus.exe`
`mp/spin/cas/xadd/split`, then retry Hollow Knight with heartbeat and Gfx/D3D trace gates.

## Lane A checkpoint — 2026-06-04 TSO atomics/no-hoist validation green

- Root fix completed in the JIT path:
  - atomic IR is blocked from hot self-loop/two-block/four-block fusion so LOCK RMW cannot be
    hoisted or lowered through non-atomic loop helpers;
  - JIT block helper execution routes `CMPXCHG/CMPXCHG8B/XCHG/XADD` through the full-barrier atomic
    helper instead of the plain interpreter helper;
  - unaligned/cacheline-crossing LOCK RMW now takes the split-lock gate and performs guest-memory
    read/modify/write directly for `CMPXCHG`, `CMPXCHG8B`, `XCHG`, and `XADD`;
  - wide self-base loads such as `mov rbx, [rbx]` fall back to the fenced helper path, avoiding
    LDAR on dynamically unaligned addresses.
- Install detail: manual `ntdll.so` relink against current `libhyperbridge.a` and Lane A baseline
  `system.lanea49.o`/`virtual.lanea49.o`; installed and codesigned into both
  `dist-arm64ec-spike/lib/wine/aarch64-windows/ntdll.so` and
  `dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so`. The earlier xadd failure was partly caused
  by only replacing the windows-side ntdll while Wine loaded the unix-side copy.
- Validation:
  - post-fuzz TSO matrix: `reports/phase4-hollow-knight/run-20260604-114143-tso-litmus-post-fuzz-final-matrix/`
    has `mp`, `spin`, `cas`, `xadd`, `split`, and `sb` all `rc=0`/PASS with heartbeat samples.
  - atomic ISA/JIT fuzz: `engine/hyperbridge/reports/phase4-hollow-knight/fuzz-20260604-114021-xchg-cmpxchg-tso-final.json`
    has `backend_mismatch_count=0`, `oracle_mismatch_count=0`, `oracle_pass_count=20000`.
  - `hb_test_runner` remains at the known residual `442 passed, 5 failed`
    (`reports/phase4-hollow-knight/test-20260604-113936-hb-test-runner-after-self-base.log`):
    decoder NOP expectation, loop block-count expectation, PE mprotect errno 13, and direct-mem
    code-size thresholds. The TSO litmus gates are green.

NEXT: commit this Lane A checkpoint, then run Hollow Knight 900s with heartbeat and D3D/Gfx trace
gates. The immediate pass criterion is clearing the `0x513xxx` livelock and advancing toward
`GfxDevice`/`D3D11CreateDevice`.

## Lane A checkpoint - 2026-06-04 CMPXCHG16B JIT gate

- Hollow Knight with the TSO atomics/no-hoist checkpoint no longer terminates at the original
  `0x513xxx` livelock. The next runtime stop was UnityPlayer.dll `rva=0x2bb3cf`, immediately before
  `lock cmpxchg16b 0x40(%rsi)` at UnityPlayer.dll `rva=0x2bb400`.
- Root fix added on the Lane A owned JIT side: `HB_IR_CMPXCHG8B` with `HB_SIZE_128` now executes
  under the existing split-lock gate instead of returning unsupported. It enforces the x86
  16-byte alignment requirement, compares `RDX:RAX` with the 128-bit memory value, writes
  `RCX:RBX` on success, writes old memory back to `RDX:RAX` on failure, and updates ZF.
- Regression coverage: the existing `interp_x64_cmpxchg8b_cmpxchg16b_family` test now also runs
  the `lock cmpxchg16b 0x40(%rsi)` success and failure cases through `HB_BACKEND_JIT`.
- Validation:
  - HyperBridge build: `reports/phase4-hollow-knight/build-20260604-120409-hb-cmpxchg16b-jit-test.log`
    PASS.
  - `tools/hb_oracle/fast_validate_family.sh phase1_core`:
    `reports/phase4-hollow-knight/validate-20260604-120409-phase1-core-cmpxchg16b-jit.log` PASS.
  - Hollow Knight after the fix:
    `reports/phase4-hollow-knight/run-20260604-115245-hk-cmpxchg16b-directmem0-300/` crosses
    the Unity `0x2bb400` gate and records `rva_513xxx=10`, but has not reached
    `GfxDevice`/`D3D11CreateDevice`.
- Current active blocker: Mono asserts while constructing `System.RuntimeType`:
  `invalid vtable method slot 16 with method System.Reflection.MemberInfo:get_Name()`, then faults
  at mono-2.0-bdwgc.dll `rva=0x1f71a3` on `mov (%rdi),%rax` with `rdi=0`.

NEXT: commit this CMPXCHG16B checkpoint, then diagnose why Mono metadata/vtable state is corrupted
before `GfxDevice`.

## Lane A checkpoint - 2026-06-04 Mono metadata fusion bisection

- Hollow Knight now clears the original `0x513xxx` TSO livelock and the Unity
  `lock cmpxchg16b` gate, then fails in Mono before graphics with:
  `System.RuntimeType has invalid vtable method slot 16 with method
  System.Reflection.MemberInfo:get_Name()`, followed by mono-2.0-bdwgc.dll
  `rva=0x1f71a3` faulting on `mov (%rdi),%rax` with `rdi=0`.
- Bisection with temporary `MACRUNNER_HB_DISABLE_MONO_FUSION*` diagnostics:
  disabling all Mono metadata fusions clears the assertion for 180s; disabling
  only string fusions is not required. Disabling bsearch/coded-index/decode-row-loop
  is insufficient, and disabling decode-row-entry or decode-col alone is insufficient.
- Production diagnostic switch was removed before this checkpoint.
- Correctness fix applied in the Mono `metadata_decode_col` fused helpers:
  the native loop refreshes `eax` to the next even-column width at the end of each
  pair iteration; both C helpers were leaving `eax` at the previous odd-column width.
  The helpers now mirror that loop and use native 32-bit row-size multiplication.
- Validation:
  - build/relink/install/codesign: `reports/phase4-hollow-knight/build-20260604-125709-hb-mono-decode-col-offset.log`
    PASS, artifact `ntdll-hb-mono-decode-col-offset-lanea49base-20260604-125709.so`.
  - Hollow Knight: `reports/phase4-hollow-knight/run-20260604-125729-hk-mono-decode-col-offset300/`
    still times out with `invalid_vtable=2`, `runtime_fail=1`, `GfxDevice=0`,
    `D3D11CreateDevice=0`.

NEXT: continue Mono metadata fusion bisection. The offset-loop bug is real but not the vtable
blocker; isolate the remaining corruptor among rowptr-entry, decode-row-entry/loop, decode-col,
coded-index, and bsearch with pair disables or direct helper-vs-IR comparison.

## Lane A checkpoint - 2026-06-04 LODS partial-register semantics

- Evidence: the broad scalar/string ISA fuzz isolated a `string_ops:lodsb` oracle mismatch:
  HyperBridge wrote `RAX=0x5d` while x86 preserved the upper bits and expected
  `RAX=0x7000105d`. This is a real x86 partial-register bug, but not the Hollow Knight
  Mono blocker root.
- Root fix: `HB_IR_LODS` now writes through the normal sized-register path. `LODSB`/`LODSW`
  update only `AL`/`AX`, `LODSD` zero-extends through `EAX` in x86-64, and `LODSQ` writes
  full `RAX`.
- Regression coverage: updated the incorrect `LODSB` tests, added a `LODSW` sibling, and
  kept `LODSD`/REP `LODSB` coverage in the `string_ops` fast family.
- Validation:
  - targeted string-op runner: `reports/phase4-hollow-knight/test-20260604-135924-hb-runner-fast-string-ops.log`
    has `27 passed, 0 failed`.
  - string-op fuzz: `reports/phase4-hollow-knight/fuzz-20260604-135954-string-ops-lods-fix-rebuild.json`
    has `backend_mismatch_count=0`, `oracle_mismatch_count=0`, `oracle_pass_count=20000`.
  - scalar/string fuzz: `reports/phase4-hollow-knight/fuzz-20260604-140120-scalar-string-post-lods-fix.json`
    has `backend_mismatch_count=0`, `oracle_mismatch_count=0`, `oracle_pass_count=4912`
    with only the known shared `div_r9` traps.
  - relink/install/codesign: `reports/phase4-hollow-knight/build-20260604-140200-hb-lods-partial-reg.log`
    PASS, artifact `ntdll-hb-lods-partial-reg-lanea49base-20260604-140200.so`.
  - Hollow Knight: `reports/phase4-hollow-knight/run-20260604-140232-hk-lods-partial-reg300/`
    still reports `invalid_vtable=2`, `runtime_fail=1`, `GfxDevice=0`, `D3D11CreateDevice=0`;
    the fault remains mono-2.0-bdwgc.dll `rva=0x1f71a3` on `mov (%rdi),%rax` with `rdi=0`.

NEXT: commit this LODS correctness checkpoint, then continue the Mono invalid-vtable evidence
pass. Current priority is to compare native Mono metadata/type/vtable helper behavior against
the lowered IR around the first bad `System.RuntimeType` slot, not to revisit the cleared
TSO/CMPXCHG16B gates.

## Lane A checkpoint - 2026-06-04 HK sampler repair

- Evidence: `run-20260604-unaligned-tso-hk900/` was started with the old controller and wrote
  `hb_count=0` at 60/300/600s while sampling a direct child of `mr-run.sh` instead of the actual
  hot `Hollow Knight.exe` descendant. The run was stopped as blind/invalid for block trajectory.
- Fix: `reports/phase4-hollow-knight/run-20260602-livelock-decisive-1800b/controller.py` now walks
  descendants recursively and chooses the process whose command contains `Hollow Knight.exe`, falling
  back to the first descendant only if the game process is not yet visible.
- Validation: `python3 -m py_compile` passes. Next HK run must use this controller so `samples.tsv`
  reports the real game PID/CPU while the heartbeat parser continues to pull block/rva from
  `macrunner-hb-heartbeat` lines.
- 60s verification: `reports/phase4-hollow-knight/run-20260604-heartbeat-sampler-game60/` uses
  game-lifetime sampling (`MR_HK_SAMPLE_FROM_GAME=1`) and proves capture is working:
  sample `60` has `hb_count=1781`, `blocks=0x185cba` (`1596602`), `rva=0xd3150`, real HK
  `pid=49810`, `pcpu=97.3`, `etime=01:01`; final row has `hb_count=1941`,
  `blocks=0x1a876a` (`1738602`), `rva=0x51589d`. D3D/Gfx counts remain 0 in this short proof.

NEXT: run the TSO litmus suite under MacRunner before any more HK diagnosis.

## Lane A checkpoint - 2026-06-05 Mono assertion cleared; next gate is HB JIT signal recovery

- TSO verdict: `reports/research/tso_litmus.c` cases `mp`, `spin`, `cas`, `xadd`, and
  `split` all PASS under MacRunner. The former `0x513xxx`/`0x515xxx` plateau is not the
  window gate; it was Mono assertion formatting.
- Mono metadata verdict: disabling/guarding the Mono metadata `decode_col` fusion removes
  `System.RuntimeType invalid vtable method slot 16 (get_Name)`. Post-guard HK no longer
  logs that assertion, but still stays before graphics (`GfxDevice=0`, `D3D11CreateDevice=0`).
- Current HK plateau: fresh JIT runs reach about `hb=2169`, `blocks=0x1e4fee`/`0x1e5797`
  with no Mono assertion and no D3D/Gfx calls.
- New root evidence: `WINEDEBUG=+seh` run
  `reports/phase4-hollow-knight/run-20260605-124824-hk-seh-attempt2-abs-300/` repeats
  `dispatch_exception code=c0000026 (EXCEPTION_INVALID_DISPOSITION)` from
  `ntdll!raise_status+0x84`. The first invalid unwind follows
  `EXCEPTION_DATATYPE_MISALIGNMENT` at guest/live address `0x87eff24e308`; Wine then tries
  to unwind `pc=0x1090e0fb4 lr=0x1090e0fb8`, which is inside the 128 MB HyperBridge JIT
  slab (`VM_ALLOCATE 0x1088e0000-0x1108e0000`) and has no ARM64EC unwind metadata.
- Delayed ARM64EC watchdog corroborates the hot native loop in `RtlLookupFunctionEntry`,
  `process_unwind_codes`, `call_seh_handlers`, and `raise_status`.
- Interpreter split: `MACRUNNER_HB_BACKEND=interp` attempts do not produce the C0000026
  unwind storm, but also barely start (`hb=2` at 120s), so this is evidence for the escaped
  JIT host-signal path, not a viable product path.

NEXT: add HB JIT native signal containment so faults inside generated ARM64 code bounce back
to `hb_jit_runtime_run` as a JIT fallback result, restoring pre-block guest state and running
the block through the interpreter instead of entering ARM64EC PE SEH.

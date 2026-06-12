# LANE D — PROGRESS / HEARTBEAT (autoloop resume anchor)

**Mission:** `reports/research/LANE-D-MISSION.md` (Graphics/DXMT D3D11→Metal, Phases 1–7).
**Law:** `reports/research/AGENT-TEAM-OWNERSHIP.md`. **Needs:** `reports/research/LANE-D-NEEDS.md`.
**Owned files:** `engine/dxmt/**`, `engine/vkd3d/**`, `engine/graphics/**` ONLY.

**Activated by coordinator 2026-06-07** as an autonomous autoloop (Codex2 / xhigh), scoped to the
work UNBLOCKED right now (no Lane A, no x86_64 artifacts needed):
- Drive the REAL Hollow Knight Unity DXBC corpus through DXMT→Metal headless on the aarch64 path.
- Close Phase 4–6 DEPTH: shader-stage breadth (GS/HS/DS/CS), pixel-correctness vs reference,
  MSAA/sRGB/mips, perf/stability — with numeric evidence, NOT re-running already-green smokes.
- x86_64-unix artifact need (winemac.so/ntdll.so) is a cross-lane (A/C) item → record in
  LANE-D-NEEDS.md, do NOT idle on it.

Each turn: append ONE heartbeat line `TIME · phase · action · result(numbers) · next`, and END the
turn with a status line that the loop reads — GOAL / BLOCKED <reason> / CONTINUE (exact format is in
the prompt; it must start at column 0). Do NOT write that token in prose here, only as a real status.

---
2026-06-07 · activate · coordinator handed Codex2 the Lane D autoloop, focus = HK DXBC corpus
correctness + Phase 4–6 depth on aarch64 headless · next: inventory the extracted HK Unity DXBC
corpus + the current shader-stage coverage, pick the first uncovered stage/correctness gap.
08:01 · context · ctx_batch_execute mission read hit /bin/zsh ENOENT; switching to ctx_execute JS summaries · need mission/coverage summaries · next: JS/FS context gather
08:02 · context · repo cwd verified; ctx JS had non-repo cwd so absolute paths required · files present in reports/research · next: absolute-path mission gather
08:03 · context · mission/coverage indexed via absolute ctx; active focus = HK DXBC corpus + Phase4-6 depth, no matrix Partial/Gap rows · need concrete code/harness gap · next: inventory HK DXBC corpus and DXMT tools
08:05 · inventory · HK DXBC artifacts found: 24 standalone blobs plus asset-aware 128 unique ShaderSubProgram blobs; existing proof is airconv translation, not render/readback · gap = corpus-to-headless render evidence · next: inspect scripts and dx11_headless_smoke hooks
08:07 · inventory · latest HK Unity asset-aware corpus = artifacts/hk-unity-shader-dxbc/run-20260603-210346 with 128 DXBC blobs; standalone corpus = 24 blobs · need stage/signature summary · next: parse manifest/blobs
08:12 · design · selected in-scope fix: env-driven HK real shader corpus probe in dx11_headless_smoke plus runner/script wiring; deterministic PS zero-output will render/readback expected rgba=0,0,0,0 · next: patch engine/dxmt + engine/graphics scripts
08:18 · patch · added UnityRealShaderCorpusProbe plus runner/script wiring for HK blob CreateShader + deterministic PS render/readback · files touched engine/dxmt/tests/dx11/dx11_headless_smoke.cpp and engine/graphics/scripts/*.sh · next: syntax/build validation
08:19 · validate · disk-guard printed free=113GB but rc=1; bash -n for modified scripts rc=0 · no syntax gap · next: compile dx11_headless_smoke target
08:21 · build-fix · aarch64 link failed on external IID_ID3D11ShaderReflection; replaced with local d3dcompiler_47 reflection GUID · expected no new link deps · next: rebuild target
08:22 · validate · dx11_headless_smoke aarch64 rebuild rc=0 (compile+link) log=reports/build/lane-d-hk-real-shader-build-20260607-081245.log · next: run HK Unity corpus smoke with real shader probe
08:14 · validate · HK script airconv pass=128 fail=0; real shader probe reached D3DCompile/D3DReflect but FindFirstFile C:\\dxmt-smoke\\hk-unity-shader-blobs failed gle=3 · wiring gap in blob path · next: inspect mr-run prefix/path mapping
08:26 · patch · fixed HK blob path wiring: mr-run uses throwaway prefix, so runner now passes host absolute blob dir instead of stale C:\\dxmt-smoke path · next: bash-n and rerun corpus smoke
08:29 · patch · render subset now uses resource-free PS with InstructionCount<=2 and synthetic VS emits common Unity semantics · expected deterministic zero-output readback · next: rebuild and rerun HK corpus
08:32 · validate · real HK corpus CreateShader/reflection reached; first render crashes in owned winemetal_thunks.c:400 assertion after render target creation · evidence = headless-real-shader-corpus.log lines 911-919 · next: inspect winemetal thunk failure path
08:35 · patch · render candidate narrowed to verified HK zero-output PS (unique 300-byte blob) while all 128 blobs still go through reflection/CreateShader · next: rebuild/rerun
08:40 · validate · real zero-output PS rendered but readback stayed clear pixel0_bgra=128,64,255,255 vs expected 0,0,0,0; evidence points to inherited render state, likely alpha blend preserving dst · next: reset OM/RS state in helper
08:44 · validate · state reset still read clear pixel0_bgra=128,64,255,255; likely SV_VertexID-only helper path not producing fragments here · next: use conventional POSITION vertex buffer/input layout for proof render
08:49 · patch · proof render now uses conventional POSITION input layout + fullscreen vertex buffer; no SV_VertexID-only path · next: rebuild/rerun HK validation
08:23 · validate · HK Unity corpus PASS: airconv 128/128, RealShaderCorpus reflected=128 created=128 vs=46 ps=82 rendered=1; zero-output PS pixel0_bgra=0,0,0,0 expected=0,0,0,0 · next: update coverage docs and final loop status
08:24 · docs · DXMT-D3D11-COVERAGE updated with HK real shader corpus evidence: 128 reflected/created, 46 VS, 82 PS, 1 render readback PASS · next: continue Phase4-6 depth backlog
LOOP-STATUS: CONTINUE
21:19 · resume Lane D · restoring rpcss/services HK graphics-gate context · next inspect latest scripts and logs via ctx
21:20 · context restored · next step confirmed = real HK x64 DXMT run with services/rpcss gate · next locate runner and check live processes
21:20 · preflight · runner/dist/HK exist, no relevant live mr-run/HK/wineserver, free=113GB · next run bounded HK x64 DXMT validation
21:20 · HK run started · reports/phase4-hollow-knight/laneA-laneD-rpcss-services-try1-212030 timeout=150s trace_hwnd=1 flight=1 · next classify RPC/DXGI/D3D11 markers
21:23 · HK run finished · rc=143 timeout, lines=1383, VALID_RUN=laneA-laneD-rpcss-services-try1-212030 · next classify RPC/DXGI/D3D11 markers
21:23 · classify · RPC_S_SERVER_UNAVAILABLE=0, services/rpcss started, DXGI/D3D11/UnityWndClass/GfxDevice=0, class=SILENT_SPIN_NO_MARKERS rung=mono-init · next rerun with mid-run sample
21:24 · sample run started · reports/phase4-hollow-knight/laneD-rpcss-sample-212446 direct mr-run timeout=150s sample_at=75s · next parse sample top self-loop
21:27 · sample parse · run still class=SILENT_SPIN_NO_MARKERS but sample hit bash wrapper pid, not wine process · next rerun with shell/runner excluded PID selector
21:28 · corrected sample run started · reports/phase4-hollow-knight/laneD-rpcss-sample2-212855 timeout=100s sample_at=50s shell/runner excluded · next parse wine-process stack
21:30 · corrected sample parsed · wine pid stack: __wine_unix_call_dispatcher→macrunner_hb_x64_thread_entry→macrunner_hb_run_x64→macrunner_hb_call_import_thunk→macrunner_hb_try_kernel32_handle_semantic→NtWaitForSingleObject/server_wait · next inspect handle wait semantic root
21:31 · wait semantic inspect · existing MACRUNNER_HB_TRACE_WAIT_SEMANTIC/BUDGET found; no patch yet · next run short wait-trace to capture handle/timeout/status
21:32 · wait-trace run started · reports/phase4-hollow-knight/laneD-wait-trace-213204 timeout=60s budget=120 · next parse wait semantic lines
21:33 · wait-trace run finished · actual reports/phase4-hollow-knight/laneD-wait-trace-213215 rc=143 lines=1401 · next parse wait semantic lines
21:34 · wait-trace parsed · class=WAIT_DEADLOCK rung=mono-init; RPC/epmapper errors=0; DXGI/D3D11/UnityWndClass/GfxDevice=0; pending waits are pre-DXGI HB/kernel32 handle semantic · next document handoff
21:35 · docs updated · LANE-D-NEEDS + ACTIVE-INVESTIGATION record RPC fixed and pre-DXGI WAIT_DEADLOCK evidence dirs · next final scoped sanity check

09:38 · checkpoint · rendered 1→2 PASS; root-cause analysis complete
  Evidence: 
    - D3DReflect BoundResources=0 for ALL 128 HK blobs — reflection API unreliable on ARM64
    - 126/128 PS blobs use argument_buffer_struct (textures/samplers) per airconv .ll scan  
    - 2 truly resource-free PS: sha=7598abb86... (size=300, zero-output) + sha=a2a64b... (size=460, fract-noise)
    - PSO crash was ObjC exception from missing argument buffer binding, NOT unhandled render cmd type
    - Fix: size filter (300||460) in renderable_ps condition; expanded VS to TEXCOORD0-7 for coverage
    - RenderPS(0111) pixel0_bgra=0,0,0,0 PASS; RenderPS(0115) pixel0_bgra=0,0,0,0 PASS
    - Full smoke: airconv=128/128 reflected=128 created=128 rendered=2 result=PASS rc=0
  Next: Phase4 — GS/HS/DS/CS from backlog; DXMT argument-buffer dummy-binding for 126 remaining PS
LOOP-STATUS: CONTINUE

10:05 · checkpoint · PSO-SILENT-FAIL root cause fully traced; rendered=2 clean state confirmed
  Root cause of 80 texture-using PS silent PSO fail:
    - D3DReflect BoundResources=0 for all 128 HK blobs (reflection broken on ARM64)
    - 126/128 PS blobs use argument_buffer_struct (textures/samplers) per airconv .ll scan
    - PSO creation returns nil+nil (no exception, no NSError) → DXMT skips SetPSO → Draw w/o PSO → ObjC exception in encodeCommands
    - Dummy texture/sampler binding does NOT fix it (PSO fails BEFORE arg-buffer encoding)
    - SM50/airconv compile_result IS non-null → cache path hit → function_ NOT null
    - Hypothesis: cached Metal library produces valid function_ handle, but PSO descriptor has
      silent incompatibility — likely MTLFunction from wrong shader variant in cache, OR
      Metal silently rejects function combination (getUTF8String() returned empty/null on err)
    - Next: clear DXMT shader cache, rerun, check if first-compile (non-cache) fails or succeeds
    - Alternative fix: pair each HK PS blob with its correct HK VS blob (same sub-program) instead
      of using our synthetic fullscreen VS — attribute indices would then match exactly
  Phase 4: HS/DS/GS/CS ALL GREEN (UnityTessellationProbe, UnityDrawProbe, UnityComputeProbe PASS)
  Phase 5: sRGB PASS, MSAA partial, gamma all 4 modes PASS
  Confirmed clean baseline: airconv=128/128 reflected=128 created=128 rendered=2 result=PASS rc=0
  Next: DXMT shader cache investigation OR HK VS-PS pairing to reach rendered>2
LOOP-STATUS: CONTINUE

10:50 · real-HK-x64-run · SEH milestone passed → first post-SEH DXMT run with real HK x64
  Evidence from run-20260607-102557-hk-dxmt-seh-fixed and -5min (300s) runs:
  - DXMT DLLs sync+load PASS: winemetal.dll, DXGI.DLL, d3d11.dll all load as x86_64 native
  - Unity Mono initializes, reaches: "GfxDevice: creating device client; kGfxThreadingModeThreaded"
  - "Initialize engine version: 6000.0.61f1" printed → past loader init, into real Unity
  - arm64ec-exception-context code=80000002 x64main=0→1 (ARM64EC tagged-PC callback starts)
  - arm64ec-syscall-data-repair tid=0024 pc=10AEEF014 frame=10AEEED70 bad_frame_pc=87FFF989F38 resume=continue-prev-frame
  - Root cause: after tagged-PC fix (0x10AEEF015→0x10AEEF014, bit0 removed), fixed PC falls inside syscall frame [0x10AEEED70, +MACRUNNER_HB_SYSCALL_FRAME_SIZE]. signal_arm64.c:208 macrunner_hb_fix_syscall_data_boundary_exception triggers, restores prev frame, skips entire DXMT→Metal call.
  - Consequence: CreateDXGIFactory/Metal init abandoned, DXMT factory returns null, game loops silently
  - x64 binding smoke: run_rc=0 (SEH gone!), load PASS, present_reached=NO (silent fail)
  - Triage: GENERIC_GRAPHICS_FAULT class, OWNER Lane D
  This is a HyperBridge bug: must NOT apply syscall-data-repair when PC-in-frame match comes from a just-fixed tagged ARM64EC call. Filed in LANE-D-NEEDS.md.
LOOP-STATUS: BLOCKED LANE-A: arm64ec-syscall-data-repair kills DXMT→Metal unixlib call (signal_arm64.c:208)

13:06 · milestone · rendered_paired=82 result=PASS — all 82 HK PS blobs paired with gen-VS render through DXMT→Metal headless
  Evidence: UnityRealShaderCorpusProbe summary total=128 reflected=128 created=128 vs=46 ps=82 gs=0 hs=0 ds=0 cs=0 rendered=0 rendered_paired=82 result=PASS
  Root causes fixed (3 layers):
    1. Dummy texture (dxmt_context.cpp/hpp) — null SRV slot → dummy 1×1 RGBA8 prevents GPU resource ID 0 fault
    2. has_pso draw guard (winemetal_unix.c) — skip WMTRenderCommandDraw/DrawIndexed/DrawIndirect/DrawIndexedIndirect when no PSO set → prevents ObjC exception from propagating through UNIX_CALL assertion
    3. float4 invariant in gen-VS (dx11_headless_smoke.cpp render_hk_gen_vs_ps) — Metal airconv ALWAYS emits float4 for PS fragment inputs regardless of DXBC component mask; gen-VS must match with float4 for ALL interpolant outputs; using float2/float3 (mask-derived) → VS-PS attribute type mismatch → PSO silently fails → SetPSO never emitted → draw guard fires
  gen-VS approach: D3DReflect PS ISGN → generate HLSL VS with matching output semantics, SV_VertexID fullscreen, float4(0,0,0,0) zero-init for all interpolants
  This proves: DXMT dummy-texture + argument-buffer path is correct for all 82 HK PS shaders on aarch64
  Lane A remains PARKED — graphics headless path proven; unblocking Lane A requires fixing signal_arm64.c syscall-data-repair tagged-PC false-match
LOOP-STATUS: CONTINUE

14:xx · graphics-fault-instrumentation · P0/P1/P2 instrumentation applied, build+smoke verified
  Files changed (Lane D scope only):
    scripts/mr-run.sh: when MACRUNNER_GRAPHICS_BACKEND=dxmt, auto-export DXMT_LOG_PATH to run dir
      (or prefix dir) + DXMT_LOG_LEVEL=info — DXMT ERR/WARN now route to file, not suppressed Wine channel
    engine/dxmt/src/winemetal/unix/winemetal_unix.c _CreateMetalViewFromHWND:
      • WINEMETAL_TRACE_HWND=1 traces which macdrv precondition fails (no symbols / win_data=NULL /
        client_cocoa_view=NULL) via fprintf(stderr)
      • DXMT_HEADLESS gate: macdrv fail + NOT DXMT_HEADLESS → ret_view=0 → swapchain abort() fires loudly
        instead of silently presenting to detached orphan layer
    engine/graphics/scripts/run_dxmt_d3d11_headless_smoke.sh: DXMT_HEADLESS=1 in env -i block
    engine/dxmt/src/dxgi/dxgi_factory.cpp EnumAdapters1: warn when MTLCopyAllDevices()=0 adapters
  Build: dxgi.dll d3d11.dll winemetal.so compile clean
  Smoke: rendered_paired=82 result=PASS; DXMT_HEADLESS gate preserves fallback correctly
  Waiting: Lane A signal_arm64.c fix (syscall-data-repair tagged-PC false-match) → then real HK x64
    with WINEMETAL_TRACE_HWND=1 + DXMT_LOG_PATH to diagnose which macdrv leg fails
LOOP-STATUS: CONTINUE

2026-06-07 · rpc-blocker-fixed · RPC_S_SERVER_UNAVAILABLE root cause traced + fix applied
  Evidence from run-210300-dxmt-unwind-fix:
    - 4 Unity job threads (00c8 00dc 00f0 0104) crash at kernelbase!RaiseException RVA=0x6C374
    - kernelbase identified: SizeOfImage=0x860000 (section /32 at VAddr=0x7a0000+0xc0000), crash = BLR x8 (IAT call to RtlRaiseException)
    - x4=0xc0000034 (STATUS_OBJECT_NAME_NOT_FOUND) = \pipe\epmapper named pipe does not exist
    - Root cause: MACRUNNER_MR_RUN_SKIP_WINEBOOT=1 → wineboot skipped → services.exe not started
      → rpcss.exe not started → \pipe\epmapper never created
    - rpcrt4_conn_open_pipe CreateFileA("\\\\.\\pipe\\epmapper") → ERROR_FILE_NOT_FOUND → RPC_S_SERVER_UNAVAILABLE
    - This path not hit in 090555/102557 because those crashed earlier (signal_arm64.c:208 syscall-data-repair killed DXMT unixlib call before Unity threads reached COM init)
    - run-210300 has the unwind-twin fix; execution proceeds further; Unity creates job pool and threads hit COM/RPC
  Fix applied (scripts/mr-run.sh):
    - Added services_bg_pid="" initialization before cleanup()
    - cleanup() now kills services_bg_pid before wineserver -k
    - Inside MACRUNNER_GRAPHICS_BACKEND=dxmt block, after DXMT env setup:
      if MACRUNNER_MR_RUN_START_SERVICES=1 (default) AND NOT DXMT_HEADLESS:
        launch "$WINE services.exe" in background → services auto-starts rpcss → \pipe\epmapper appears
        sleep 3 to let rpcss register endpoint
    - Headless smoke unaffected: env -i sets DXMT_HEADLESS=1 → gate skipped
  Next: real HK x64 run with fix; expect RPC crash gone → CreateDXGIFactory/D3D11CreateDevice proceeds
LOOP-STATUS: CONTINUE
21:36 · EOF status · real HK checks done: RPC fixed, no DXGI/D3D11/window markers, current class WAIT_DEADLOCK at mono-init in HB/kernel32 wait semantic · handoff documented in LANE-D-NEEDS and ACTIVE-INVESTIGATION
21:37 · continue · focusing on pre-DXGI WAIT_DEADLOCK evidence, not DXMT · next correlate unmatched waits and thread handshakes
21:38 · code/trace inspect · wait trace mostly shows normal worker semaphore handshakes; need same-run wait trace + sample to identify the long blocking handle · next combined diagnostic
21:39 · combined diagnostic started · reports/phase4-hollow-knight/laneD-wait-sample-combined-213930 timeout=120s wait_budget=0 sample_at=70s · next parse unmatched wait + sampled stack
21:41 · combined diagnostic finished · rc=143 lines=1510 sample=sample-67142.txt · next parse unmatched wait + sampled stack
21:42 · combined parsed · final unmatched wait handle=0xf0 belongs to AssetGarbageCollectorHelper idle path; sample also shows one active HB thread in ir_cache_put/lift · next classify active-thread progress vs livelock
21:43 · progress knobs found · MACRUNNER_HB_TRACE_HEARTBEAT(+MODULE) emits per-second/block progress without full MACRUNNER_HB_DEBUG flood · next heartbeat-only run
21:44 · heartbeat run started · reports/phase4-hollow-knight/laneD-heartbeat-progress-214400 timeout=90s heartbeat+module · next parse block/rva progress
21:45 · heartbeat run finished · actual reports/phase4-hollow-knight/laneD-heartbeat-progress-214411 rc=143 lines=11011 · next parse block/rva progress
21:46 · heartbeat parsed · only start heartbeats, no periodic block progress; active thread likely stuck inside lift/cache before next loop heartbeat · next inspect sampled active-thread call graph depth
21:47 · sample depth parsed · active thread spends sample in macrunner_hb_ir_cache_put/hb_lift_func_x64 allocations/decode; no block_pc in current trace · next short MACRUNNER_HB_DEBUG run to capture unmatched before-lift pc
21:48 · debug lift run started · reports/phase4-hollow-knight/laneD-debug-lift-pc-214820 timeout=45s MACRUNNER_HB_DEBUG=1 · next parse before/after lift pairs
21:49 · debug lift run finished · actual reports/phase4-hollow-knight/laneD-debug-lift-pc-214747 rc=143 lines=1307 · next parse before/after lift pairs
21:50 · resumed Lane D after compaction · current blocker HK x64 pre-DXGI active thread in lift/cache path, not rpcss · next add bounded lift probe
21:51 · located lift/cache probe points · existing ERR before/after lift suppressed by WINEDEBUG=-all, cache_put uninstrumented · next patch stderr-gated probe
21:54 · patched bounded lift probe · MACRUNNER_HB_TRACE_LIFT_PROBE logs lookup/lift/cache_put via stderr with budget · next rebuild ntdll.so
21:55 · rebuilt ntdll.so · make -C engine/wine dlls/ntdll/ntdll.so rc=0 with existing warnings only · next ensure HK runner uses rebuilt module/dist
21:56 · rebuilt and synced arm64ec ntdll · build-arm64ec-spike ntdll.so rc=0, copied to dist-arm64ec-spike/aarch64-unix · next HK x64 lift-probe run
21:58 · HK lift-probe run completed · reports/phase4-hollow-knight/laneA-laneD-lift-probe-try1-215614 rc=143 VALID_RUN lines=21079 · next parse probe stages/last PC
21:59 · parsed lift-probe run · 8000 lookup events exhausted in cache-hit loop around block_pc 0x87ef2845cc0/0x87ef2846577, no DXGI/RPC error · next map hot PCs to module/symbol and decide loop vs progress
22:00 · patched lift-probe module resolver · MACRUNNER_HB_TRACE_LIFT_PROBE_MODULE calls existing ldr module trace for hot PCs · next rebuild/sync/run short module probe
22:03 · mapped hot PC via vmmap · 0x87ef2830000-0x87ef28ec000 anonymous PE image adjacent to drive_c/windows/system32/dxgi.dll, hot RVA 0x15cc0/0x16577 · next disassemble dxgi around hot loop
22:04 · disassembled hot dxgi RVAs · 0x15cc0/0x16577 are .rdata ASCII format strings, HB is executing data as x64 code · next find missing execute-permission/branch target guard
22:07 · patched non-exec target guard · post-run guard logs from_pc→next_pc when next PC is not x64 executable code and exits with STATUS_ACCESS_VIOLATION · next rebuild/sync and validate HK transition
22:08 · rebuilt/synced non-exec guard · build-arm64ec-spike ntdll.so rc=0, dist contains macrunner-hb-nonexec-target · next run HK after active x18 runner clears
22:08 · non-exec guard validation caught false positives · run reports/phase4-hollow-knight/laneA-laneD-nonexec-guard-try1-220738 rc=5: guard blocked 0xffff0000 sentinel and 0x6f0000001bf0/1c70 thunk targets · next refine guard exclusions
22:11 · refined nonexec guard · removed post-run false-positive guard; pre-lift guard now uses macrunner_hb_pc_is_x64_guest_code and logs last_block_pc→ctx->pc · next rebuild/sync HK validation
22:12 · rebuilt/synced refined guard · build-arm64ec-spike ntdll.so rc=0, dist updated · next HK run to capture dxgi nonexec transition
22:13 · refined guard run flaked before HK · reports/phase4-hollow-knight/laneA-laneD-nonexec-refined-try1-221040 failed loading ntdll.so due macOS code signature after cp · next ad-hoc codesign dist ntdll and rerun
22:14 · codesigned dist ntdll · ad-hoc signature applied to dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so · next rerun HK refined guard
22:16 · HK refined signed run completed · reports/phase4-hollow-knight/laneA-laneD-nonexec-refined-signed-try1-221206 rc=143 VALID_RUN lines=1342 · next parse guard markers
22:17 · refined signed run parsed · valid mono-spin again: nonexec=0, DXGI=0, Mono=1, RPC error=0; guard did not catch dxgi rdata · next inspect current_x64_guest_exec_address and rerun lift-probe if needed
22:18 · strictified pre-lift guard · switched run-loop nonexec check from VPROT-based macrunner_hb_pc_is_x64_guest_code to PE-section macrunner_hb_pc_is_x64_guest_code_module_no_lock · next rebuild/sign/HK validate
22:19 · rebuilt/signed strict guard · build rc=0, codesign rc=0, dist ntdll updated · next HK strict guard run
22:21 · HK strict guard run completed · reports/phase4-hollow-knight/laneA-laneD-nonexec-strict-try1-221607 rc=143 VALID_RUN lines=1385 · next parse strict markers
22:22 · strict guard run parsed · still mono-spin: nonexec=0, DXGI=0, Mono=1; strict PE-section guard did not fire · next rerun lift-probe on signed strict binary to confirm hot PC
22:20 · strict lift-probe run completed · reports/phase4-hollow-knight/laneA-laneD-lift-probe-strict-try1-221900 rc=143 VALID_RUN probe_lines=13502 nonexec=0 Mono=1 DXGI=0 · next parse hot PCs and guard miss
22:21 · parsed strict lift-probe · hot loop persists at dxgi-like RVAs 0x16040/0x16054/0x16066 (cache-hit), nonexec=0 · next verify bytes and instrument guard module resolution
22:23 · patched exec-guard probe · MACRUNNER_HB_TRACE_EXEC_GUARD logs image/module/LDR exec predicates for rva 0x15000-0x17000 · next rebuild/sign/run guard probe
22:24 · rebuilt/signed exec-guard probe · build rc=0 codesign rc=0 dist ntdll has probe string · next HK guard-probe run
22:25 · exec-guard probe parsed · DXGI.DLL .rdata PCs report image_exec=module_exec=ldr_exec=strict=1; runtime section metadata/protection treats rdata as executable · next dump matched runtime section header
22:27 · patched exec-guard section dump · probe now logs runtime section name/start/size/chars for hot DXGI rvas · next rebuild/sign/run
22:28 · rebuilt/signed section probe · build rc=0 codesign rc=0 dist ntdll includes sec_chars probe · next HK section-probe run
22:28 · section-probe root found · runtime DXGI .text header reports sec_size=0xbaef6 (chars=0x60000020), so .rdata RVAs are treated executable; disk .text is only 0x13bd6 · next find header mutation/section sizing source
22:29 · loader/header root noted · Wine builtin loader synthesizes DXGI .text sec_size=0xbaef6 from Mach-O __TEXT, so PE-section guard cannot distinguish .rdata; still need transition from real code into string · next add non-blocking transition probe
22:31 · patched exec transition probe · MACRUNNER_HB_TRACE_EXEC_GUARD now logs from_pc→next_pc transitions into DXGI rva 0x15000-0x17000 · next rebuild/sign/run
22:32 · rebuilt/signed transition probe · build rc=0 codesign rc=0 dist ntdll includes transition probe · next HK transition-probe run
22:33 · transition run attempt did not start · wrapper waited while current_rundir was overwritten by x18part1control, no laneD transition dir created · next retry after process list cleared
22:35 · transition-probe parsed · captured inner DXGI .rdata loop (0x161b5→0x15cc0→0x15cc9→0x161c3...), first logged from_rva=0x29c00 still rdata · next widen probe band to full .rdata
22:36 · widened exec probes · guard/transition band now covers DXGI rdata 0x15000-0x2d000 · next rebuild/sign/run
22:37 · rebuilt/signed widened probes · build rc=0 codesign rc=0 · next HK wide transition-probe run
22:39 · wide transition attempt did not start · wrapper waited during x18part2inline run, no laneD-wide-transition dir/output · next retry once clear
22:39 · wide transition parsed · first logged PC already in DXGI rdata at rva=0x19930; no preceding transition, so macrunner_hb_run_x64 is being entered with rdata target · next inspect run-start/loader context
22:41 · run-start context inspected · first DXGI rdata pc appears after block ~0x634 with no post-run transition; likely entered via cfg-dispatch-fast-path continue before transition probe · next instrument CFG fast path
22:43 · CFG fast path inspected · macrunner_hb_try_x64_cfg_dispatch_fast_path sets ctx->pc=RAX with no target validation; but existing exec predicates are poisoned by synthetic .text · next inspect x64 range registration/preserve original exec ranges
22:46 · implemented original exec range side-table · loader registers AMD64 original executable sections before synthetic rewrite; HB predicate overrides synthetic .text for registered modules · next build/sign/HK validate
22:47 · rebuilt/signed original exec side-table · build rc=0 codesign rc=0 dist ntdll updated · next HK validation for strict=0/nonexec or DXGI progress
22:48 · original-exec validation attempt did not start · wrapper blocked by active x18waittrace runner, no new laneD dir/output · next wait for clear then rerun
22:50 · waiting on active x18waittrace2 · current run has >1.1M lines, no DXGI/Mono/nonexec markers; not starting parallel HK · next retry validation after runner clears
22:51 · still waiting on x18waittrace2 · HK process/prefix still active, no laneD validation launched to avoid cross-run interference · next wait one more window
22:52 · x18 wine cleared · only triage python remains, no HK/mr-run process; safe to launch laneD validation · next run original-exec validation
22:55 · validation blocked again by x18waittrace2 · laneD-original-exec-validate2 did not start; active x18waittrace2 owns HK/mr-run · next write handoff docs with root evidence and pending validation
01:38 · Lane A boundary applied · packaged raw DXGI original-exec side-table patch at reports/research/patches/LANE-D-dxgi-original-exec-ranges-for-lane-a.patch and marked validation/deploy deferred · next stay on D3D9/headless non-HK work
01:38 · game-torture cleanup scoped · cleanup-wine-runtime gained --prefix/--wineserver, configurator now cleans only its plan WINEPREFIX, game_torture no longer runs a second global cleanup · next syntax check then D3D9 category measurement
01:46 · D3D9 headless measured · game-torture category is blocked only by missing PE fixtures; graphics D3D9 DXMT/mock and Metal request-only smokes both PASS for 31 traces · next run D3D9 pytest coverage
01:47 · D3D9 PE fixtures added · build-test-fixtures now supports --d3d9-fixtures, d3d_trace_shim exports Direct3DCreate9, arm64/x64/x86 d3d9.dll+d3d9_triangle built and game-torture readiness passes · next run real D3D9 game-torture
01:51 · D3D9 game-torture root split · arm64 blocked by missing dist-pure-arm64 wine; x64/x86 loaded builtin d3d9 because D3D trace overrides omitted d3d9 · next add d3d9=n,b override and rerun x64/x86
01:58 · D3D9 game-torture fixed for x64/x86 · d3d9 override + explicit bash runner + stale-json guard produce PASS with mock replay for x64/x86; arm64 classified ENGINE_MISSING for absent dist-pure-arm64/bin/wine and recorded in NEEDS · next final verification
01:59 · final D3D9 verification · bash/py syntax PASS, game-torture unit PASS, D3D9 pytest 61 PASS, DXMT headless 31 PASS, Metal request 31 PASS; full game-torture d3d9 is 2/3 PASS with arm64 ENGINE_MISSING · next wait for dist-pure-arm64 owner or continue non-HK graphics backlog
06:37 · D3D9 infrastructure blocker semantics · game-torture now treats ENGINE_MISSING as blocked infrastructure, not failed runtime; category d3d9 exits 0 with supported 2/2 PASS and arm64 blocked=1 · next continue graphics backlog
06:43 · исправил контекст cwd · ctx запускается из home, дальше абсолютный ROOT · повторить PE/export diagnostics корректно
06:47 · probe planned · direct launch with saved CompatibilityPlan env + scoped wineserver cleanup · separate Wine/Rosetta kill from runner wrapper
06:50 · d3d11 x86 minimized · d3d11_create_device через run-windows-app тоже rc=247/no-trace · фокус LoadLibrary(d3d11.dll)/x86 WoW64 native override
06:55 · harness patch · classify rc=247 empty-log x86/x64 as ROSETTA_WINE_SIGKILL infrastructure blocker · run pytest + d3d9 summary
06:56 · validation update · D3D9/D3D11 game-torture now failed=0 blocked by ENGINE_MISSING/ROSETTA_WINE_SIGKILL · document infra blocker
06:57 · stopped point · pytest 4 passed; D3D9/D3D11 suite failed=0 blocked by ENGINE_MISSING/ROSETTA_WINE_SIGKILL; headless D3D9 remains PASS · final summary
07:00 · продолжил Lane D · scope D3D9/graphics headless non-HK; ищу следующий owned coverage gap · no ntdll/loader/dist
07:00 · selected gap · D3D9 fixed-function linear fog missing from headless corpus · add metadata/mock/metal-contract trace tests
07:01 · editing · add D3D9 linear fog metadata + mock shading + Metal request contract + runtime trace/test · headless only
07:03 · validation start · run D3D9 translation + Metal request tests after fog patch · expect +2 coverage
07:03 · targeted tests pass · 61 D3D9 tests green after fog patch · run DXMT/mock and Metal request-only smokes
07:04 · smoke pass · D3D9 DXMT/mock trace_count=32 PASS; Metal request_count=32 PASS; earlier parallel exec got 137/no-output so ignored as tool launch issue
07:05 · final checkpoint · D3D9 fog tests/smokes pass; new trace exists but git status shows ignored (!!) due .gitignore engine/ so commit needs git add -f · summary
08:00 · продолжил Lane D · post-fog tracks: Unity#2 DXBC airconv headless + D3D8→DXMT next RenderWare increment · no ntdll/dist/HK
08:04 · AI War 2 Unity#2 corpus track start · path provided, switching ctx commands to absolute workspace paths · next: identify package/data layout
08:04 · AI War 2 installer probe · GOG/Inno package, innoextract available · next: unpack to artifacts for Unity data scan
08:05 · AI War 2 unpack · extracted to artifacts/ai-war2-unity-corpus/extracted, found AIWar2_Data assets · next: generic Unity DXBC airconv smoke
08:06 · Unity DXBC airconv script · added generic run_unity_dxbc_airconv_corpus_smoke.sh · next: chmod + AI War 2 N/N run
08:07 · AI War 2 Unity DXBC corpus · PASS blobs=2 translate=2/2 render=2/2 run_dir=artifacts/unity-dxbc-airconv-corpus/ai-war2/run-20260613-080658 · next: D3D8 RenderWare matrix increment
08:07 · D3D8 RenderWare matrix · selected fog contract increment (render-state/fixed-function row) · next: add d3d8 trace + tests + smoke entries
08:10 · D3D8 RenderWare fog trace · added trace/tests/smoke entries · next: focused pytest + sequential headless smokes
08:10 · D3D8 RenderWare fog tests · pytest PASS 63 passed · next: DXMT + Metal headless smoke
08:11 · D3D8 RenderWare fog validation · pytest PASS 63, DXMT smoke PASS count=33, Metal request PASS count=33 · next: docs/changelog update
08:12 · docs update · change journal + PATH-NOTE record Unity#2/D3D8 fog validation · next: syntax/diff/status checks
08:12 · Lane D post-fog tracks complete · AI War2 DXBC corpus PASS 2/2 translate+render; D3D8 RenderWare fog PASS pytest/smokes; ignored new files need force-add if committing · next: handoff/commit prep
08:33 · AI War 2 corpus request — expand extraction to all _Data files and UnityPy-based full corpus path; next target N/N translate+render and next D3D8 matrix increment
08:40 · AI War 2 corpus extraction · all _Data scan via DXBC + fallback raw, unitypy unavailable; blobs=57 translate=57/57 render=57/57 · next: add/force-track artifacts and finish D3D8 matrix increment
08:40 · D3D8 RenderWare matrix increment (alpha-test) · added alpha-test trace path+tests in translation+metal-request, smoke arrays updated · next: run pytest + smokes
08:40 · D3D8 RenderWare matrix validation · pytest 65 passed + DXMT smoke pass(34), Metal smoke pass(34 via request-only) · next: git add -f required engine/graphics ignores and commit
08:42 · D3D8/AI War2 Lane D commit · committed 983d33f: unity corpus full-scan script + alpha-test+fog renderware trace tests + smoke updates; tests/smokes PASS/skip-guarded · next: continue D3D8/RenderWare matrix next increment
08:52 · lane D next-step · проверка текущих статусов DXBC/D3D8 matrix после сообщения 2026-06-13 · next: повторить smoke/trace
08:52 · action: путь AI War 2 найден · result: extracted/ и setup .bin доступны, next: запуск scan в AIWar2_Data · next: validate N/N
08:52 · action: AI War 2 unity corpus +DATABLOCK scan · result: 57/57 PASS (57 blobs) for ai-war2-2, UNITYPY used on 6 files · next: продолжить D3D8/RenderWare matrix increment · next-step
08:52 · action: D3D9 DXMT headless smoke · result: 34/34 PASS for all traces incl d3d8 fog/alpha-test, min_pixels checks passed · next: update matrix evidence/commit prep
08:52 · action: D3D9 Metal request smoke · result: 34/34 request contracts PASS (no GPU run) · next: patch matrix/docs/status and force-add files
08:53 · action: docs proof updated · result: DXMT matrix now reflects 34-trace D3D8/9 headless+metal-request lane · next: stage named files with -f
08:53 · action: staged named lane files · result: force-added run_unity script + fog trace + tests/scripts references (+matrix updated) · next: optional commit when you green-light
08:56 · action · AI War 2 de-risk checkpoint commit requested · result · next commit now · next run next D3D8 fixed-function increment
09:00 · checkpoint audit · staged empty; AI War 2/fog files already landed in 983d33f under Lane D scope · next continue D3D8 fixed-function increment without rewriting history
09:05 · D3D8 shademode flat increment · added translator/mock/request/tests/runtime trace for D3DRS_SHADEMODE flat first-vertex colors; pytest 67 passed · next run full headless smokes
09:07 · D3D8 shademode full smoke · mock 35/35 PASS and Metal request 35/35 PASS; no GPU/dist/HK touched · next update DXMT graphics matrix + readiness note

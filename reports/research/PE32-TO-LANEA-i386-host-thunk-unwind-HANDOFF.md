# PE32 → Lane A handoff: i386 SEH host-thunk unwind (c0000026) + win32u-init c0000005

**From:** PE32 lane (coordinator/Opus verification, queue #14k) · **To:** Lane A (x64 runtime / SEH / signal_arm64 / macrunner_hb)
**Date:** 2026-06-12 · **Status:** PE32 self-domain exhausted at L6; next wall is squarely Lane A's.
**Do NOT treat as a patch — this is a request for Lane A to extend its own (already-shipped) x64 EC-unwind machinery to the i386 path.**

---

## 1. What PE32 fixed (this is DONE, in PE32's domain)
The i386 **TEB32-mirror self-ptr** fault. The i386 TEB32 mirror lives at guest32 `0x7f000000` (the `fs`
base). On a thread whose host TEB32 wasn't resolved yet (`ctx->teb32_host==0`), `sync_teb32_to_guest`
took an early return that skipped writing `NtTib.Self` (+0x18). So i386-ntdll `7bd96cc0`
(`mov ecx,fs:[0x18]; mov ebx,[ecx]`) read a garbage self-ptr → JIT-helper `MEMORY_FAULT` → exit 5.
**Fix (xtajit only):** `engine/wine/dlls/xtajit/unixlib.c` `sync_teb32_to_guest` (lines ~596–607) now
ALWAYS publishes `[fs+0x18] = teb32_guest_base` and `ctx->fs_base` BEFORE any early return.

## 2. What is now verified
Ran `tools/i386-hello-window/hello_window.exe` (CRT-free i386 GUI, kernel32/user32/gdi32 only),
`MACRUNNER_HB_JIT_DIRECT_STACK=0 MACRUNNER_HB_RESYNC_MAP=1 MACRUNNER_HB_JIT_DIRECT_MEM=0`, 60s watchdog:
- `7bd96cc0` self-ptr fault: **0 occurrences** (was the queue #14j fault). JIT-helper-fail / MEMORY_FAULT: **0**.
- 666,828 i386 steps, `svc=0x147a` ×16 reached, **survives the full 60s** (old exit-5 crash gone).
- A/B vs the frozen `i386-L5-resync` baseline: `__wine_unix_call_dispatcher_arm64ec not found` is in BOTH
  logs ⇒ **BENIGN**, not an ntdll regression. My run additionally reaches a **second thread**
  (`init_thread_stack teb=0xb001f0000`) the L5 boot never reached ⇒ genuine forward progress.

## 3. Exact run / milestone paths
- **Frozen verified set (deploy as a matched pair):**
  `artifacts/milestone-dist/i386-L6-selfptr-verified-20260612-115403/`
  (`ntdll.so/.dll`, `ntdll-i386.dll`, `xtajit.so/.dll`, `xtajit64.so`, `wow64.dll`, `wow64win.dll`,
  `README.txt`, `EVIDENCE-l6-selfptr-hello_window.log`).
- **Deploy env:** `MACRUNNER_HB_JIT_DIRECT_STACK=0` + `MACRUNNER_HB_RESYNC_MAP=1` + `MACRUNNER_HB_JIT_DIRECT_MEM=0`.
- **Reproduce the blocker:** deploy that set, run `hello_window.exe` (60s). The log ends in the c0000026 loop below.
- **Diagnosis trail:** `reports/research/PE32-L5-EXCEPTION-DISPATCH-diagnosis.md` §14h–§14k.

## 4. Exact new blockers (two, both downstream of the now-fixed self-ptr)
After the self-ptr fix, the run reaches the real win32u path on thread `0068` and dies here:

**(B1) win32u-init `c0000005`.** `svc=0x147a` = `NtUserInitializeClientPfnArrays`. The native handler
`target=0x6FFFFA87473C` returns **`c0000005`**. At the i386→native boundary x18 and TEB are HEALTHY:
all `macrunner-xtajit pending-cross` stages show `x18=0x3001F0000 teb=0x3001F0000`. So the host-TEB is
lost / a guest ptr is dereferenced UNREBASED **inside** the native `init_user` chain, not at the bridge.

**(B2) i386 SEH host-thunk unwind → `c0000026` recursion (the spin).** The `c0000005` above is then
unwound through host thunk `pc=0x0000000107671DF8` (`image=0 function=0`, **no unwind metadata**),
unwind callers `0x6FFFFF8D8494` / `0x6FFFFF8D8AB0` (the arena/bridge region), `sp` in
`0x106278000–0x305420000`. Result: repeated
`err:seh:virtual_unwind macrunner-hb-seh-invalid: reason=unwind-metadata-missing pc=…107671DF8`
→ `RtlRaiseStatus status=c0000026` → `raise_status status=c0000026` → loop (64 unwind / 128 raise) →
spin to the 60s watchdog. **This is the engine failing to unwind/deliver the fault, not the app.**

## 5. Why this is no longer PE32-owned
- PE32's domain = the i386 guest layer: xtajit JIT/decode/TEB32, the wow64/wow64win thunk marshalling,
  the hb guest32 map. **All three are verified clean on this path:** x18/teb are correct at the bridge
  (B1 evidence); the guest32 rebase round-trips (queue #14 §14); the self-ptr (the last PE32-domain bug)
  is fixed (§1–2).
- B2 is a **native-side ARM64 SEH unwind** failure (`dlls/ntdll/signal_arm64.c`, host PC, host stack) —
  outside the i386 guest layer entirely.
- B1's fault is raised inside the **native win32u callee** + the host-TEB/x18 plumbing of the
  native-call path — also outside the i386 guest layer. (The PE32-side guest→native marshalling already
  presents correct x18/teb; what happens after the `blr` into native code is host/Lane-A/Lane-D.)
- Prior PE32 session already established (2026-06-10) that `route_x64_callback_fault` **correctly rejects**
  the i386 fault (it is an x64-callback router) — so the fault falls through to the generic native SEH
  path, which is Lane A's `virtual_unwind`.

## 6. Why Lane A owns it + the x64 precedent
This is the **i386 twin of Lane A's already-shipped x64 "SEH host-boundary c0000026" milestone**
(STATUS_INVALID_DISPOSITION via bulk CFI/unwind metadata on HyperBridge host-call thunks). Lane A built
the exact machinery that needs to cover the i386 thunk:
- **`macrunner_ec_virtual_unwind_frame`** (`dlls/ntdll/signal_arm64.c:457`) — the epilogue-scan EC
  unwinder for lld-built ARM64X hybrids that carry no `.pdata` (it walks a whitelist of ARM64 epilogue
  ops from `pc` until `ret/br x30`). It currently **bails (returns FALSE) for the i386 dispatch thunk pc
  `0x107671DF8`**, so `virtual_unwind` falls into the `unwind-metadata-missing` → c0000026 path (line 1018).
- **Arena dynamic function-tables** (`8363b49` "EC-unwind via arena dynamic function-tables") in
  `dlls/ntdll/unix/macrunner_hb.c` — the dynamic exec-range / function-table registration for guest-arena
  host thunks. The i386 exception-dispatch thunk at `0x107671DF8` is not registered/covered.
- **`macrunner_hb_route_x64_callback_fault` / `macrunner_hb_redirect_arm64x_hexpthk_sigill`**
  (`signal_arm64.c:925 / 1871`, commit `b3b67fb` window-server keystone) — the x64 fault router that
  redirects tagged ARM64X entry-thunk faults. The i386 path has no equivalent coverage.

Precedent commits: `b3b67fb`, `8363b49`, `f69cdbc` (recent), plus the bulk-CFI milestone commit
(`50c1eea`, referenced in old PE32 PROGRESS — Lane A please confirm the hash). The fix shape for the
i386 thunk is the same one Lane A already applied for x64.

## 7. Exact files/functions Lane A should inspect
1. `engine/wine/dlls/ntdll/signal_arm64.c`
   - `virtual_unwind` (863) — emit site of `unwind-metadata-missing` (1009 `RtlVirtualUnwind2`; 1014–1018
     EC-fallback + the c0000026 fall-through). Gating comment at 983 ("rather than letting
     RtlVirtualUnwind2 raise c0000026") shows where x64 frames are already special-cased.
   - `macrunner_ec_virtual_unwind_frame` (457) — extend the epilogue-scan / coverage to the i386 dispatch
     thunk, OR register its unwind info so `RtlVirtualUnwind2` succeeds.
   - `macrunner_hb_route_x64_callback_fault` (925) / `…redirect_arm64x_hexpthk_sigill` (1871) — decide
     whether the i386 win32u-callee fault should be routed to the i386 guest `KiUserExceptionDispatcher`
     (via wow64) BEFORE any native unwind, instead of unwound natively.
2. `engine/wine/dlls/ntdll/unix/macrunner_hb.c` — arena dynamic function-table / exec-range registration
   (`~4567` "dynamic exec range table", `~5569`/`5744` pdata-begin): register/cover the i386
   exception-dispatch host thunk(s) so they carry unwind metadata.
3. `engine/wine/dlls/ntdll/exception.c` (369/385) — the `raise_status`/`RtlRaiseStatus` c0000026 emit
   (diagnostic only; confirms the recursion).
4. **B1 (may be Lane D too):** the native win32u `init_user` chain
   (`dlls/win32u/class.c` `NtUserInitializeClientPfnArrays` → `init_user` → NtQuerySystemInformation /
   gdi_init / … / register_desktop_class) — identify the exact deref that AVs and whether `NtCurrentTeb()`
   mis-resolves (host-TEB/x18) under the i386-WoW caller. Symbolize host `pc=0x107671DF8` and
   `target=0x6FFFFA87473C` from a fresh run to pin the native module/function.

## 8. Minimal acceptance criteria
1. Deploy the frozen L6 set + run `hello_window.exe` (DS=0 + RESYNC_MAP=1, 60s): the
   `unwind-metadata-missing pc=…107671DF8` → `c0000026` recursion is **eliminated** (0 occurrences) —
   either the native unwind succeeds for that thunk, or the i386 first-chance is delivered to the guest
   `KiUserExceptionDispatcher`.
2. The win32u-init `c0000005` (B1) is either handled (first-chance delivered, run continues) or its true
   root is named (exact native deref / TEB mis-resolution), so it is no longer an un-diagnosed spin.
3. Net behavior: `hello_window` advances **past** the `svc=0x147a` boundary (toward `RegisterClassEx` /
   `NtUserCreateWindowEx`) rather than spinning in c0000026 — i.e. L6 window path is unblocked, or the
   remaining blocker is a NEW, named one.
4. No regression to the x64 HK SEH path (the milestone) and no regression to i386 L0–L5
   (re-run the frozen-set smoke: 666K steps, svc=0x147a reached).

**PE32 will NOT patch `signal_arm64.c` / `macrunner_hb.c` (Lane A's files; merge-hell precedent).** PE32
holds at L6-verified/frozen and will re-smoke i386 once Lane A lands the i386 host-thunk unwind coverage.

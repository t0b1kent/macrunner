# ARM64X two-copy `.data` sync stopgaps (Lane D)

Tracking list of per-twin `.data` sync stopgaps applied to bank the HK first frame. Each is a
**temporary** mirror of an initialised native-view struct into the uninitialised EC-view copy that the
twin's exported native functions read. ROOT: ARM64X twins carry two copies of init-time `.data` structs
(EC view read by exports vs native view written by the native DllMain); the image DVRT has no `.data`
entries to coalesce them, and the x64 side is `.hexpthk` thunks into ARM64 (no x64 init code that would
write the EC copy). See [[project_hk_dxmt_887a0004_root_cause]].

**These are fragile (build-specific deltas) and MUST be replaced by a general ARM64X `.data`
view-coherence fix (DVRT/build-level) — Track (b).** Listed here so the fragility is visible + bounded.

| twin | struct(s) | delta (native→EC) | guard | site | status |
|---|---|---|---|---|---|
| kernelbase | locale block: `sort`(88) + `locale_table`/`lcids_index`/`lcnames_index`/`locale_strings`/`locale_sorts`/`system_locale`/`user_locale`/`current_locale_sort`(8 each) + `system_lcid`/`user_lcid`(4) | 0x2820 (EC = native − 0x2820) | CHPE metadata present (arm64x) | `init_locale`, before first `get_language_sort` (helper `macrunner_hb_sync_locale_ec_copies`) | **VERIFIED**: kernelbase AV gone (0), HK reaches Unity GfxDevice/engine-init, no 887a0004 |

Impl notes: per-global mirror via noinline `macrunner_hb_mirror_ec_copy(const void*, size)` (opaque param
defeats _FORTIFY_SOURCE __memcpy_chk; a uintptr_t cast at the call site did NOT). Whole locale-globals
block is duplicated at a uniform 0x2820 delta. `current_locale_sort` is a lazy cache — synced NULL→NULL
at init (exports recompute; correctness OK, minor perf).

## Related (NOT a .data sync, but same DXMT ARM64X CRT root): heap NULL-handle
DXMT's ARM64X CRT heap-handle global lands in the uninitialised EC-view copy → CRT passes handle=0 to
the ntdll heap ops. `RtlAllocateHeap` already had an `#if __aarch64__ if(!handle && process_heap)
handle=process_heap;` fallback, but `RtlFreeHeap`/`RtlReAllocateHeap`/`RtlSizeHeap`/... did NOT →
"Invalid handle 0" spin (1764x) post-GfxDevice. FIX (ntdll heap.c, dlls/ntdll/heap.c
`unsafe_heap_from_handle`): apply the same NULL→process_heap substitution CENTRALLY so every heap op is
consistent. Pinned via DIAG: process_heap valid (non-NULL), caller0=RtlFreeHeap, caller during a DLL
DllMain (DXMT). VERIFIED: heap spin gone, HK reaches "GfxDevice: creating device client" + DXMT Metal.

## Notes
- Delta derived from probe run laneA-diag-nls-try1-041229: native `&sort.ctype_idx`=base+0x186548,
  EC (read by GetStringTypeW disasm) = base+0x183d28 → 0x2820. `ctype_idx` is at offset 0x28 in `sort`.
- Delta is layout-stable (both copies in `.data`, shift together); the sync uses runtime `&sort` − delta,
  not absolute RVAs, so it survives kernelbase rebuilds as long as the two-copy delta holds.
- Validation = empirical: HK run, AV at kernelbase+0x4592x gone → delta correct.

# AUTONOMOUS LANE (OFFLINE, READ-ONLY): why does one game map 48 GB and swap 22 GB?

Auto-loop lane. Relaunched until `LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at column 0 of
`reports/research/LANE-HK-MEMORY-PROGRESS.md`.
Repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## The measurement that opened this lane

A live Hollow Knight process under MacRunner, sitting in the opening sequence:

```
RSS            5.0 GB          VSZ           462 GB
Writable regions   total 48.1 GB, written 24.9 GB, resident 5.2 GB, swapped_out 22.0 GB
VM_ALLOCATE        26.7 GB across 1121 regions
VM_ALLOCATE (reserved)  4.0 GB across 8 regions
```

The same game on Windows needs about 2 GB. **We map 48 GB of writable space and push 22 GB
to swap**, and the machine has 460 GB of disk with ~30 GB free, so this is not free.

It has a concrete, blocking consequence, observed in the same run:
`macrunner-hb-guest-alloc-fail: import=VirtualAlloc status=0xc0000018 base=0x672f70000 /
0x6bb800000 / 0x6cb370000 size=0x10000 type=0x3000 protect=0x40` — **19 refusals and
climbing**. `0xc0000018` is STATUS_CONFLICTING_ADDRESSES: Mono asks for a 64 KB
PAGE_EXECUTE_READWRITE segment at a placed base (a fresh JIT code region) and we refuse
because the range is taken. The bases climb, so Mono is retrying upward and losing each time.
The game cannot compile the methods the opening sequence needs, at any speed.

## Your questions

1. **Where do the 48 GB and the 1121 `VM_ALLOCATE` regions come from?** Attribute them to
   code paths: guest heap emulation, the translation cache, region bookkeeping, per-block IR,
   shadow structures, flag emulation state. Give a ranked breakdown with numbers, not
   adjectives.
2. **Why 1121 separate regions rather than a few large ones?** Fragmentation is what makes a
   *placed* `VirtualAlloc` fail even when total free space is huge. Find what allocates in
   small pieces and whether it could allocate in arenas.
3. **What actually occupies `0x672f70000` / `0x6bb800000` / `0x6cb370000`?** These are the
   bases Mono was refused. Determine from the code which of our structures lands in that
   range and whether the placement is deliberate or incidental.
4. **Does our `VirtualAlloc` honour placement the way Windows does?** On Windows a placed
   `MEM_COMMIT|MEM_RESERVE` at a taken base fails, but callers expect a retry-upward to
   eventually succeed. Ours apparently keeps failing. Check `MEM_RESERVE` handling, whether
   we ever return a base we did not reserve, and whether an aligned-region search exists.
5. **Cheapest wins.** Rank concrete reductions by GB saved versus risk. `find_region_normalized`
   showing up at ~5% of samples in a CPU profile suggests the region list is also a *lookup*
   cost, not just a memory cost — say whether it is linear.

## Territory — several agents are live in this repo

- **YOURS to write:** `reports/**`, `tools/**`.
- **READ-ONLY:** `engine/wine/dlls/ntdll/unix/macrunner_hb.c`, `engine/hyperbridge/**`. The
  engine lane is editing them and they carry uncommitted work.
- **DO NOT run `make` in `engine/hyperbridge`** — the engine lane discovered that
  `Makefile.in:8` links `libhyperbridge.a` into `ntdll.so`, so building it even for a unit
  test arms the next wine link for every lane. This is a standing hazard, not a lane rule.
- **FORBIDDEN:** `engine/wine/dlls/winemac.drv/**`, `engine/dxmt/**`.
- **NEVER run Hollow Knight, never launch `scripts/mr-run.sh`, never `pkill`/`killall`.**
- **No commits, no `git add`.**

## Hard gates

- **Numbers, with the command that produced them.** "Large" is not a finding.
- **Quote the allocating code** for every attribution. An attribution with no call site behind
  it is a guess.
- **`not found` != `not there`** — say what you searched.
- Mark every unproven statement `[HYPOTHESIS]`.

## Useful context, already established — do not re-derive

- The interpreter dominates when the direct paths are off: `hb_memory_read` 552,
  `exec_instr_unlocked` 294, `find_region_normalized` 212, `_tlv_get_addr` 190 in a live sample.
- Bisection: of the five `MACRUNNER_HB_JIT_DIRECT_*` knobs only `DIRECT_MEM` faults; the engine
  lane has since proven the cause is an LDP-fusion stale base and fixed it.
- Measured time-to-menu is 2782 s against the Windows oracle's 33.9 s — about 82x.

## Termination

- `LOOP-STATUS: GOAL` — the 48 GB is attributed with numbers, the refused bases are explained,
  and the cheapest reductions are ranked.
- `LOOP-STATUS: BLOCKED` — name in one sentence what only the operator can decide.

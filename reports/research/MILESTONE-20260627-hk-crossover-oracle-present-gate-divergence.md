# MILESTONE 2026-06-27 — HK CrossOver differential oracle: present-gate `esi` divergence (coherence, not control-flow)

**Verdict (evidence, not status):** Hollow Knight runs and **renders** under CrossOver (Wine + Rosetta 2).
At the render-gate `UnityPlayer+0x5872dc` (`cmpl $1,%esi`) CrossOver reads **`esi = 1` → present taken**;
MacRunner reads **`esi = 0` → `jne 0x5874d8`, present skipped, Present=0**. The producer writes the present
flag to the **same item** the consumer reads (item-ptr equality proven), so the divergence is a
**write-visibility / memory-coherence defect on `[item+0x40]`**, NOT a wrong-pointer or control-flow bug.
This is the **same class as the hot-region-cache / direct-mem coherence finding** (`hb_memory.c` hot[]
invalidation + live-VM-write path) — the producer's `[item+0x40]=1` is not visible to the consumer's read.

Oracle = CrossOver `wine` is **x86_64** under Rosetta 2 → runs HK's x64 code. Bottle: `MacRunnerGames`.
Binary = the SAME extracted build the MacRunner lane runs
(`game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight.exe` + `UnityPlayer.dll`,
Unity 2020.3). HK MacRunner lane untouched (Codex mid-trace); golden untouched; MacRunner tree untouched.

---

## Setup / harness

- HK is **not installed inside** the `MacRunnerGames` bottle's `drive_c`; it is reached via the bottle's
  drive mapping `z: -> /` (and `y: -> /Users/timurtoby`) at the extracted path above. Runs as
  `Y:\Documents\MacRunner\Main\game-hollow.knight-(89718)\extracted-…\Hollow Knight.exe`.
- UnityPlayer.dll load base under CrossOver = **`0x6ffffc620000`** (Wine high-DLL zone, NOT the preferred
  `0x180000000`). **Base is deterministic across runs** (verified 2×) → absolute bp = base + RVA.
- Absolute addresses used: gate `0x6ffffcba72dc` (+0x5872dc); producer-write `0x6ffffcba6da3` (+0x586da3);
  present slots `0x6ffffcba7381 / …738a / …73ab` (+0x587381/+0x58738a/+0x5873ab).
- winedbg gotchas (this CX build): the `x/FMT` examine command is rejected ("No symbols found for x") —
  use `print *(long*)(expr)` instead; **conditional breakpoints work** (`condition <n> $rsi == 1`).
- Self-timeout via perl `alarm` (no `timeout`/`sleep` here); cleanup = scoped `"$CX/bin/wineserver" -k`
  on the CrossOver bottle ONLY (never global pkill). The dist-arm64ec-spike MacRunner wineserver stayed
  alive across all runs — isolation verified.

## Behavioral premise — HK renders under CrossOver → **YES**

Window titled **"Hollow Knight"** (System Events), process visible (non-background), sustained **~48% CPU**,
RSS **~1.05 GB flat** at 30 s and 55 s (steady state). (Concurrently, Codex's MacRunner HK ran under
dist-arm64ec-spike at 110–132% CPU / ~1.58 GB — fully separate bottle/wineserver, untouched.)

## Decisive capture — `esi @ 0x5872dc`

Gate disasm (memory, verified on instruction boundary):
```
0x5872dc  cmpl $1, %esi            ; render-gate: present iff esi == 1
0x5872df  jne  0x5874d8            ; esi != 1 -> skip (MacRunner path)
0x5872e7  je   0x5874d8
```
- **Pre-render (seconds after boot):** bp hits with `rsi/esi = 0`, identical context 3× (rax=1,
  r14=rcx=`0x8008a2d0`, r15=`0x30d750a0`, rsp=`0xff0e0`) — the producer hasn't published yet.
- **Steady-state rendering (conditional bp `$rsi==1`):** STOPS with **`rsi = 0x1` → `esi = 1`**, all other
  regs identical to the pre-render capture (only `rsi` flips 0→1). → the gate's input `esi` transitions
  0→1 once the producer publishes; the **item (r14=`0x8008a2d0`) is the same object throughout**.
  (rax=1 is a flag, not the item — `[rax+0x40]`=`0x41` is an invalid address.)

| | CrossOver / Rosetta (renders) | MacRunner / HyperBridge (Present=0) |
|---|---|---|
| `esi` @ `UnityPlayer+0x5872dc` | **1** → fall-through to present | **0** → `jne 0x5874d8`, present skipped |
| item ptr (r14/rcx) | `0x8008a2d0` | (same RVA gate; reads stale `[item+0x40]=0`) |

## Producer-write `0x586da3` — publishes the flag the gate reads (item-ptr equality)

Disasm: **`movl $1, 0x40(%rsi)`** — writes **1** to `[rsi+0x40]`, then signals
(`call 0x5841f0` = semaphore release) and `lock xaddl %ebp, 0x40(%rbx)` (container counter `[rdi+0xd8+0x40]`).
Captured producer items:
- hit 1 — `rsi = 0x8008a2d0`, rdi = `0x30d750a0`  ← **same item + container as the gate**
- hit 2 — `rsi = 0x102486a0` (a different item)
- hit 3 — `rsi = 0x8008a2d0`, rdi = `0x30d750a0`  ← same again

**Item-ptr equality CONFIRMED:** producer `rsi` = gate `r14`/`rcx` = present `r14`/`rcx` = **`0x8008a2d0`**;
container `rdi`/`r15` = **`0x30d750a0`** throughout. The producer publishes `[0x8008a2d0+0x40]=1`; the gate
consumes `esi=[0x8008a2d0+0x40]=1` for the **same** object.

## Present slots reached `0x587381 / 738a / 73ab` — **all hit** (esi=1 → present executes)

```
0x587381  callq *0x68(%rax)        ; present slot 1   (bp HIT, r14=rcx=0x8008a2d0, rsi=1)
0x587384  movq  (%r14), %rax       ; rax = item vtable (=0x6ffffe179530)
0x587387  movq  %r14, %rcx         ; this = item
0x58738a  callq *0x78(%rax)        ; present/draw vtable call on item (bp HIT, r14=rcx=0x8008a2d0)
0x58738d  testb %al,%al ; je 0x5873a5
0x587391  lock decl 0x2f8(%r15)    ; container refcount dec
0x5873a0  callq 0x5841f0           ; semaphore signal
0x5873ab  movq (%r14), %rax …      ; present slot 3   (bp HIT, r14=rcx=0x8008a2d0)
```
Under CrossOver the full chain runs on item `0x8008a2d0`. Under MacRunner `esi=0` → `jne 0x5874d8` →
the present slots are **never reached** → `Present=0`.

## Mechanism (resolved) + Lane-A hand-off

1. **Producer** (`0x586da3`): `mov [item+0x40], 1` on item `0x8008a2d0` + signal semaphore.
2. **Consumer/gate** (`0x5872dc`): `cmp esi,1`, `esi = [item+0x40]`. CrossOver: 1 → present; MacRunner: 0 → skip.
3. **Present** (`0x587381…73ab`): virtual present calls `[vtable+0x68]/[+0x78]` on the item.

**The divergence is data, not control flow.** Same item, same container, same RVA code path — the only
difference is that under MacRunner the consumer reads **0** from `[item+0x40]` although the producer wrote
**1**. This is a **producer→consumer write-visibility / coherence gap on a guest-heap item**, i.e. the
HyperBridge memory-coherence class (hot-region-cache staleness on data-region churn / live-VM-write path —
see `present-stall-diagnosis-20260627.md` and the hot[]-cache UAF review). NOT a DXMT/present-path bug
(present is never reached because the gate already read 0).

**Lane-A verification target (MacRunner side):** at `UnityPlayer+0x5872dc`, dump `[r14+0x40]` (r14 = the
item) and compare to the producer's `[rsi+0x40]` write at `0x586da3` for the same item — under MacRunner
the consumer load returns 0 while the producer stored 1. Confirming the A/B with the coherence toggle
(`MACRUNNER_HB_LIVE_VM_WRITE_MACH=1`) or the hot[]-cache invalidation fix should flip `esi` 0→1 → present.

## Reproduction
```sh
CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
export CX_ROOT="$CX" CX_BOTTLE=MacRunnerGames
export WINEPREFIX="$HOME/Library/Application Support/CrossOver/Bottles/MacRunnerGames"
HKDIR="/Users/timurtoby/Documents/MacRunner/Main/game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620"
cd "$HKDIR"
# base: info share -> unityplayer base 0x6ffffc620000 (stable); abs = base + rva
# gate esi during rendering (conditional):
printf 'break *0x6ffffcba72dc\ncondition 1 $rsi == 1\ncont\ninfo reg\nbt\nquit\n' \
  | "$CX/bin/wine" winedbg "$HKDIR/Hollow Knight.exe"
# producer write: break *0x6ffffcba6da3 ; disas ; cont ; info reg   (movl $1,0x40(%rsi))
# present slots: break *0x6ffffcba7381 / …738a / …73ab
"$CX/bin/wineserver" -k    # scoped cleanup, NEVER global pkill
```
Helper used: scratchpad `hk-winedbg.sh` (perl-alarm self-timeout + scoped `wineserver -k`).
**Rails honored:** CrossOver bottle only; MacRunner HK lane + golden + tree untouched; scoped cleanup;
Codex's dist-arm64ec-spike wineserver verified alive throughout.

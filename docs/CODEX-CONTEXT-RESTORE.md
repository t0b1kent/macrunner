# Codex Context Restore — read FIRST on every session/restart

If Codex session restarted / memory wiped — read this to restore full context.

> ⚠️ ПЕРЕД ВСЕМ ОСТАЛЬНЫМ: прочитай `docs/ACTIVE-INVESTIGATION.md` — там ЖИВОЕ
> состояние текущего бага (CONFIRMED / **DISPROVED — не перепроверять** / current
> boundary / next step). Разделы ниже = постоянная роль/метод; ACTIVE-INVESTIGATION
> = что происходит ПРЯМО СЕЙЧАС. Не повторяй уже опровергнутые гипотезы.
> (Секция «Current mission» ниже может быть устаревшей — ACTIVE-INVESTIGATION главнее.)

## Identity & role

You are **Codex**, primary engine engineer для MacRunner (Windows apps on Apple
Silicon without Rosetta, via own HyperBridge translator + Wine 11 ARM64 fork).

Your territory: engine internals.
- `engine/hyperbridge/` — x86_64→ARM64 translator (decoder, lifter, interpreter, JIT)
- `engine/wine/dlls/ntdll/` — Wine NT layer, cross-arch bridge
- `engine/wine/dlls/kernelbase/`, `win32u/`, `combase/` — Wine subsystems
- Build scripts, smoke harness

Kimi (parallel agent) owns: audio, AOT cache, networking, cloud backend, AI
configurator, engine audits (read-only). Don't duplicate Kimi's work.

## Mandatory reading order (every restart)

1. `/Volumes/MacOS/MacRunner/AGENTS.md` — Zeroth Principle + all mandatory protocols
2. `docs/CODEX-MEGA-DIRECTIVE-v2-batch-method.md` — current method + Notepad++ closure
3. `docs/CODEX-DIRECTIVE-notepad-plus-plus-full-functional.md` — 80 closure criteria
4. `/Users/timurtoby/Documents/MacRunner/90-architectural-discoveries-fundamental-bugs.md` — 9 bugs found
5. `reports/phase-h/MILESTONE/functional-issues.md` — current Notepad++ state
6. `reports/ai-configurator/CODEX-NEXT-TASK.md` — tasks Kimi routed to you

## Current mission (2026-05-20)

**Notepad++ x64 FULL FUNCTIONAL closure.** NOT commercially closed yet.
- Process works, text input works, save dialog opens, file saves, tabs work
- BROKEN: black dialog buttons, gray toolbar icons, black bands, font metrics off
- Hypothesis: GetSysColor returns black → buttons/bands/scroll black (ONE fix)
- NO KeePass until all 80 criteria green

## Method (critical — don't repeat past mistakes)

**Batch-trace, don't iterate one-at-a-time:**
- ONE comprehensive trace run (all categories, limits at 10M+) → collect ALL
  failures → classify into families → batch-fix → ONE rebuild → verify
- Raise any limit decisively to "never hit during legit work" (not increments)
- Use Kimi's Visual Regression Lab (commit 9dc63c7) for automated artifact detection
- Family audit per AGENTS.md before any opcode/API fix

## Bugs closed so far (1-9)

1. RIP-rel + trailing imm | 2. x18 sigreturn | 3. Basic block PC | 4. REX.W MOVD/MOVQ
| 5. 0x66 ALU group | 6. ARM64 callee-saved across callback | 7. Host PROT_EXEC silent exec
| 8. High-8 registers AH/CH/DH/BH | 9. scalar SSE move widths
Plus 10+ SIMD families. Block cap fix for x64-signal-callback.

## Known traps (AGENTS.md has full list)

- Stale `libhyperbridge.a` after source change → rm + rebuild + force ntdll relink
- Stale PE `aarch64-windows/ntdll.dll` after PE source change
- `engine/` directory in .gitignore — engine changes NOT version controlled (infra issue)
- ccache: source config/env.sh so builds use it (5-10x speedup)
- WINEDEBUG=-all suppresses ERR — use fprintf(stderr) env-gated traces
- python3 Homebrew no Quartz — use /usr/bin/python3
- Wine process hygiene: pkill before/after every run

## Build incantation

```bash
cd /Volumes/MacOS/MacRunner
. config/env.sh   # sets CCACHE_DIR, paths

# HyperBridge change:
cd engine/hyperbridge && rm -f src/*.o libhyperbridge.a && make -j4 libhyperbridge.a
cd ../wine/build-pure-arm64 && rm -f dlls/ntdll/ntdll.so && make -j4 dlls/ntdll/ntdll.so install
codesign --force --sign - ../dist-pure-arm64/lib/wine/aarch64-unix/ntdll.so

# Launch Notepad++:
./scripts/run-notepad-x64.sh --duration=60 --trace=faults
```

## Stop criteria

Phase H closes: all 80 criteria green + 3 clean smoke runs + 30-min stable +
clean exit + visual no-artifacts + user sign-off → tag v0.2-...-production →
then Phase I (KeePass 1.43).

## Update this file when

Phase H closes, new fundamental bug found, major method change.
Keep current — survives session restart.

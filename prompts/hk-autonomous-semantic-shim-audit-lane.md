# AUTONOMOUS LANE (OFFLINE, READ-ONLY): audit every semantic shim for fake-success

You are running in an auto-loop. You will be relaunched in fresh threads until you write
`LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at column 0 of
`reports/research/LANE-HK-SHIMAUDIT-PROGRESS.md`. Work continuously; do not wait for a human.

Repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## Why this lane exists — one shim cost us two months

On 2026-07-27 a sister lane proved that `macrunner_hb_try_registry_semantic`
(`engine/wine/dlls/ntdll/unix/macrunner_hb.c`) short-circuited **seven** advapi32 W-registry
imports — `RegOpenKeyExW`, `RegOpenKeyW`, `RegQueryValueExW`, `RegSetValueExW`,
`RegCreateKeyExW`, `RegDeleteValueW`, `RegCloseKey` — handing back synthetic handles from
`0x00006f00f0000000` on which every later call failed. Introduced in `2728b13b` (2026-06-01)
and live ever since.

Consequences: Hollow Knight could not read its saved language, so it showed the language menu
and parked forever on `while (!confirmedLanguage)` — a week of debugging. And because
`RegSetValueExW` was in the list, **registry writes through the W API silently failed for
every title since June**: anything that saved settings was losing them, with no error.

The shim reported success. That is the whole problem: **a stub that returns "fine" is
invisible until something downstream needs the real effect.**

There are **35 distinct `macrunner_hb_try_*_semantic` handlers** in that one file and roughly
**1493 import-name comparisons**. Nobody has audited them as a class. Your job is to.

## What you are looking for

For each shim, decide which of these it is and give evidence:

- **FAKE-SUCCESS (the dangerous class).** Returns a success code, a synthetic handle, a
  constant, or silently swallows the call without performing the real operation. Especially:
  handles minted from a counter, `return TRUE` with no side effect, zeroed out-parameters
  presented as valid data.
- **FAITHFUL.** Genuinely implements the semantic, or forwards to the real implementation.
- **PARTIAL.** Implements some inputs and fakes the rest — often the worst, because the
  working cases hide the broken ones.

Then rank by **blast radius**: how much breaks silently, and how long before anyone notices.
`macrunner_hb_try_security_token_semantic` (`OpenProcessToken` / `OpenThreadToken`) is
already flagged by the registry lane as the same shape — start there, but do not stop there.

## Deliverable

A single ranked table: shim → APIs it intercepts → classification → evidence (quote the
return path) → what breaks if it is wrong → suggested action. Plus a short list of the ones
that should be removed or repaired first, with reasoning.

Do not fix anything. Naming them precisely is the whole job — a sister lane and the operator
decide what gets changed.

## Territory — this is a READ-ONLY lane, three other agents are in this repo

- **YOURS to write:** `reports/**` and `tools/**` only.
- **`engine/wine/dlls/ntdll/unix/macrunner_hb.c` is READ-ONLY for you.** The engine lane is
  editing it live and it carries 700+ uncommitted lines. Do not edit, do not revert, do not
  `git checkout` anything under `engine/`.
- **NEVER run Hollow Knight, never launch `scripts/mr-run.sh`, never `pkill`/`killall`.**
  Another lane owns the single JIT title slot.
- **No commits, no `git add`.**

## Hard gates — each has cost this project a day

- **Quote the return path.** A classification without the actual `return` line and what it
  hands back is a guess. Guesses are what this lane exists to replace.
- **`not found` ≠ `not there`.** If a shim seems unreachable, say how you checked; several
  are reached only through the import-thunk chain, not by direct call.
- **Do not assume the registry shim was the only one of its kind, and do not assume it was
  not.** Both are conclusions that need evidence.
- **Beware `ps | grep -q` under `set -o pipefail`** if you script anything: `grep -q` exits on
  first match, `ps` takes SIGPIPE, `pipefail` propagates it, and the condition reads FALSE
  while the thing you are checking for is very much alive. That misfire deployed a binary
  under a live run today. Count into a variable instead.
- **Mark every unproven statement `[HYPOTHESIS]`.**

## Reporting

- One report per iteration under `reports/phase4-hollow-knight/`, named for the question.
- Append one heartbeat line per step to `reports/research/LANE-HK-SHIMAUDIT-PROGRESS.md`;
  writing a report counts as work.

## Termination

- `LOOP-STATUS: GOAL` — all 35 shims classified with evidence, ranked by blast radius, and
  the fake-success ones named.
- `LOOP-STATUS: BLOCKED` — you need something only the operator can decide. Name it in one
  sentence.
- Otherwise keep going.

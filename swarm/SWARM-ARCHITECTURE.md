# Swarm Architecture — safe parallel discovery + serial engine fix

## Physical constraints (hard limits)
- Project 91GB, engine/wine 12GB, engine/ gitignored
- Disk: ~109GB external free → ONE project copy max, NOT per-agent
- RAM 8GB → max 2-3 concurrent Wine runs, NOT 20-30

## Division of labor

### Kimi swarm (20-30 parallel, batched to fit token limits)
DISCOVERY only — never edit engine code, never run Wine concurrently.
Each agent owns a DISJOINT deliverable (separate output file = no merge conflict):
- Engine audit: one agent per Wine subsystem path (gdi32/comctl32/shell32/font/etc)
- PE analysis: one agent per Windows app (static, lightweight)
- Root-cause investigation: one agent per known bug → file:line + fix scope
- Profile/baseline generation: one agent per app
- Test authoring: separate test files per opcode family

Output → swarm/inbox-for-codex/<task-id>.md (structured: symptom, root cause,
exact file:line, suggested fix scope, confidence).

### Codex (serial, SOLE engine editor)
- Reads swarm/inbox-for-codex/*.md (completed discoveries)
- Fixes engine code bug-by-bug (root cause already found → fast)
- Only Codex touches engine .c files → ZERO merge conflict
- Marks consumed reports done → moves to swarm/reports/done/

### Wine RUNS — serialized queue
Even with swarm, app launches go through one queue (RAM limit).
Kimi MVP orchestrator can queue, but execution is one-at-a-time.

## Why this avoids merge hell
- Swarm produces SEPARATE files (audits, reports, profiles) → no overlap
- Engine edits happen ONLY in Codex (one writer) → no concurrent edits
- Discovery (most of the time) parallelizes; fixing (fast once root known) serializes

## Batch sizing (token limits)
- Launch 20-30 agents per wave (not 300) to stay within token budget
- Each agent = bounded task with clear output spec
- If wave incomplete → next wave picks up remaining (idempotent task list)

## Task partitioning rule
NEVER assign two agents to edit the same file. Discovery agents read shared
tree, write disjoint outputs. This is the ONLY safe parallelization.

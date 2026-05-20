# Engine Change Journal — append-only log of every engine edit

**Purpose**: engine/ is large; only key source files force-tracked in git. This
journal is the **memory** of what changed/why, surviving context compaction.
Codex appends EVERY engine edit here. Never delete entries.

## Format (append newest at top)

```
## YYYY-MM-DD HH:MM — <short title>
File(s): path:line
Type: ROOT-FIX | REVERT | DIAGNOSTIC | WORKAROUND(TODO)
What: [exact change]
Why: [root cause / audit ref]
Verify: [how confirmed — smoke case / screenshot]
Status: applied | reverted | superseded-by-<entry>
```

## Rules
- Log BEFORE rebuild, update Status after verify.
- Mark WORKAROUND only with TODO + link to real root.
- If reverting a prior entry → reference it, mark old as superseded.
- Tracked engine files: `git diff engine/...` shows actual changes. Use it +
  this journal together = full memory.

---

## Entries

(append here)

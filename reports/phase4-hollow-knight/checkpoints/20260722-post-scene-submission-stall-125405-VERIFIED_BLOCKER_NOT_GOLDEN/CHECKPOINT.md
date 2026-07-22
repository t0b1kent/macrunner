# HK Post-Scene Submission Stall Checkpoint

Classification: `VERIFIED_BLOCKER_NOT_GOLDEN`

Machine verdict: `POST_SCENE_SUBMISSION_STALL`

Source run:

`reports/phase4-hollow-knight/laneA-post-scene-first-frame-staged-20260722-125405`

Report:

`reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-FRAME-RESULT.md`

This compact checkpoint anchors the first determined post-scene boundary. The
managed language sequence completed, all three managers were created, and
`Performing automatic level start.` was reached. Before that marker the run had
`GetBuffer=1` and `Present/Present1=48/48`. During the following 352 seconds the
same live process and window produced no `GetBuffer`, `Present`, `Present1`,
draw, or encoder event. Five captures remained byte-identical black frames, and
no unsupported opcode, memory fault, reject, or HUP was recorded.

This is a verified blocker floor, not a visible-pixel milestone. It means the
next investigation belongs at the post-scene Unity/native submission boundary;
shader work is not yet authorized because no post-scene frame is submitted.

The checkpoint intentionally excludes full `run.log`, raw `final-child.json`,
shader translation dumps, staged runtime trees, prefixes, and translation-cache
bytes.
